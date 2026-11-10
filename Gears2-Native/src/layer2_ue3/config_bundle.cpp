// Layer 2 (Unreal Engine 3): parse / write / edit the cooked config bundle (ue3/config_bundle.h). Standard library
// only, so the unit tests build it on its own. Our code (MIT).
#include <ue3/config_bundle.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>

namespace ue3 {
namespace {

struct Reader {
  const std::vector<uint8_t>& b;
  size_t o = 0;
  bool ok = true;
  int32_t I32() {
    if (o + 4 > b.size()) { ok = false; return 0; }
    const uint32_t v = uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3];
    o += 4;
    return int32_t(v);
  }
  ConfigString Str() {
    ConfigString s;
    const int32_t n = I32();
    if (!ok) return s;
    if (n == 0) { s.present = false; s.wide = false; return s; }
    if (n < 0) {
      const size_t chars = size_t(-int64_t(n));
      if (o + chars * 2 > b.size() || chars > (1u << 24)) { ok = false; return s; }
      s.wide = true;
      for (size_t i = 0; i < chars; ++i) s.text.push_back(char16_t(b[o + 2 * i] | (b[o + 2 * i + 1] << 8)));
      o += chars * 2;
    } else {
      const size_t chars = size_t(n);
      if (o + chars > b.size()) { ok = false; return s; }
      s.wide = false;
      for (size_t i = 0; i < chars; ++i) s.text.push_back(char16_t(b[o + i]));
      o += chars;
    }
    if (!s.text.empty() && s.text.back() == 0) s.text.pop_back();  // the terminator
    return s;
  }
};

void PutI32(std::vector<uint8_t>& v, int32_t x) {
  const uint32_t u = uint32_t(x);
  v.push_back(uint8_t(u >> 24)); v.push_back(uint8_t(u >> 16)); v.push_back(uint8_t(u >> 8)); v.push_back(uint8_t(u));
}
void PutStr(std::vector<uint8_t>& v, const ConfigString& s) {
  if (!s.present) { PutI32(v, 0); return; }
  const int32_t n = int32_t(s.text.size() + 1);
  if (s.wide) {
    PutI32(v, -n);
    for (char16_t c : s.text) { v.push_back(uint8_t(c)); v.push_back(uint8_t(c >> 8)); }
    v.push_back(0); v.push_back(0);
  } else {
    PutI32(v, n);
    for (char16_t c : s.text) v.push_back(uint8_t(c));
    v.push_back(0);
  }
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}
bool EndsWithCase(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return Lower(s.substr(s.size() - suffix.size())) == Lower(suffix);
}
std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  return s.substr(i);
}

}  // namespace

std::string ConfigString::Utf8() const {
  std::string out;
  for (char16_t c : text) {
    if (c < 0x80) out.push_back(char(c));
    else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
    else { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
  }
  return out;
}

ConfigString ConfigString::FromUtf8(const std::string& s, bool wide) {
  ConfigString o;
  o.wide = wide;
  for (size_t i = 0; i < s.size();) {  // edits are ASCII in practice; decode 1-3 byte sequences
    const uint8_t c = uint8_t(s[i]);
    if (c < 0x80) { o.text.push_back(c); i += 1; }
    else if ((c >> 5) == 6 && i + 1 < s.size()) { o.text.push_back(char16_t((c & 0x1F) << 6 | (s[i + 1] & 0x3F))); i += 2; }
    else if (i + 2 < s.size()) { o.text.push_back(char16_t((c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F))); i += 3; }
    else break;
  }
  return o;
}

bool ParseConfigBundle(const std::vector<uint8_t>& bytes, ConfigBundle& out, std::string* error) {
  Reader r{bytes};
  out.files.clear();
  const int32_t files = r.I32();
  if (!r.ok || files < 0 || files > 100000) { if (error) *error = "bad file count"; return false; }
  for (int32_t f = 0; f < files && r.ok; ++f) {
    ConfigFile& file = out.files.emplace_back();
    file.name = r.Str();
    const int32_t sections = r.I32();
    if (sections < 0 || sections > 1000000) { r.ok = false; break; }
    for (int32_t s = 0; s < sections && r.ok; ++s) {
      ConfigSection& sec = file.sections.emplace_back();
      sec.name = r.Str();
      const int32_t keys = r.I32();
      if (keys < 0 || keys > 1000000) { r.ok = false; break; }
      for (int32_t k = 0; k < keys && r.ok; ++k) {
        ConfigKey& key = sec.keys.emplace_back();
        key.key = r.Str();
        key.value = r.Str();
      }
    }
  }
  if (!r.ok) { if (error) *error = std::format("truncated or malformed at byte {}", r.o); return false; }
  if (r.o != bytes.size()) { if (error) *error = std::format("{} bytes left over after {} files", bytes.size() - r.o, files); return false; }
  return true;
}

std::vector<uint8_t> WriteConfigBundle(const ConfigBundle& bundle) {
  std::vector<uint8_t> v;
  PutI32(v, int32_t(bundle.files.size()));
  for (const ConfigFile& f : bundle.files) {
    PutStr(v, f.name);
    PutI32(v, int32_t(f.sections.size()));
    for (const ConfigSection& s : f.sections) {
      PutStr(v, s.name);
      PutI32(v, int32_t(s.keys.size()));
      for (const ConfigKey& k : s.keys) {
        PutStr(v, k.key);
        PutStr(v, k.value);
      }
    }
  }
  return v;
}

std::vector<ConfigEdit> ParseConfigEdits(const std::string& spec, std::string* error) {
  std::vector<ConfigEdit> out;
  std::stringstream all(spec);
  std::string item;
  while (std::getline(all, item, ';')) {
    item = Trim(item);
    if (item.empty()) continue;
    const size_t p1 = item.find('|'), p2 = p1 == std::string::npos ? p1 : item.find('|', p1 + 1);
    const size_t eq = p2 == std::string::npos ? p2 : item.find('=', p2 + 1);
    if (eq == std::string::npos) {
      if (error) *error += std::format("'{}': expected file|section|key=value; ", item);
      continue;
    }
    ConfigEdit e;
    e.file = Trim(item.substr(0, p1));
    e.section = Trim(item.substr(p1 + 1, p2 - p1 - 1));
    std::string key = item.substr(p2 + 1, eq - p2 - 1);
    e.value = item.substr(eq + 1);
    if (Trim(key) == "~") {  // patch 0127: replace text inside every value of the section
      const size_t gt = e.value.find('>');
      if (gt == std::string::npos || gt == 0) {
        if (error) *error += std::format("'{}': expected ~=old>new; ", item);
        continue;
      }
      e.replace = true;
      e.replace_old = e.value.substr(0, gt);
      e.replace_new = e.value.substr(gt + 1);
      e.key = "~";
      if (e.file.empty() || e.section.empty()) {
        if (error) *error += std::format("'{}': empty file or section; ", item);
        continue;
      }
      out.push_back(e);
      continue;
    }
    if (!key.empty() && Trim(key).front() == '-') {  // patch 0127: remove one entry of a repeated key
      key = Trim(key).substr(1);
      e.remove = true;
    } else if (!key.empty() && key.back() == '*') {
      key.pop_back();
      e.scale = true;
      char* end = nullptr;
      e.factor = std::strtod(e.value.c_str(), &end);
      if (end == e.value.c_str()) {
        if (error) *error += std::format("'{}': the factor is not a number; ", item);
        continue;
      }
    }
    e.key = Trim(key);
    if (e.file.empty() || e.section.empty() || e.key.empty()) {
      if (error) *error += std::format("'{}': empty file, section or key; ", item);
      continue;
    }
    out.push_back(e);
  }
  return out;
}

int ApplyConfigEdits(ConfigBundle& bundle, const std::vector<ConfigEdit>& edits, std::vector<std::string>* log) {
  int changed = 0;
  for (const ConfigEdit& e : edits) {
    const std::string lkey = Lower(e.key);
    for (ConfigFile& f : bundle.files) {
      const std::string fname = f.name.Utf8();
      if (!EndsWithCase(fname, e.file)) continue;
      bool section_seen = false;
      for (ConfigSection& s : f.sections) {
        const bool sec_match = e.section == "*" || Lower(s.name.Utf8()) == Lower(e.section);
        if (!sec_match) continue;
        if (e.remove) {  // patch 0127: drop the entries of the key with that value; nothing is added
          const std::string lvalue = Lower(Trim(e.value));
          const size_t before = s.keys.size();
          std::erase_if(s.keys, [&](const ConfigKey& k) {
            return Lower(k.key.Utf8()) == lkey && Lower(Trim(k.value.Utf8())) == lvalue;
          });
          const size_t gone = before - s.keys.size();
          changed += int(gone);
          if (gone && log)
            log->push_back(std::format("{} [{}] {}: removed '{}' ({}x)", fname, s.name.Utf8(), e.key, e.value, gone));
          section_seen = true;
          continue;
        }
        if (e.replace) {  // patch 0127: text replaced inside every value of the section; nothing is added
          for (ConfigKey& k : s.keys) {
            const std::string before = k.value.Utf8();
            std::string after = before;
            for (size_t at = 0; (at = after.find(e.replace_old, at)) != std::string::npos; at += e.replace_new.size())
              after.replace(at, e.replace_old.size(), e.replace_new);
            if (after == before) continue;
            k.value = ConfigString::FromUtf8(after, k.value.wide || !k.value.present);
            ++changed;
            if (log) log->push_back(std::format("{} [{}] {}: '{}' -> '{}'", fname, s.name.Utf8(), k.key.Utf8(), before, after));
          }
          section_seen = true;
          continue;
        }
        bool key_seen = false;
        for (ConfigKey& k : s.keys) {
          if (Lower(k.key.Utf8()) != lkey) continue;
          key_seen = true;
          const std::string before = k.value.Utf8();
          std::string after;
          if (e.scale) {
            char* end = nullptr;
            const double v = std::strtod(before.c_str(), &end);
            if (end == before.c_str()) continue;  // not a number: left alone
            char num[64];
            std::snprintf(num, sizeof(num), "%g", v * e.factor);
            after = std::string(num) + std::string(end);
          } else {
            after = e.value;
          }
          if (after == before) continue;
          k.value = ConfigString::FromUtf8(after, k.value.wide || !k.value.present);
          k.value.present = true;
          ++changed;
          if (log) log->push_back(std::format("{} [{}] {}: '{}' -> '{}'", fname, s.name.Utf8(), k.key.Utf8(), before, after));
        }
        if (e.section != "*") {
          section_seen = true;
          if (!key_seen && !e.scale) {
            ConfigKey k;
            k.key = ConfigString::FromUtf8(e.key, true);
            k.value = ConfigString::FromUtf8(e.value, true);
            s.keys.push_back(k);
            ++changed;
            if (log) log->push_back(std::format("{} [{}] {}: added '{}'", fname, s.name.Utf8(), e.key, e.value));
          }
        }
      }
      if (e.section != "*" && !section_seen && !e.scale && !e.remove && !e.replace) {
        ConfigSection s;
        s.name = ConfigString::FromUtf8(e.section, true);
        ConfigKey k;
        k.key = ConfigString::FromUtf8(e.key, true);
        k.value = ConfigString::FromUtf8(e.value, true);
        s.keys.push_back(k);
        f.sections.push_back(s);
        ++changed;
        if (log) log->push_back(std::format("{} [{}] {}: added (new section) '{}'", fname, e.section, e.key, e.value));
      }
    }
  }
  return changed;
}

Sha1Digest Sha1(const uint8_t* data, size_t size) {  // FIPS 180-4
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  auto block = [&](const uint8_t* b) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999u;
      else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1u;
      else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDCu;
      else f = bb ^ c ^ d, k = 0xCA62C1D6u;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(bb, 30), bb = a, a = t;
    }
    h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
  };
  size_t i = 0;
  for (; i + 64 <= size; i += 64) block(data + i);
  uint8_t tail[128] = {};
  const size_t rest = size - i;
  if (rest) std::memcpy(tail, data + i, rest);
  tail[rest] = 0x80;
  const size_t tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
  const uint64_t bits = uint64_t(size) * 8;
  for (int k = 0; k < 8; ++k) tail[tail_len - 1 - k] = uint8_t(bits >> (8 * k));
  block(tail);
  if (tail_len == 128) block(tail + 64);
  Sha1Digest out;
  for (int k = 0; k < 5; ++k)
    for (int j = 0; j < 4; ++j) out[4 * k + j] = uint8_t(h[k] >> (24 - 8 * j));
  return out;
}

std::vector<size_t> FindFileHash(const uint8_t* data, size_t size, const std::string& name, const Sha1Digest& hash) {
  std::vector<size_t> out;
  if (name.empty() || size < name.size() + 1 + hash.size()) return out;
  const size_t n = name.size();
  for (size_t i = 0; i + n + 1 + hash.size() <= size; ++i) {
    if (data[i] != uint8_t(name[0]) || std::memcmp(data + i, name.data(), n) != 0 || data[i + n] != 0) continue;
    // The name's hash must also match the file we read: nothing else in an image is followed by that.
    if (std::memcmp(data + i + n + 1, hash.data(), hash.size()) == 0) out.push_back(i + n + 1);
  }
  return out;
}

}  // namespace ue3
