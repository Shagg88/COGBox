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

bool EndsWithCase(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return AsciiLower(s.substr(s.size() - suffix.size())) == AsciiLower(suffix);
}
std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  return s.substr(i);
}

// The message padding SHA-1 and MD5 share: the 64-byte blocks, then 0x80, zeros and the bit length in the last 8 bytes
// of one or two final blocks (big-endian for SHA-1, little-endian for MD5).
template <typename Block>
void HashBlocks(const uint8_t* data, size_t size, bool big_endian_length, Block&& block) {
  size_t i = 0;
  for (; i + 64 <= size; i += 64) block(data + i);
  uint8_t tail[128] = {};
  const size_t rest = size - i;
  if (rest) std::memcpy(tail, data + i, rest);
  tail[rest] = 0x80;
  const size_t tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
  const uint64_t bits = uint64_t(size) * 8;
  for (int k = 0; k < 8; ++k)
    tail[big_endian_length ? tail_len - 1 - k : tail_len - 8 + k] = uint8_t(bits >> (8 * k));
  block(tail);
  if (tail_len == 128) block(tail + 64);
}

}  // namespace

std::string AsciiLower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

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

namespace {
void FlatSections(const ConfigString& text, std::vector<ConfigSection>& out) {
  const std::string t = text.Utf8();
  ConfigSection* cur = nullptr;
  size_t p = 0;
  while (p < t.size()) {
    size_t nl = t.find('\n', p);
    if (nl == std::string::npos) nl = t.size();
    std::string line = t.substr(p, nl - p);
    p = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string tl = Trim(line);
    if (tl.size() >= 2 && tl.front() == '[' && tl.back() == ']') {
      cur = &out.emplace_back();
      cur->name = ConfigString::FromUtf8(tl.substr(1, tl.size() - 2), text.wide);
      continue;
    }
    const size_t eq = line.find('=');
    if (!cur || eq == std::string::npos || tl.empty() || tl[0] == ';') continue;
    ConfigKey& k = cur->keys.emplace_back();
    k.key = ConfigString::FromUtf8(Trim(line.substr(0, eq)), text.wide);
    k.value = ConfigString::FromUtf8(line.substr(eq + 1), text.wide);
  }
}
bool SameStr(const ConfigString& a, const ConfigString& b) { return a.text == b.text; }
bool SameSections(const std::vector<ConfigSection>& a, const std::vector<ConfigSection>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!SameStr(a[i].name, b[i].name) || a[i].keys.size() != b[i].keys.size()) return false;
    for (size_t k = 0; k < a[i].keys.size(); ++k)
      if (!SameStr(a[i].keys[k].key, b[i].keys[k].key) || !SameStr(a[i].keys[k].value, b[i].keys[k].value)) return false;
  }
  return true;
}
}  // namespace

bool ParseFlatConfigBundle(const std::vector<uint8_t>& bytes, ConfigBundle& out, std::vector<ConfigString>& texts,
                           std::string* error) {
  Reader r{bytes};
  out.files.clear();
  texts.clear();
  int32_t files = r.I32();
  if (!r.ok || files < 0 || files > 100000) { if (error) *error = "bad file count"; return false; }
  out.count_strings = files % 2 == 0;   // Gears 1 counts strings (name + text per file); try that first
  if (out.count_strings) files /= 2;
  for (int32_t f = 0; f < files && r.ok; ++f) {
    ConfigFile& file = out.files.emplace_back();
    file.name = r.Str();
    texts.push_back(r.Str());
    if (r.ok) FlatSections(texts.back(), file.sections);
  }
  if (!r.ok) { if (error) *error = std::format("truncated or malformed at byte {}", r.o); return false; }
  if (r.o != bytes.size()) { if (error) *error = std::format("{} bytes left over after {} files", bytes.size() - r.o, files); return false; }
  return true;
}

std::vector<uint8_t> WriteFlatConfigBundle(const ConfigBundle& bundle, const std::vector<ConfigString>& texts) {
  std::vector<uint8_t> v;
  PutI32(v, int32_t(bundle.files.size()) * (bundle.count_strings ? 2 : 1));
  for (size_t i = 0; i < bundle.files.size(); ++i) {
    const ConfigFile& f = bundle.files[i];
    PutStr(v, f.name);
    std::vector<ConfigSection> orig;
    const bool have = i < texts.size();
    if (have) FlatSections(texts[i], orig);
    if (have && SameSections(orig, f.sections)) { PutStr(v, texts[i]); continue; }
    std::string t;
    for (const ConfigSection& s : f.sections) {
      t += "[" + s.name.Utf8() + "]\r\n";
      for (const ConfigKey& k : s.keys) t += k.key.Utf8() + "=" + k.value.Utf8() + "\r\n";
      t += "\r\n";
    }
    PutStr(v, ConfigString::FromUtf8(t, have ? texts[i].wide : false));
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
    if (Trim(key) == "~") {  // Gears 2's patch 0127: replace text inside every value of the section
      const size_t gt = e.value.find('>');
      if (gt == std::string::npos || gt == 0 || e.file.empty() || e.section.empty()) {
        if (error) *error += std::format("'{}': expected file|section|~=old>new; ", item);
        continue;
      }
      e.replace = true;
      e.replace_old = e.value.substr(0, gt);
      e.replace_new = e.value.substr(gt + 1);
      e.key = "~";
      out.push_back(e);
      continue;
    }
    if (!key.empty() && key.front() == '-') {  // "-Key=Value": Gears 3's spelling (d5bf473) of Key-=Value
      e.remove = true;
      key.erase(0, 1);
    } else if (!key.empty() && (key.back() == '+' || key.back() == '-')) {  // array edits (config_bundle.h)
      (key.back() == '+' ? e.append : e.remove) = true;
      key.pop_back();
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
    const std::string lkey = AsciiLower(e.key), lsection = AsciiLower(e.section);
    if (e.replace) {  // nothing is added
      for (ConfigFile& f : bundle.files) {
        const std::string fname = f.name.Utf8();
        if (!EndsWithCase(fname, e.file)) continue;
        for (ConfigSection& s : f.sections) {
          if (e.section != "*" && AsciiLower(s.name.Utf8()) != lsection) continue;
          for (ConfigKey& k : s.keys) {
            const std::string before = k.value.Utf8();
            std::string after = before;
            for (size_t at = 0; (at = after.find(e.replace_old, at)) != std::string::npos; at += e.replace_new.size())
              after.replace(at, e.replace_old.size(), e.replace_new);
            if (after == before) continue;
            k.value = ConfigString::FromUtf8(after, k.value.wide);
            ++changed;
            if (log) log->push_back(std::format("{} [{}] {}: '{}' -> '{}'", fname, s.name.Utf8(), k.key.Utf8(), before, after));
          }
        }
      }
      continue;
    }
    if (e.append || e.remove) {  // array edits: one occurrence more, or none at all
      for (ConfigFile& f : bundle.files) {
        const std::string fname = f.name.Utf8();
        if (!EndsWithCase(fname, e.file)) continue;
        bool section_seen = false;
        for (ConfigSection& s : f.sections) {
          if (e.section != "*" && AsciiLower(s.name.Utf8()) != lsection) continue;
          section_seen = true;
          if (e.remove) {
            const size_t before = s.keys.size();
            // key-= removes every occurrence; key-=value only the occurrences whose value is exactly that (case-
            // insensitive, trimmed) - e.g. one StartupMovies entry (the intro logos) out of a list (Shagg's launcher 1).
            const std::string lvalue = AsciiLower(Trim(e.value));
            std::erase_if(s.keys, [&](const ConfigKey& k) {
              return AsciiLower(k.key.Utf8()) == lkey && (lvalue.empty() || AsciiLower(Trim(k.value.Utf8())) == lvalue);
            });
            const size_t gone = before - s.keys.size();
            changed += int(gone);
            if (gone && log) log->push_back(std::format("{} [{}] {}: {} removed", fname, s.name.Utf8(), e.key, gone));
          } else if (e.section != "*") {
            ConfigKey k;
            k.key = ConfigString::FromUtf8(e.key, true);
            k.value = ConfigString::FromUtf8(e.value, true);
            s.keys.push_back(k);
            ++changed;
            if (log) log->push_back(std::format("{} [{}] {}: appended '{}'", fname, s.name.Utf8(), e.key, e.value));
          }
        }
        if (e.append && e.section != "*" && !section_seen) {
          ConfigSection s;
          s.name = ConfigString::FromUtf8(e.section, true);
          ConfigKey k;
          k.key = ConfigString::FromUtf8(e.key, true);
          k.value = ConfigString::FromUtf8(e.value, true);
          s.keys.push_back(k);
          f.sections.push_back(s);
          ++changed;
          if (log) log->push_back(std::format("{} [{}] {}: appended (new section) '{}'", fname, e.section, e.key, e.value));
        }
      }
      continue;
    }
    for (ConfigFile& f : bundle.files) {
      const std::string fname = f.name.Utf8();
      if (!EndsWithCase(fname, e.file)) continue;
      bool section_seen = false;
      for (ConfigSection& s : f.sections) {
        if (e.section != "*" && AsciiLower(s.name.Utf8()) != lsection) continue;
        bool key_seen = false;
        for (ConfigKey& k : s.keys) {
          if (AsciiLower(k.key.Utf8()) != lkey) continue;
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
      if (e.section != "*" && !section_seen && !e.scale) {
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
  HashBlocks(data, size, true, block);
  Sha1Digest out;
  for (int k = 0; k < 5; ++k)
    for (int j = 0; j < 4; ++j) out[4 * k + j] = uint8_t(h[k] >> (24 - 8 * j));
  return out;
}

std::string Md5Hex(const uint8_t* data, size_t size) {  // RFC 1321
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                            14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                            4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  auto block = [&](const uint8_t* b) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = uint32_t(b[4 * i]) | uint32_t(b[4 * i + 1]) << 8 | uint32_t(b[4 * i + 2]) << 16 | uint32_t(b[4 * i + 3]) << 24;
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) f = (bb & c) | (~bb & d), g = i;
      else if (i < 32) f = (d & bb) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = bb ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (bb | ~d), g = (7 * i) % 16;
      const uint32_t t = d;
      d = c;
      c = bb;
      const uint32_t x = a + f + K[i] + m[g];
      bb = bb + ((x << R[i]) | (x >> (32 - R[i])));
      a = t;
    }
    h[0] += a, h[1] += bb, h[2] += c, h[3] += d;
  };
  HashBlocks(data, size, false, block);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 4; ++j) {
      const uint8_t v = uint8_t(h[k] >> (8 * j));
      out += hex[v >> 4];
      out += hex[v & 15];
    }
  return out;
}

int UpdateTocEntry(std::string& toc, const std::string& file_name, uint64_t size, const std::string& md5_hex) {
  int changed = 0;
  const std::string want = AsciiLower("\\" + file_name);
  std::string out;
  size_t pos = 0;
  while (pos < toc.size()) {
    size_t end = toc.find('\n', pos);
    const size_t next = end == std::string::npos ? toc.size() : end + 1;
    if (end == std::string::npos) end = toc.size();
    size_t body_end = end;
    if (body_end > pos && toc[body_end - 1] == '\r') --body_end;
    std::string line = toc.substr(pos, body_end - pos);
    // "<size> <usize> <path> <md5>"
    std::istringstream in(line);
    std::string sz, usz, path, md5, extra;
    if (in >> sz >> usz >> path >> md5 && !(in >> extra) && path.size() >= want.size() &&
        AsciiLower(path.substr(path.size() - want.size())) == want) {
      line = std::to_string(size) + " " + usz + " " + path + " " + md5_hex;
      ++changed;
    }
    out += line;
    out += toc.substr(body_end, next - body_end);  // the line's own ending (CRLF / LF / none)
    pos = next;
  }
  toc = std::move(out);
  return changed;
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
