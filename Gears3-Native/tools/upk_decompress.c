/* upk_decompress <in.xxx> <out.upk>: decompress a "fully compressed" UE3 Xbox 360 package (Gears 3 .xxx files).
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

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: upk_decompress <in.xxx> <out.upk>\n"); return 2; }
  FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* in = malloc(n); if (fread(in, 1, n, f) != (size_t)n) { fclose(f); return 2; } fclose(f);
  FILE* o = fopen(argv[2], "wb"); if (!o) { perror(argv[2]); return 2; }
  if (n < 16 || be32(in) != 0x9E2A83C1u || be32(in + 4) != 0x20000) {  /* not fully compressed: copy */
    fwrite(in, 1, n, o); fclose(o); printf("%s: stored (%ld bytes)\n", argv[1], n); return 0;
  }
  uint32_t total_u = be32(in + 12), bs = be32(in + 4);
  uint32_t blocks = (total_u + bs - 1) / bs;
  const uint8_t* tab = in + 16; const uint8_t* data = tab + 8 * blocks;
  size_t done = 0;
  for (uint32_t i = 0; i < blocks; ++i) {
    uint32_t cs = be32(tab + 8 * i), us = be32(tab + 8 * i + 4);
    if (data + cs > in + n) { fprintf(stderr, "block %u past end\n", i); return 3; }
    uint8_t* out = calloc(us, 1);
    /* Strip the XMemCompress frame headers: 2-byte big-endian compressed size per 32 KB frame, or 0xFF + 2-byte
     * uncompressed size + 2-byte compressed size for a shorter frame; a zero size ends the block. */
    uint8_t* raw = malloc(cs); size_t rn = 0; const uint8_t* q = data; const uint8_t* qe = data + cs;
    while (q + 2 <= qe) {
      uint32_t c;
      if (q[0] == 0xFF) { if (q + 5 > qe) break; c = (uint32_t)q[3] << 8 | q[4]; q += 5; }
      else { c = (uint32_t)q[0] << 8 | q[1]; q += 2; }
      if (c == 0 || q + c > qe) break;
      memcpy(raw + rn, q, c); rn += c; q += c;
    }
    memfile src = { raw, rn, 0 }, dst = { out, us, 0 };
    struct lzxd_stream* z = lzxd_init(&msys, (struct mspack_file*)&src, (struct mspack_file*)&dst, 17, 0, 256 * 1024, us, 0);
    int r = z ? lzxd_decompress(z, us) : -1;
    if (z) lzxd_free(z);
    free(raw);
    if (r != MSPACK_ERR_OK) { fprintf(stderr, "block %u: lzx error %d (cs %u us %u)\n", i, r, cs, us); return 4; }
    fwrite(out, 1, us, o); free(out); data += cs; done += us;
  }
  fclose(o);
  printf("%s: %u blocks, %zu bytes\n", argv[1], blocks, done);
  return done == total_u ? 0 : 5;
}
