// Layer 2 (Unreal Engine 3, Gears 1): script package byte patches (ue3/package_patch.h; GitHub #420, parse / apply as
// Gears 2's package_patch.cpp, the LZO1X decoder as Gears 2's shader_prebuild.cpp). Our code (MIT).
#include <ue3/package_patch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <sstream>

namespace ue3 {
namespace {
constexpr uint32_t kTag = 0x9E2A83C1u, kBlock = 0x20000;

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutLe32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
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
std::string AsciiLower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
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

int ApplyPackagePatches(std::vector<uint8_t>& data, const std::string& file, const std::vector<PackagePatch>& patches,
                        std::vector<std::string>* log) {
  const std::string want = AsciiLower(file);
  std::vector<const PackagePatch*> mine;
  for (const PackagePatch& p : patches)
    if (AsciiLower(p.file) == want) mine.push_back(&p);
  for (const PackagePatch* p : mine) {  // check every one first: all or nothing
    if (p->offset > data.size() || p->expect.size() > data.size() - p->offset ||
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

// Literal runs, M1-M4 matches, trailing literals in the low 2 bits; bounds-checked, exactly out_len bytes.
bool Lzo1xDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
  size_t ip = 0, op = 0;
  auto need = [&](size_t k) { return ip + k <= in_len; };
  auto lit = [&](size_t k) {
    if (!need(k) || op + k > out_len) return false;
    std::memcpy(out + op, in + ip, k);
    ip += k;
    op += k;
    return true;
  };
  auto copy = [&](size_t dist, size_t len) {
    if (dist == 0 || dist > op || op + len > out_len) return false;
    for (size_t k = 0; k < len; ++k, ++op) out[op] = out[op - dist];
    return true;
  };
  auto run = [&](size_t base, size_t& len) {   // a zero-extended length
    len = base;
    while (need(1) && in[ip] == 0) { len += 255; ++ip; }
    if (!need(1)) return false;
    len += in[ip++];
    return true;
  };
  if (!need(1)) return false;
  int state = 0, t = -1;
  if (in[0] > 17) {
    ip = 1;
    const size_t k = in[0] - 17u;
    if (!lit(k)) return false;
    state = k >= 4 ? 4 : int(k);
  }
  for (;;) {
    if (t < 0) { if (!need(1)) return false; t = in[ip++]; }
    if (t < 16) {
      if (state == 0) {   // a literal run
        size_t len = size_t(t);
        if (t == 0 && !run(15, len)) return false;
        if (!lit(len + 3)) return false;
        state = 4;
        if (!need(1)) return false;
        t = in[ip++];
        if (t >= 16) continue;
        if (!need(1)) return false;
        const size_t dist = 1 + 0x0800 + (size_t(t) >> 2) + (size_t(in[ip++]) << 2);
        if (!copy(dist, 3)) return false;
      } else {
        if (!need(1)) return false;
        const size_t dist = (state < 4 ? 1 : 1 + 0x0800) + (size_t(t) >> 2) + (size_t(in[ip++]) << 2);
        if (!copy(dist, state < 4 ? 2 : 3)) return false;
      }
      state = t & 3;
      if (!lit(size_t(state))) return false;
      t = -1;
      continue;
    }
    size_t len, dist;
    int trailing;
    if (t >= 64) {
      if (!need(1)) return false;
      dist = 1 + ((size_t(t) >> 2) & 7) + (size_t(in[ip++]) << 3);
      len = (size_t(t) >> 5) + 1;
      trailing = t & 3;
    } else if (t >= 32) {
      len = size_t(t) & 31;
      if (len == 0 && !run(31, len)) return false;
      len += 2;
      if (!need(2)) return false;
      const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
      ip += 2;
      dist = 1 + (d >> 2);
      trailing = int(d & 3);
    } else {   // 16..31
      len = size_t(t) & 7;
      if (len == 0 && !run(7, len)) return false;
      len += 2;
      if (!need(2)) return false;
      const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
      ip += 2;
      dist = ((size_t(t) & 8) << 11) + (d >> 2);
      if (dist == 0) return op == out_len;   // end of stream
      dist += 0x4000;
      trailing = int(d & 3);
    }
    if (!copy(dist, len)) return false;
    state = trailing;
    if (!lit(size_t(trailing))) return false;
    t = -1;
  }
}

std::vector<uint8_t> Lzo1xStore(const uint8_t* in, size_t len) {
  std::vector<uint8_t> o;
  if (len == 0) return {0x11, 0x00, 0x00};
  if (len <= 238) {
    o.push_back(uint8_t(17 + len));   // the first byte's short literal run
  } else {
    o.push_back(0);   // a literal run of 18 + zeros*255 + last (last 1..255)
    const size_t r = len - 18, zeros = (r - 1) / 255;
    o.insert(o.end(), zeros, 0);
    o.push_back(uint8_t(r - zeros * 255));
  }
  o.insert(o.end(), in, in + len);
  o.push_back(0x11);   // end of stream (M4, distance 0)
  o.push_back(0x00);
  o.push_back(0x00);
  return o;
}

uint64_t LegacyCompressedTotal(const uint8_t* head16) {
  return Le32(head16) == kTag && Le32(head16 + 4) == kTag ? Le32(head16 + 12) : 0;
}

int PatchCompressedPackage(const std::vector<uint8_t>& in, const std::string& file, const std::vector<PackagePatch>& patches,
                           std::vector<uint8_t>& out, std::vector<std::string>* log, std::string* error) {
  out.clear();
  const std::string want = AsciiLower(file);
  std::vector<PackagePatch> mine;
  for (const PackagePatch& p : patches)
    if (AsciiLower(p.file) == want) mine.push_back(p);
  if (mine.empty()) return 0;
  auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    out.clear();
    return -1;
  };
  if (in.size() < 16 || Le32(in.data()) != kTag || Le32(in.data() + 4) != kTag) return fail("not a Gears 1 compressed package");
  const uint32_t tc = Le32(in.data() + 8), tu = Le32(in.data() + 12);
  const uint64_t blocks = (uint64_t(tu) + kBlock - 1) / kBlock;
  const uint64_t data0 = 16 + 8 * blocks;
  if (data0 + tc > in.size()) return fail("block table or blocks past the end");
  std::vector<uint64_t> at(blocks + 1);
  at[0] = data0;
  uint64_t sumu = 0;
  for (uint64_t i = 0; i < blocks; ++i) {
    const uint32_t cs = Le32(&in[16 + 8 * i]), us = Le32(&in[20 + 8 * i]);
    if (us > kBlock || (i + 1 < blocks && us != kBlock)) return fail(std::format("block {}: size {}", i, us));
    at[i + 1] = at[i] + cs;
    sumu += us;
  }
  if (at[blocks] != data0 + tc || sumu != tu) return fail("block sizes don't add up to the header's totals");
  // Decode every block a patch touches into one plain image at its real offsets (other blocks stay zero, unused).
  std::map<uint64_t, std::vector<uint8_t>> touched;
  for (const PackagePatch& p : mine) {
    if (p.offset + p.expect.size() > tu) return fail(std::format("{} @0x{:X}: past the end ({} bytes)", file, p.offset, tu));
    for (uint64_t b = p.offset / kBlock; b <= (p.offset + p.expect.size() - 1) / kBlock; ++b) touched[b];
  }
  std::vector<uint8_t> plain(tu);
  for (auto& [b, unused] : touched) {
    const uint32_t us = Le32(&in[20 + 8 * b]);
    if (!Lzo1xDecompress(&in[at[b]], size_t(at[b + 1] - at[b]), &plain[b * kBlock], us))
      return fail(std::format("block {}: LZO error", b));
  }
  const int applied = ApplyPackagePatches(plain, file, mine, log);
  if (applied < 0) return fail("a patched byte isn't what it should be (another game version?)");
  for (auto& [b, enc] : touched) enc = Lzo1xStore(&plain[b * kBlock], Le32(&in[20 + 8 * b]));
  // header, table, blocks (the touched ones stored again), zeros to 32 KB as the disc file
  uint64_t new_tc = 0;
  for (uint64_t b = 0; b < blocks; ++b) new_tc += touched.count(b) ? touched[b].size() : at[b + 1] - at[b];
  PutLe32(out, kTag);
  PutLe32(out, kTag);
  PutLe32(out, uint32_t(new_tc));
  PutLe32(out, tu);
  for (uint64_t b = 0; b < blocks; ++b) {
    PutLe32(out, uint32_t(touched.count(b) ? touched[b].size() : at[b + 1] - at[b]));
    PutLe32(out, Le32(&in[20 + 8 * b]));
  }
  for (uint64_t b = 0; b < blocks; ++b) {
    if (touched.count(b)) out.insert(out.end(), touched[b].begin(), touched[b].end());
    else out.insert(out.end(), in.begin() + ptrdiff_t(at[b]), in.begin() + ptrdiff_t(at[b + 1]));
  }
  out.resize((out.size() + 0x7FFF) / 0x8000 * 0x8000, 0);
  return applied;
}

}  // namespace ue3
