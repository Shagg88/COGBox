// CogBox 0.4.6: the pictures in a game's default.xex - see xdbf_icons.h. Our code (MIT).
#include "xdbf_icons.h"

#include <ue3/prebuild_lzx.h>   // Gears2-Native/src/layer2_ue3 (ours, MIT): the LZX decoder the launcher already links

#include <algorithm>
#include <cstring>
#include <fstream>

namespace xdbf {
namespace {

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// ---- AES-128 decryption (FIPS-197), enough for the XEX session key (ECB) and the image (CBC, zero IV) -------------
struct Aes {
    uint8_t rk[176];
    uint8_t sbox[256], inv[256];

    static uint8_t xt(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b)
    {
        uint8_t r = 0;
        while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
        return r;
    }
    uint8_t m9[256], m11[256], m13[256], m14[256];

    explicit Aes(const uint8_t key[16])
    {
        // the S-box from the multiplicative inverse and the affine map
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= uint8_t(q << 1); q ^= uint8_t(q << 2); q ^= uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            const uint8_t s = uint8_t(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4) ^ 0x63);
            sbox[p] = s;
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = uint8_t(i);
        for (int i = 0; i < 256; i++) {
            m9[i] = mul(uint8_t(i), 9); m11[i] = mul(uint8_t(i), 11); m13[i] = mul(uint8_t(i), 13); m14[i] = mul(uint8_t(i), 14);
        }
        std::memcpy(rk, key, 16);
        uint8_t rc = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
            if (i % 16 == 0) {
                const uint8_t t0 = t[0];
                t[0] = uint8_t(sbox[t[1]] ^ rc); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[t0];
                rc = xt(rc);
            }
            for (int j = 0; j < 4; j++) rk[i + j] = uint8_t(rk[i - 16 + j] ^ t[j]);
        }
    }

    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16], t[16];
        for (int i = 0; i < 16; i++) s[i] = uint8_t(in[i] ^ rk[160 + i]);
        for (int round = 9; ; round--) {
            // InvShiftRows + InvSubBytes (column-major state: byte i is row i%4, column i/4)
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) t[c * 4 + r] = inv[s[((c - r + 4) % 4) * 4 + r]];
            for (int i = 0; i < 16; i++) t[i] ^= rk[round * 16 + i];
            if (round == 0) break;
            for (int c = 0; c < 4; c++) {   // InvMixColumns
                const uint8_t *a = t + c * 4;
                s[c * 4 + 0] = uint8_t(m14[a[0]] ^ m11[a[1]] ^ m13[a[2]] ^ m9[a[3]]);
                s[c * 4 + 1] = uint8_t(m9[a[0]] ^ m14[a[1]] ^ m11[a[2]] ^ m13[a[3]]);
                s[c * 4 + 2] = uint8_t(m13[a[0]] ^ m9[a[1]] ^ m14[a[2]] ^ m11[a[3]]);
                s[c * 4 + 3] = uint8_t(m11[a[0]] ^ m13[a[1]] ^ m9[a[2]] ^ m14[a[3]]);
            }
        }
        std::memcpy(out, t, 16);
    }

    // CBC over [in, in + n) (n a multiple of 16); iv: the ciphertext block before it (zeros at the start)
    void decrypt_cbc(const uint8_t *in, uint8_t *out, size_t n, const uint8_t iv0[16]) const
    {
        uint8_t iv[16];
        std::memcpy(iv, iv0, 16);
        for (size_t o = 0; o + 16 <= n; o += 16) {
            uint8_t b[16], c[16];
            std::memcpy(c, in + o, 16);
            decrypt_block(c, b);
            for (int i = 0; i < 16; i++) out[o + i] = uint8_t(b[i] ^ iv[i]);
            std::memcpy(iv, c, 16);
        }
    }
};

const uint8_t kRetail[16] = { 0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91 };
const uint8_t kDevkit[16] = {};

bool fail(std::string *err, const char *what) { if (err) *err = what; return false; }

}  // namespace

bool parse_xdbf_images(const uint8_t *d, size_t n, std::vector<Image> &out)
{
    if (n < 24 || std::memcmp(d, "XDBF", 4) != 0) return false;
    const uint32_t etl = be32(d + 8), ec = be32(d + 12), ftl = be32(d + 16);
    const uint64_t base = 24 + uint64_t(etl) * 18 + uint64_t(ftl) * 8;
    if (ec > etl || base > n) return false;
    for (uint32_t i = 0; i < ec; i++) {
        const uint8_t *e = d + 24 + size_t(i) * 18;
        const uint16_t ns = be16(e);
        const uint64_t id = be64(e + 2);
        const uint64_t off = base + be32(e + 10), sz = be32(e + 14);
        if (ns != 2 || off + sz > n || sz < 8 || std::memcmp(d + off, "\x89PNG", 4) != 0) continue;   // 2: images
        out.push_back({ id, std::string((const char *)d + off, (size_t)sz) });
    }
    return true;
}

bool parse_xex_images(const std::string &file, std::vector<Image> &out, uint32_t *title_id, std::string *err)
{
    const uint8_t *d = (const uint8_t *)file.data();
    const size_t n = file.size();
    if (n < 0x18 || std::memcmp(d, "XEX2", 4) != 0) return fail(err, "not an XEX2 file");
    const uint32_t flags = be32(d + 4), hdr = be32(d + 8), sec = be32(d + 0x10), cnt = be32(d + 0x14);
    if (flags & 0x60) return fail(err, "a title update patch, not a game");
    if (hdr > n || uint64_t(sec) + 0x184 > n || 0x18 + uint64_t(cnt) * 8 > hdr) return fail(err, "damaged XEX header");
    uint32_t ff = 0, res = 0, exec = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint32_t k = be32(d + 0x18 + 8 * i), v = be32(d + 0x1C + 8 * i);
        if (k == 0x000003FF) ff = v;
        else if (k == 0x000002FF) res = v;
        else if (k == 0x00040006) exec = v;
    }
    if (title_id) *title_id = exec && uint64_t(exec) + 16 <= hdr ? be32(d + exec + 12) : 0;
    if (!ff || uint64_t(ff) + 8 > hdr || !res || uint64_t(res) + 4 > hdr) return fail(err, "no file format or resource header");
    const uint32_t image_size = be32(d + sec + 4), load = be32(d + sec + 0x110);
    const uint16_t enc = be16(d + ff + 4), comp = be16(d + ff + 6);
    const uint32_t ff_size = be32(d + ff);
    const uint8_t *payload = d + hdr;
    const size_t plen = (n - hdr) & ~size_t(15);

    // the XDBF resource: named by the title ID ("4D53082D"); the first one that is an XDBF wins
    struct Res { uint32_t addr, size; };
    std::vector<Res> list;
    const uint32_t rsz = be32(d + res);
    for (uint32_t o = 4; o + 16 <= rsz && res + o + 16 <= hdr; o += 16)
        list.push_back({ be32(d + res + o + 8), be32(d + res + o + 12) });
    if (list.empty()) return fail(err, "no resources");

    for (const uint8_t *master : { kRetail, kDevkit }) {
        uint8_t key[16];
        Aes(master).decrypt_block(d + sec + 0x150, key);
        const Aes aes(key);
        const uint8_t zero[16] = {};
        if (comp == 1) {
            // "basic": runs of data then zeros. Only the bytes a resource covers are decrypted (CBC: each block needs
            // only the ciphertext block before it).
            const uint32_t nb = ff_size >= 8 ? (ff_size - 8) / 8 : 0;
            for (const Res &r : list) {
                if (r.addr < load || uint64_t(r.addr - load) + r.size > image_size) continue;
                const uint64_t want = r.addr - load;
                uint64_t img = 0, pay = 0;
                bool found = false;
                for (uint32_t b = 0; b < nb && uint64_t(ff) + 16 + 8ull * b <= hdr; b++) {
                    const uint32_t dsz = be32(d + ff + 8 + 8 * b), zsz = be32(d + ff + 12 + 8 * b);
                    if (want >= img && want + r.size <= img + dsz) { pay += want - img; found = true; break; }
                    img += uint64_t(dsz) + zsz;
                    pay += dsz;
                }
                if (!found || pay + r.size > plen) continue;
                std::vector<uint8_t> buf;
                const uint8_t *blob = payload + pay;
                if (enc == 1) {
                    const uint64_t a = pay & ~uint64_t(15), e = std::min<uint64_t>(plen, (pay + r.size + 15) & ~uint64_t(15));
                    buf.resize(size_t(e - a));
                    aes.decrypt_cbc(payload + a, buf.data(), buf.size(), a ? payload + a - 16 : zero);
                    blob = buf.data() + (pay - a);
                }
                if (parse_xdbf_images(blob, r.size, out)) return true;
            }
        } else if (comp == 2) {
            // "normal": the whole image is one LZX stream in hashed blocks
            if (uint64_t(ff) + 36 > hdr) return fail(err, "damaged file format header");
            const uint32_t window = be32(d + ff + 8);
            int bits = 0;
            while (bits < 32 && (1u << bits) < window) bits++;
            if (bits < 15 || bits > 21 || image_size > ue3::prebuild::kPrebuildLzxMaxOutput) return fail(err, "LZX window or image size not supported");
            std::vector<uint8_t> plain(plen);
            if (enc == 1) aes.decrypt_cbc(payload, plain.data(), plen, zero);
            else std::memcpy(plain.data(), payload, plen);
            std::vector<uint8_t> stream;
            size_t p = 0, size = be32(d + ff + 12);
            while (size && p + size <= plain.size() && size >= 24) {
                const uint8_t *blk = plain.data() + p;
                const size_t next = be32(blk);
                size_t q = 24;
                while (q + 2 <= size) {
                    const size_t c = be16(blk + q);
                    q += 2;
                    if (!c || q + c > size) break;
                    stream.insert(stream.end(), blk + q, blk + q + c);
                    q += c;
                }
                p += size;
                size = next;
            }
            std::vector<uint8_t> image(image_size);
            if (stream.empty() || !ue3::prebuild::PrebuildLzxDecode(stream.data(), stream.size(), image.data(), image.size(), bits)) continue;
            for (const Res &r : list)
                if (r.addr >= load && uint64_t(r.addr - load) + r.size <= image_size &&
                    parse_xdbf_images(image.data() + (r.addr - load), r.size, out)) return true;
        } else if (comp == 0) {
            std::vector<uint8_t> plain(plen);
            if (enc == 1) aes.decrypt_cbc(payload, plain.data(), plen, zero);
            else std::memcpy(plain.data(), payload, plen);
            for (const Res &r : list)
                if (r.addr >= load && uint64_t(r.addr - load) + r.size <= plain.size() &&
                    parse_xdbf_images(plain.data() + (r.addr - load), r.size, out)) return true;
        } else {
            return fail(err, "unknown compression");
        }
        if (enc != 1) break;   // not encrypted: the key does not matter
    }
    return fail(err, "no XDBF found (wrong key or damaged file)");
}

bool read_xex_images(const std::filesystem::path &xex, std::vector<Image> &out, uint32_t *title_id, std::string *err)
{
    std::ifstream f(xex, std::ios::binary);
    if (!f) return fail(err, "could not open the file");
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len <= 0 || len > (std::streamoff(96) << 20)) return fail(err, "not an XEX of a size we read");
    std::string data((size_t)len, '\0');
    f.seekg(0);
    if (!f.read(&data[0], len)) return fail(err, "could not read the file");
    return parse_xex_images(data, out, title_id, err);
}

}  // namespace xdbf
