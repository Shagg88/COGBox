/* upk_shaders <out_dir> <file>...: decompress each "fully compressed" UE3 Xbox 360 package (Gears 3 .xxx) in memory
 * and write every compiled Xbox 360 shader in it to <out_dir>/<vs|ps>-<XXH3-64 of big-endian microcode>.ucode
 * (host-order dwords - the Gears 2 corpus format read by gn_census / gn_disasm_check / gn_hlsl_check). Shader
 * container layout per XenosRecomp shader.h (MIT): see tools/analysis/extract_shaders.py. Files that are not fully
 * compressed (GlobalShaderCache-Xbox360.bin, small stored packages) are scanned as they are.
 * Package layout:
 * Layout (big-endian): u32 tag 0x9E2A83C1, u32 block size (0x20000), u32 total compressed, u32 total uncompressed,
 * then one (u32 compressed, u32 uncompressed) pair per block, then the blocks back to back. Each block is an
 * XMemCompress LZX stream (128 KB window), decoded with libmspack's lzxd (LGPL-2.1, used as a separate tool only).
 * Files that do not start with that header are copied unchanged (some small packages are stored uncompressed).
 * Game-derived output: keep it under out\ and never commit it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mspack.h"
#include "lzx.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

struct mem { struct mspack_system sys; };
typedef struct { uint8_t* buf; size_t size, off; } memfile;
static struct mspack_file* m_open(struct mspack_system* s, const char* f, int m) { (void)s; (void)f; (void)m; return NULL; }
static void m_close(struct mspack_file* f) { (void)f; }
static int m_read(struct mspack_file* f, void* b, int n) { memfile* m = (memfile*)f; size_t r = m->size - m->off; if ((size_t)n < r) r = n; memcpy(b, m->buf + m->off, r); m->off += r; return (int)r; }
static int m_write(struct mspack_file* f, void* b, int n) { memfile* m = (memfile*)f; size_t r = m->size - m->off; if ((size_t)n < r) r = n; memcpy(m->buf + m->off, b, r); m->off += r; return (int)r; }
static int m_seek(struct mspack_file* f, off_t o, int mode) { (void)f; (void)o; (void)mode; return -1; }
static off_t m_tell(struct mspack_file* f) { return (off_t)((memfile*)f)->off; }
static void m_msg(struct mspack_file* f, const char* fmt, ...) { (void)f; (void)fmt; }
static void* m_alloc(struct mspack_system* s, size_t n) { (void)s; return calloc(n, 1); }
static void m_free(void* p) { free(p); }
static void m_copy(void* s, void* d, size_t n) { memmove(d, s, n); }
static struct mspack_system msys = { m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg, m_alloc, m_free, m_copy, NULL };

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Decompress one compressed chunk at p (tag 0x9E2A83C1, block size 0x20000, totals, block table, blocks). Each
 * 128 KB block is an independent LZX stream. XMemCompress writes no pad byte after an odd-length LZX uncompressed
 * block, unlike CAB LZX: build with libmspack's lzxd.c patched by tools/lzxd-xmem-nopad.patch, or such blocks fail
 * ("bad block type"). A block that still cannot be decoded is left zero-filled, counted in *bad_blocks, and decoding
 * goes on. */
static uint8_t* decompress_chunk(const uint8_t* p, size_t avail, size_t* out_n, size_t* in_used, unsigned* bad_blocks) {
  if (avail < 16 || be32(p) != 0x9E2A83C1u || be32(p + 4) != 0x20000) return NULL;
  uint32_t total_c = be32(p + 8), total_u = be32(p + 12), bs = 0x20000;
  uint32_t blocks = (total_u + bs - 1) / bs;
  if (total_u == 0 || total_u > 0x40000000u || 16 + 8ull * blocks > avail) return NULL;
  const uint8_t* tab = p + 16; const uint8_t* data = tab + 8 * blocks; const uint8_t* end = p + avail;
  uint8_t* all = calloc(total_u, 1); size_t done = 0;
  for (uint32_t i = 0; i < blocks; ++i) {
    uint32_t cs = be32(tab + 8 * i), us = be32(tab + 8 * i + 4);
    if (data + cs > end || done + us > total_u) { free(all); return NULL; }
    uint8_t* raw = malloc(cs ? cs : 1); size_t rn = 0; const uint8_t* q = data; const uint8_t* qe = data + cs;
    while (q + 2 <= qe) {
      uint32_t c;
      if (q[0] == 0xFF) { if (q + 5 > qe) break; c = (uint32_t)q[3] << 8 | q[4]; q += 5; }
      else { c = (uint32_t)q[0] << 8 | q[1]; q += 2; }
      if (c == 0 || q + c > qe) break;
      memcpy(raw + rn, q, c); rn += c; q += c;
    }
    memfile src = { raw, rn, 0 }, dst = { all + done, us, 0 };
    struct lzxd_stream* z = lzxd_init(&msys, (struct mspack_file*)&src, (struct mspack_file*)&dst, 17, 0, 256 * 1024, us, 0);
    int r = z ? lzxd_decompress(z, us) : -1;
    if (z) lzxd_free(z);
    free(raw);
    if (r != MSPACK_ERR_OK) {
      memset(all + done, 0, us); (*bad_blocks)++;
      if (getenv("UPK_DEBUG")) fprintf(stderr, "bad block %u of %u: compressed %u, uncompressed %u, lzxd error %d, decoded %zu\n", i, blocks, cs, us, r, dst.off);
    }
    data += cs; done += us;
  }
  (void)total_c;
  *out_n = done; *in_used = (size_t)(data - p); return all;
}

static unsigned nv, np, dup, bad;
static const char* outdir;
static void scan(const uint8_t* d, size_t dn) {
  for (size_t i = 0; i + 0x24 <= dn; ++i) {
    if (d[i] != 0x10 || d[i + 1] != 0x2A || d[i + 2] != 0x11 || d[i + 3] > 1) continue;
    uint32_t vsz = be32(d + i + 4), psz = be32(d + i + 8), so = be32(d + i + 24);
    if (!(so >= 0x24 && so < vsz && vsz <= 0x100000 && psz > 0 && psz <= 0x100000 && i + vsz + psz <= dn)) { bad++; continue; }
    uint32_t poff = be32(d + i + so), size = be32(d + i + so + 4);
    if (size == 0 || size % 4 || poff + size > psz) { bad++; continue; }
    const uint8_t* be = d + i + vsz + poff;
    uint64_t h = XXH3_64bits(be, size);
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s-%016llX.ucode", outdir, d[i + 3] == 1 ? "vs" : "ps", (unsigned long long)h);
    FILE* o = fopen(path, "wbx");  /* exclusive create: another worker may write the same shader */
    if (!o) { dup++; continue; }
    for (uint32_t k = 0; k < size; k += 4) { uint8_t le[4] = { be[k + 3], be[k + 2], be[k + 1], be[k] }; fwrite(le, 1, 4, o); }
    fclose(o);
    if (d[i + 3] == 1) nv++; else np++;
  }
}

/* Level packages keep material shaders in a SeekFreeShaderCache export: an index (16-byte key, u32 0, u16 offset,
 * u16 size per shader) followed by code chunks stored as u32 uncompressed size, u32 compressed size, then an
 * XMemCompress framed stream (same frames as package blocks; decoded size up to 64 KB). Seen in SP_OldShipyard_04_S
 * (59 pixel shaders in the first chunk). Any (us, cs, frames) triple whose frames add up is decoded and scanned;
 * "failed" counts candidates that looked like a chunk but did not decode (mostly chance matches in other data). */
static unsigned nested, nested_bad;
static void scan_nested(const uint8_t* d, size_t dn) {
  for (size_t i = 0; i + 13 <= dn; ++i) {
    uint32_t us = be32(d + i), cs = be32(d + i + 4);
    if (us < 64 || us > 0x100000 || cs < 8 || cs > us + 64 || i + 8 + cs > dn) continue;
    const uint8_t* q = d + i + 8; const uint8_t* qe = q + cs; uint32_t sum_u = 0, sum_c = 0, frames = 0;
    while (q + 2 <= qe) {
      uint32_t u = 32768, c;
      if (q[0] == 0xFF) { if (q + 5 > qe) break; u = (uint32_t)q[1] << 8 | q[2]; c = (uint32_t)q[3] << 8 | q[4]; q += 5; }
      else { c = (uint32_t)q[0] << 8 | q[1]; q += 2; }
      if (c == 0 || c > 0x8100 || u == 0 || u > 32768 || q + c > qe) break;
      q += c; sum_u += u; sum_c += c; frames++;
      if (sum_u >= us) break;
    }
    if (!frames || sum_u < us || sum_u - us >= 32768 || (size_t)(qe - q) > 16) continue;
    uint8_t* raw = malloc(sum_c); size_t rn = 0; q = d + i + 8;
    for (uint32_t k = 0; k < frames; ++k) {
      uint32_t c;
      if (q[0] == 0xFF) { c = (uint32_t)q[3] << 8 | q[4]; q += 5; } else { c = (uint32_t)q[0] << 8 | q[1]; q += 2; }
      memcpy(raw + rn, q, c); rn += c; q += c;
    }
    uint8_t* out = calloc(us, 1);
    memfile src = { raw, rn, 0 }, dst = { out, us, 0 };
    struct lzxd_stream* z = lzxd_init(&msys, (struct mspack_file*)&src, (struct mspack_file*)&dst, 17, 0, 256 * 1024, us, 0);
    int r = z ? lzxd_decompress(z, us) : -1;
    if (z) lzxd_free(z);
    if (r == MSPACK_ERR_OK) { scan(out, us); nested++; i += 8 + cs - 1; } else nested_bad++;
    free(out); free(raw);
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: upk_shaders <out_dir> <file>...\n"); return 2; }
  outdir = argv[1];
  unsigned long tot_new = 0, tot_dup = 0, failed = 0, tot_bad_blocks = 0;
  for (int a = 2; a < argc; ++a) {
    FILE* f = fopen(argv[a], "rb"); if (!f) { printf("%s: cannot open\n", argv[a]); failed++; continue; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* in = malloc(n ? n : 1); size_t got = fread(in, 1, n, f); fclose(f);
    nv = np = dup = bad = 0; nested = nested_bad = 0;
    unsigned chunks = 0, bad_blocks = 0; size_t unpacked = 0;
    /* Uncompressed parts (package header, stored packages, GlobalShaderCache) are scanned as they are; every compressed
     * chunk found in the file (fully compressed packages are one chunk at offset 0) is decompressed and scanned. */
    scan(in, got);
    /* UPK_DUMP=<path>: also write the package as the game sees it (bytes before the first chunk, then every chunk
     * decompressed in order) for header / export-table inspection. */
    const char* dump_path = getenv("UPK_DUMP"); FILE* dump = dump_path ? fopen(dump_path, "wb") : NULL; int first = 1;
    for (size_t i = 0; i + 16 <= got; ) {
      if (be32(in + i) == 0x9E2A83C1u && be32(in + i + 4) == 0x20000) {
        size_t dn = 0, used = 0;
        uint8_t* d = decompress_chunk(in + i, got - i, &dn, &used, &bad_blocks);
        if (d && dump && first) { fwrite(in, 1, i, dump); first = 0; }
        if (d && dump) fwrite(d, 1, dn, dump);
        if (d) { scan(d, dn); scan_nested(d, dn); free(d); chunks++; unpacked += dn; i += used; continue; }
      }
      i += 1;
    }
    if (dump) fclose(dump);
    printf("%s: %zu bytes, %u chunks -> %zu bytes, shader-code chunks %u (failed %u), new vs %u ps %u, dup %u, bad %u, undecodable blocks %u\n",
           argv[a], got, chunks, unpacked, nested, nested_bad, nv, np, dup, bad, bad_blocks);
    tot_new += nv + np; tot_dup += dup; tot_bad_blocks += bad_blocks;
    free(in);
  }
  printf("TOTAL new %lu dup %lu failed %lu undecodable_blocks %lu\n", tot_new, tot_dup, failed, tot_bad_blocks);
  return failed ? 1 : 0;
}
