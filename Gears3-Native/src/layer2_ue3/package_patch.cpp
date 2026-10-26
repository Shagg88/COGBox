// Layer 2 (Unreal Engine 3): script package byte patches (ue3/package_patch.h, notes/25). Our code (MIT), including
// the LZX decoder (lzx.cpp).
#include <ue3/config_bundle.h>
#include <ue3/package_patch.h>

#include <cstring>
#include <format>
#include <sstream>

namespace ue3 {
namespace {
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool ParseHex(const std::string& s, std::vector<uint8_t>& out) {
  out.clear();
  std::string h;
  for (char c : s)
    if (c != ' ') h += c;
  if (h.empty() || h.size() % 2) return false;
  for (size_t i = 0; i < h.size(); i += 2) {
    char* end = nullptr;
    const std::string byte = h.substr(i, 2);
    const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
    if (end != byte.c_str() + 2) return false;
    out.push_back(uint8_t(v));
  }
  return true;
}
std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s += std::format("{:02x}", p[i]);
  return s;
}
}  // namespace

std::vector<PackagePatch> ParsePackagePatches(const std::string& spec, std::string* error) {
  std::vector<PackagePatch> out;
  std::stringstream items(spec);
  std::string item;
  while (std::getline(items, item, ';')) {
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (item.empty()) continue;
    PackagePatch p;
    const size_t hash = item.find('#');
    if (hash != std::string::npos) {
      p.note = item.substr(hash + 1);
      item.resize(hash);
    }
    const size_t at = item.find('@'), colon = item.find(':', at == std::string::npos ? 0 : at),
                 arrow = item.find('>', colon == std::string::npos ? 0 : colon);
    bool ok = at != std::string::npos && colon != std::string::npos && arrow != std::string::npos && at > 0;
    if (ok) {
      p.file = item.substr(0, at);
      const std::string off = item.substr(at + 1, colon - at - 1);
      char* end = nullptr;
      p.offset = std::strtoull(off.c_str(), &end, 0);
      ok = !off.empty() && end == off.c_str() + off.size() && ParseHex(item.substr(colon + 1, arrow - colon - 1), p.expect) &&
           ParseHex(item.substr(arrow + 1), p.replace) && p.expect.size() == p.replace.size();
    }
    if (!ok) {
      if (error) *error += (error->empty() ? "" : "; ") + std::string("can't read package patch '") + item + "'";
      continue;
    }
    out.push_back(std::move(p));
  }
  return out;
}

bool IsFullyCompressed(const std::vector<uint8_t>& in) {
  return in.size() >= 16 && Be32(in.data()) == 0x9E2A83C1u && Be32(in.data() + 4) == 0x20000u;
}

bool DecompressFully(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* error) {
  out.clear();
  if (!IsFullyCompressed(in)) return false;
  const uint32_t block_size = Be32(in.data() + 4), total = Be32(in.data() + 12);
  const uint64_t blocks = (uint64_t(total) + block_size - 1) / block_size;
  if (16 + 8 * blocks > in.size()) {
    if (error) *error = "block table past the end";
    return false;
  }
  out.resize(total);
  const uint8_t* table = in.data() + 16;
  const uint8_t* data = table + 8 * blocks;
  const uint8_t* const in_end = in.data() + in.size();
  size_t done = 0;
  std::vector<uint8_t> raw;
  for (uint64_t i = 0; i < blocks; ++i) {
    const uint32_t cs = Be32(table + 8 * i), us = Be32(table + 8 * i + 4);
    if (data + cs > in_end || done + us > out.size()) {
      if (error) *error = std::format("block {} past the end", i);
      out.clear();
      return false;
    }
    // XMemCompress frames: a 2-byte big-endian compressed size per 32 KB frame, or 0xFF + 2-byte uncompressed size +
    // 2-byte compressed size for a shorter one; a zero size ends the block. The LZX decoder wants the frames joined.
    raw.clear();
    const uint8_t* q = data;
    const uint8_t* const qe = data + cs;
    while (q + 2 <= qe) {
      uint32_t c;
      if (q[0] == 0xFF) {
        if (q + 5 > qe) break;
        c = uint32_t(q[3]) << 8 | q[4];
        q += 5;
      } else {
        c = uint32_t(q[0]) << 8 | q[1];
        q += 2;
      }
      if (c == 0 || q + c > qe) break;
      raw.insert(raw.end(), q, q + c);
      q += c;
    }
    if (!LzxDecompress(raw.data(), raw.size(), out.data() + done, us, 17)) {
      if (error) *error = std::format("block {}: LZX error", i);
      out.clear();
      return false;
    }
    data += cs;
    done += us;
  }
  if (done != total) {
    if (error) *error = std::format("{} of {} bytes", done, total);
    out.clear();
    return false;
  }
  return true;
}

int ApplyPackagePatches(std::vector<uint8_t>& data, const std::string& file, const std::vector<PackagePatch>& patches,
                        std::vector<std::string>* log) {
  const std::string want = AsciiLower(file);
  std::vector<const PackagePatch*> mine;
  for (const PackagePatch& p : patches)
    if (AsciiLower(p.file) == want) mine.push_back(&p);
  for (const PackagePatch* p : mine) {  // check every one first: all or nothing
    if (p->offset + p->expect.size() > data.size() ||
        std::memcmp(data.data() + p->offset, p->expect.data(), p->expect.size()) != 0) {
      if (log) {
        const size_t n = p->offset < data.size() ? std::min(p->expect.size(), size_t(data.size() - p->offset)) : 0;
        log->push_back(std::format("{} @0x{:X}: expected {}, found {} - no patch applied to this file (another game "
                                   "version?)",
                                   file, p->offset, Hex(p->expect.data(), p->expect.size()),
                                   n ? Hex(data.data() + p->offset, n) : std::string("(past the end)")));
      }
      return -1;
    }
  }
  for (const PackagePatch* p : mine) {
    std::memcpy(data.data() + p->offset, p->replace.data(), p->replace.size());
    if (log)
      log->push_back(std::format("{} @0x{:X}: {} -> {}{}{}", file, p->offset, Hex(p->expect.data(), p->expect.size()),
                                 Hex(p->replace.data(), p->replace.size()), p->note.empty() ? "" : " - ", p->note));
  }
  return int(mine.size());
}

}  // namespace ue3
