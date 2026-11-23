// Layer 0 (game-free): the game window's own icon - see xenos/window_icon.h (GitHub #355). Our code (MIT).
#include <xenos/window_icon.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <stb_image.h>  // the implementation is in renderer/prompt_overlay_compositor.cpp

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(cogbox_window_icon, true, "COGBox",
                    "GitHub #355: give the game window (taskbar, Alt+Tab) the game's own title image as its icon, read "
                    "from the game's files in memory");

namespace xenos::window_icon {
namespace {

std::mutex g_mutex;
std::string g_png;           // the title image, PNG
void* g_window = nullptr;    // HWND
bool g_done = false;

#ifdef _WIN32
// An RGBA picture resized to size x size: each output pixel is the average of the source pixels it covers (box filter),
// so the 64 x 64 title image shrinks to 16..48 without dropped rows; upsizing repeats pixels.
std::vector<uint8_t> Resize(const uint8_t* src, int w, int h, int size) {
  std::vector<uint8_t> out(size_t(size) * size * 4);
  for (int y = 0; y < size; ++y) {
    const int y0 = y * h / size, y1 = std::max(y0 + 1, (y + 1) * h / size);
    for (int x = 0; x < size; ++x) {
      const int x0 = x * w / size, x1 = std::max(x0 + 1, (x + 1) * w / size);
      uint32_t sum[4] = {0, 0, 0, 0}, csum[3] = {0, 0, 0}, n = 0;
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx, ++n) {
          const uint8_t* p = src + (size_t(sy) * w + sx) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += p[c];
          for (int c = 0; c < 3; ++c) csum[c] += uint32_t(p[c]) * p[3];
        }
      // colour weighted by alpha, so see-through edge pixels don't darken the small icon
      const size_t o = (size_t(y) * size + x) * 4;
      out[o + 3] = uint8_t(sum[3] / n);
      for (int c = 0; c < 3; ++c) out[o + c] = uint8_t(sum[3] ? csum[c] / sum[3] : 0);
    }
  }
  return out;
}

// A 32-bit icon with its alpha from an RGBA picture. nullptr on failure.
HICON MakeIcon(const uint8_t* rgba, int w, int h, int size) {
  const std::vector<uint8_t> px = Resize(rgba, w, h, size);
  BITMAPV5HEADER bi = {};
  bi.bV5Size = sizeof bi;
  bi.bV5Width = size;
  bi.bV5Height = -size;  // top-down
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!colour || !bits) {
    if (colour) DeleteObject(colour);
    return nullptr;
  }
  uint8_t* d = static_cast<uint8_t*>(bits);
  for (size_t i = 0; i < size_t(size) * size; ++i) {  // RGBA -> BGRA
    d[i * 4 + 0] = px[i * 4 + 2];
    d[i * 4 + 1] = px[i * 4 + 1];
    d[i * 4 + 2] = px[i * 4 + 0];
    d[i * 4 + 3] = px[i * 4 + 3];
  }
  // all zero (opaque by mask; the alpha decides), so nothing that draws with the mask shows garbage
  const std::vector<uint8_t> mask_bits(size_t((size + 15) / 16) * 2 * size, 0);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
  ICONINFO ii = {};
  ii.fIcon = TRUE;
  ii.hbmColor = colour;
  ii.hbmMask = mask;
  HICON icon = mask ? CreateIconIndirect(&ii) : nullptr;
  DeleteObject(colour);
  if (mask) DeleteObject(mask);
  return icon;
}
#endif

// Under g_mutex.
void ApplyLocked() {
  if (g_done || g_png.empty() || !g_window || !REXCVAR_GET(cogbox_window_icon)) return;
  g_done = true;
#ifdef _WIN32
  int w = 0, h = 0, ch = 0;
  stbi_uc* rgba = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(g_png.data()), int(g_png.size()), &w, &h,
                                        &ch, 4);
  if (!rgba || w <= 0 || h <= 0) {
    if (rgba) stbi_image_free(rgba);
    REXLOG_WARN("window_icon: the title image could not be decoded - the window keeps its default icon");
    return;
  }
  const int big_px = std::max(GetSystemMetrics(SM_CXICON), 16);
  const int small_px = std::max(GetSystemMetrics(SM_CXSMICON), 16);
  // Kept for the life of the process: the window holds them (WM_SETICON does not copy).
  HICON big_icon = MakeIcon(rgba, w, h, big_px);
  HICON small_icon = MakeIcon(rgba, w, h, small_px);
  stbi_image_free(rgba);
  HWND hwnd = static_cast<HWND>(g_window);
  // Posted, not sent: the caller may be any thread, and a cross-thread SendMessage would wait on the window's thread.
  if (big_icon && !PostMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon))) DestroyIcon(big_icon);
  if (small_icon && !PostMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon))) DestroyIcon(small_icon);
  REXLOG_INFO("window_icon: the game's title image ({}x{}) set as the window icon ({} / {} px){}", w, h, big_px, small_px,
              big_icon && small_icon ? "" : " - an icon could not be made");
#endif
}

}  // namespace

void SetImage(const void* png, size_t size) {
  if (!png || size < 8 || std::memcmp(png, "\x89PNG", 4) != 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_png.empty()) g_png.assign(static_cast<const char*>(png), size);
  ApplyLocked();
}

void SetWindow(void* native_handle) {
  if (!native_handle) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_window) g_window = native_handle;
  ApplyLocked();
}

void TakeFromRuntime(rex::Runtime* runtime) {
  if (!REXCVAR_GET(cogbox_window_icon) || !runtime || !runtime->kernel_state()) return;
  const auto db = runtime->kernel_state()->title_xdbf();
  if (!db.is_valid()) {
    REXLOG_INFO("window_icon: the game has no XDBF resource - the window keeps its default icon");
    return;
  }
  const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, 0x8000);
  if (!block || block.size == 0) {
    REXLOG_INFO("window_icon: no title image (0x8000) in the game's XDBF - the window keeps its default icon");
    return;
  }
  SetImage(block.buffer, block.size);
}

}  // namespace xenos::window_icon
