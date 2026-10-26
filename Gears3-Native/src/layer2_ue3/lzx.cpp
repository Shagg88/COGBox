// Layer 2 (Unreal Engine 3 on the 360): our own LZX decoder for XMemCompress streams (the compression of the 360
// "fully compressed" packages, ue3/package_patch.h). Written from the LZX format (Microsoft's LZX DELTA / CAB
// documentation), checked byte for byte against the decompressed packages from tools/upk_decompress. Why not the SDK's
// lzx_decompress: it is libmspack's stock decoder, which reads a pad byte after an odd-length uncompressed block -
// XMemCompress writes none, so some blocks fail (Gears 3's GearGame.xxx block 340; our tool needed a patched copy).
// Our code (MIT).
#include <ue3/package_patch.h>

#include <array>
#include <cstring>
#include <vector>

namespace ue3 {
namespace {

constexpr int kNumChars = 256;
constexpr int kPrimaryLengths = 7;
constexpr int kSecondaryLengths = 249;
constexpr int kMinMatch = 2;
constexpr uint32_t kFrameSize = 32768;
constexpr int kTableBits = 16;  // one direct lookup per symbol (codes are at most 16 bits)

enum BlockType { kInvalid = 0, kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

// Bits are taken from 16-bit little-endian words, most significant first. Past the end of the input reads zeros.
class BitReader {
 public:
  BitReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  void Ensure(int n) {
    while (left_ < n) {
      const uint32_t b0 = p_ < end_ ? *p_++ : 0, b1 = p_ < end_ ? *p_++ : 0;
      buf_ |= ((b1 << 8) | b0) << (16 - left_);
      left_ += 16;
    }
  }
  uint32_t Peek(int n) const { return buf_ >> (32 - n); }
  void Remove(int n) {
    buf_ = n >= 32 ? 0 : buf_ << n;
    left_ -= n;
  }
  uint32_t Read(int n) {
    if (n == 0) return 0;
    Ensure(n);
    const uint32_t v = Peek(n);
    Remove(n);
    return v;
  }
  int left() const { return left_; }
  void Drop() { buf_ = 0; left_ = 0; }
  bool ReadByte(uint8_t* b) {  // raw byte (bit buffer must be empty)
    if (p_ >= end_) return false;
    *b = *p_++;
    return true;
  }
  size_t ReadBytes(uint8_t* dst, size_t n) {
    const size_t k = std::min(n, size_t(end_ - p_));
    std::memcpy(dst, p_, k);
    p_ += k;
    return k;
  }
  bool overrun() const { return p_ > end_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  uint32_t buf_ = 0;  // left_ bits at the top
  int left_ = 0;
};

// Canonical Huffman code from code lengths; decoded with one 16-bit lookup.
class Huffman {
 public:
  explicit Huffman(int symbols) : len_(symbols, 0) {}
  std::vector<uint8_t>& lengths() { return len_; }
  // false: the lengths don't form a complete code (an all-zero tree gives empty() instead).
  bool Build() {
    empty_ = true;
    for (uint8_t l : len_)
      if (l) empty_ = false;
    if (empty_) return true;
    table_.assign(size_t(1) << kTableBits, 0xFFFF);
    uint32_t code = 0;
    for (int bits = 1; bits <= kTableBits; ++bits) {
      for (size_t s = 0; s < len_.size(); ++s) {
        if (len_[s] != bits) continue;
        const uint32_t first = code << (kTableBits - bits), count = 1u << (kTableBits - bits);
        if (first + count > table_.size()) return false;  // over-subscribed
        for (uint32_t i = 0; i < count; ++i) table_[first + i] = uint16_t(s);
        ++code;
      }
      code <<= 1;
    }
    return code == (1u << (kTableBits + 1));  // complete (lengths 1..16 fill the whole 16-bit space)
  }
  bool empty() const { return empty_; }
  bool Decode(BitReader& in, int* sym) const {
    if (empty_) return false;
    in.Ensure(kTableBits);
    const uint16_t s = table_[in.Peek(kTableBits)];
    if (s == 0xFFFF) return false;
    in.Remove(len_[s]);
    *sym = s;
    return true;
  }

 private:
  std::vector<uint8_t> len_;
  std::vector<uint16_t> table_;
  bool empty_ = true;
};

struct Tables {
  std::array<uint8_t, 51> extra_bits{};
  std::array<uint32_t, 51> position_base{};
  Tables() {
    for (int i = 0, j = 0; i < 51; i += 2) {  // 0,0,0,0,1,1,2,2,...,17,17
      extra_bits[i] = uint8_t(j);
      if (i + 1 < 51) extra_bits[i + 1] = uint8_t(j);
      if (i != 0 && j < 17) ++j;
    }
    for (int i = 0, j = 0; i < 51; ++i) {
      position_base[i] = uint32_t(j);
      j += 1 << extra_bits[i];
    }
  }
};
const Tables& T() {
  static const Tables t;
  return t;
}

// Reads code lengths [first, last) of `tree` as deltas from the previous ones, through a 20-symbol pretree.
bool ReadLengths(BitReader& in, std::vector<uint8_t>& lens, int first, int last) {
  Huffman pre(20);
  for (int i = 0; i < 20; ++i) pre.lengths()[i] = uint8_t(in.Read(4));
  if (!pre.Build()) return false;
  for (int x = first; x < last;) {
    int z;
    if (!pre.Decode(in, &z)) return false;
    if (z == 17) {
      int y = int(in.Read(4)) + 4;
      while (y-- > 0 && x < last) lens[x++] = 0;
    } else if (z == 18) {
      int y = int(in.Read(5)) + 20;
      while (y-- > 0 && x < last) lens[x++] = 0;
    } else if (z == 19) {
      int y = int(in.Read(1)) + 4;
      if (!pre.Decode(in, &z)) return false;
      int v = lens[x] - z;
      if (v < 0) v += 17;
      while (y-- > 0 && x < last) lens[x++] = uint8_t(v);
    } else {
      int v = lens[x] - z;
      if (v < 0) v += 17;
      lens[x++] = uint8_t(v);
    }
  }
  return true;
}

}  // namespace

// One LZX stream (window 2^window_bits, no reset interval) of exactly `out_len` bytes.
bool LzxDecompress(const uint8_t* in_data, size_t in_len, uint8_t* dest, size_t out_len, int window_bits) {
  static const int kPositionSlots[] = {30, 32, 34, 36, 38, 42, 50};  // window bits 15..21
  if (window_bits < 15 || window_bits > 21 || out_len > (size_t(1) << window_bits)) return false;
  const Tables& t = T();
  const int num_main = kNumChars + kPositionSlots[window_bits - 15] * 8;
  Huffman main_tree(num_main), length_tree(kSecondaryLengths), aligned_tree(8);
  BitReader in(in_data, in_len);
  uint32_t r0 = 1, r1 = 1, r2 = 1;
  int block_type = kInvalid;
  uint32_t block_remaining = 0;
  int32_t intel_size = 0;
  bool intel_started = false;
  if (in.Read(1)) {
    const uint32_t hi = in.Read(16), lo = in.Read(16);
    intel_size = int32_t((hi << 16) | lo);
  }
  std::vector<uint8_t> window(out_len);  // decoded data; `dest` gets each frame after the CALL translation
  uint8_t* const out = window.data();
  size_t pos = 0;
  for (size_t frame_start = 0; frame_start < out_len; frame_start += kFrameSize) {
    const size_t frame_end = std::min(out_len, frame_start + kFrameSize);
    while (pos < frame_end) {
      if (block_remaining == 0) {
        block_type = int(in.Read(3));
        const uint32_t hi = in.Read(16), lo = in.Read(8);
        block_remaining = (hi << 8) | lo;
        switch (block_type) {
          case kAligned:
            for (int i = 0; i < 8; ++i) aligned_tree.lengths()[i] = uint8_t(in.Read(3));
            if (!aligned_tree.Build()) return false;
            [[fallthrough]];
          case kVerbatim:
            if (!ReadLengths(in, main_tree.lengths(), 0, kNumChars) ||
                !ReadLengths(in, main_tree.lengths(), kNumChars, num_main) || !main_tree.Build() || main_tree.empty())
              return false;
            if (main_tree.lengths()[0xE8]) intel_started = true;
            if (!ReadLengths(in, length_tree.lengths(), 0, kSecondaryLengths) || !length_tree.Build()) return false;
            break;
          case kUncompressed: {
            intel_started = true;
            // 1-16 bits of padding to the next 16-bit word, then R0-R2 as 32-bit little-endian values.
            if (in.left() == 0) in.Ensure(16);
            in.Drop();
            uint8_t b[12];
            for (uint8_t& x : b)
              if (!in.ReadByte(&x)) return false;
            r0 = b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24;
            r1 = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
            r2 = b[8] | b[9] << 8 | b[10] << 16 | uint32_t(b[11]) << 24;
            break;
          }
          default:
            return false;
        }
      }
      size_t run = std::min<size_t>(block_remaining, frame_end - pos);
      if (block_type == kUncompressed) {
        // XMemCompress: no pad byte after an odd-length uncompressed block (unlike CAB LZX).
        if (in.ReadBytes(out + pos, run) != run) return false;
        pos += run;
        block_remaining -= uint32_t(run);
        continue;
      }
      const size_t run_end = pos + run;
      while (pos < run_end) {
        int sym;
        if (!main_tree.Decode(in, &sym)) return false;
        if (sym < kNumChars) {
          out[pos++] = uint8_t(sym);
          continue;
        }
        sym -= kNumChars;
        int length = sym & kPrimaryLengths;
        if (length == kPrimaryLengths) {
          int footer;
          if (!length_tree.Decode(in, &footer)) return false;
          length += footer;
        }
        length += kMinMatch;
        uint32_t offset;
        const int slot = sym >> 3;
        if (slot == 0) {
          offset = r0;
        } else if (slot == 1) {
          offset = r1;
          r1 = r0;
          r0 = offset;
        } else if (slot == 2) {
          offset = r2;
          r2 = r0;
          r0 = offset;
        } else {
          const int extra = slot >= 36 ? 17 : t.extra_bits[slot];
          offset = t.position_base[slot] - 2;
          if (extra >= 3 && block_type == kAligned) {
            if (extra > 3) offset += in.Read(extra - 3) << 3;
            int aligned;
            if (!aligned_tree.Decode(in, &aligned)) return false;
            offset += uint32_t(aligned);
          } else if (extra) {
            offset += in.Read(extra);
          }
          r2 = r1;
          r1 = r0;
          r0 = offset;
        }
        if (offset == 0 || offset > pos || pos + size_t(length) > frame_end) return false;  // no window history here
        for (int i = 0; i < length; ++i, ++pos) out[pos] = out[pos - offset];
      }
      block_remaining -= uint32_t(run);
    }
    // Each 32 KB frame ends on a 16-bit boundary of the input.
    if (in.left() > 0) in.Ensure(16);
    if (in.left() & 15) in.Remove(in.left() & 15);
    // x86 CALL translation (only when the stream asked for it), on the copy - matches read the untranslated data.
    const size_t fsize = frame_end - frame_start;
    uint8_t* const o = dest + frame_start;
    std::memcpy(o, out + frame_start, fsize);
    if (intel_started && intel_size && frame_start / kFrameSize < 32768 && fsize > 10) {
      int32_t curpos = int32_t(frame_start);
      for (size_t i = 0; i < fsize - 10;) {
        if (o[i++] != 0xE8) {
          ++curpos;
          continue;
        }
        const int32_t abs = int32_t(o[i] | o[i + 1] << 8 | o[i + 2] << 16 | uint32_t(o[i + 3]) << 24);
        if (abs >= -curpos && abs < intel_size) {
          const int32_t rel = abs >= 0 ? abs - curpos : abs + intel_size;
          o[i] = uint8_t(rel);
          o[i + 1] = uint8_t(rel >> 8);
          o[i + 2] = uint8_t(rel >> 16);
          o[i + 3] = uint8_t(uint32_t(rel) >> 24);
        }
        i += 4;
        curpos += 5;
      }
    }
  }
  return pos == out_len;
}

}  // namespace ue3
