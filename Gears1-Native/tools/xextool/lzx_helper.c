/* Small LZX helper for xextool.py. Ours, MIT. Links against libmspack's lzxd.c (LGPL-2.1, used as a library).
 *
 * gj_lzx_decompress(src, src_len, dst, dst_len, window_size, ref, ref_len)
 *   Decompresses one LZX stream (XEX "normal" compression, or one XEX delta-patch entry) into dst.
 *   For a delta entry, ref is the old bytes: they sit at the END of an otherwise zeroed window,
 *   which is how the XEX delta format addresses them.
 *   Returns 0 on success, otherwise a libmspack error code (or -1 for a setup failure).
 */
#include <stdlib.h>
#include <string.h>
#include "mspack.h"
#include "lzx.h"

struct memfile {
  unsigned char *buf;
  size_t size;
  size_t pos;
};

static struct mspack_file *mem_open(struct mspack_system *s, const char *f, int m) { (void)s; (void)f; (void)m; return NULL; }
static void mem_close(struct mspack_file *f) { (void)f; }

static int mem_read(struct mspack_file *file, void *buffer, int bytes) {
  struct memfile *f = (struct memfile *)file;
  size_t left = f->size - f->pos;
  size_t n = (size_t)bytes < left ? (size_t)bytes : left;
  memcpy(buffer, f->buf + f->pos, n);
  f->pos += n;
  return (int)n;
}

static int mem_write(struct mspack_file *file, void *buffer, int bytes) {
  struct memfile *f = (struct memfile *)file;
  size_t left = f->size - f->pos;
  if ((size_t)bytes > left) return -1; /* never write past the destination */
  memcpy(f->buf + f->pos, buffer, (size_t)bytes);
  f->pos += (size_t)bytes;
  return bytes;
}

static int mem_seek(struct mspack_file *f, off_t o, int m) { (void)f; (void)o; (void)m; return -1; }
static off_t mem_tell(struct mspack_file *f) { return (off_t)((struct memfile *)f)->pos; }
static void mem_msg(struct mspack_file *f, const char *fmt, ...) { (void)f; (void)fmt; }
static void *mem_alloc(struct mspack_system *s, size_t n) { (void)s; return calloc(1, n); }
static void mem_free(void *p) { free(p); }
static void mem_copy(void *src, void *dst, size_t n) { memmove(dst, src, n); }

static struct mspack_system memsys = {
  mem_open, mem_close, mem_read, mem_write, mem_seek, mem_tell, mem_msg, mem_alloc, mem_free, mem_copy, NULL
};

int gj_lzx_decompress(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len,
                      unsigned int window_size, const unsigned char *ref, size_t ref_len) {
  int bits = 0;
  while (bits < 32 && (1u << bits) != window_size) bits++;
  if (bits < 15 || bits > 21) return -1;
  if (ref && ref_len > window_size) return -1;

  struct memfile in = { (unsigned char *)src, src_len, 0 };
  struct memfile out = { dst, dst_len, 0 };
  struct lzxd_stream *lzx = lzxd_init(&memsys, (struct mspack_file *)&in, (struct mspack_file *)&out,
                                      bits, 0, 0x8000, (off_t)dst_len, 0);
  if (!lzx) return -1;
  if (ref) {
    memset(lzx->window, 0, window_size - ref_len);
    memcpy(lzx->window + (window_size - ref_len), ref, ref_len);
    lzx->ref_data_size = window_size;
  }
  int rc = lzxd_decompress(lzx, (off_t)dst_len);
  lzxd_free(lzx);
  return rc;
}
