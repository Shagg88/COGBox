// Layer 0 (game-free): 4x prompt overlay compositor and pre-allocated style cache for Xbox, PlayStation, Switch,
// Steam Deck, Steam Controller, and procedural Keyboard/Mouse keycaps.
#include <xenos/renderer/prompt_overlay_compositor.h>

#include <xenos/renderer/texture_cache.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../../../cogbox/src/gen/font_bodyb.h"

namespace {

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

}  // namespace

namespace xenos::renderer::prompt {

namespace {

constexpr int kScale = 4;  // 4x overlay scale relative to guest atlas
constexpr int kSS = 4;     // 4x supersampling inside each procedural keycap

std::mutex g_alias_mutex;
std::unordered_set<uint64_t> g_global_aliases;

const char* const kCanonicalButtons[] = {
    "a",          "b",         "x",               "y",            "lb",        "rb",           "lt",       "rt",
    "ls",         "rs",        "lspress",         "rspress",      "stickmove", "back",         "start",    "dpad_up",
    "dpad_down",  "dpad_left", "dpad_right",      "dpad_up_down", "dpad_left_right", "dpad_all"};

const std::pair<const char*, const char*> kDefaultKeybinds[] = {
    {"keybind_right_trigger", "LMB"},
    {"keybind_left_trigger", "RMB"},
    {"keybind_a", "Space"},
    {"keybind_b", "F"},
    {"keybind_x", "E"},
    {"keybind_y", "Q"},
    {"keybind_right_shoulder", "R"},
    {"keybind_left_shoulder", "Tab"},
    {"keybind_dpad_left", "1"},
    {"keybind_dpad_right", "2"},
    {"keybind_dpad_up", "3"},
    {"keybind_dpad_down", "4"},
    {"keybind_rstick_press", "MMB,Z"},
    {"keybind_lstick_press", "C"},
    {"keybind_start", "Escape,Return"},
    {"keybind_back", "Backspace"},
};

std::filesystem::path ResolveGlyphsDir(const std::filesystem::path& requested) {
  std::error_code ec;
  if (!requested.empty() && std::filesystem::exists(requested / "xbox" / "a.png", ec)) {
    return requested;
  }

  std::vector<std::filesystem::path> candidates;
  if (!requested.empty()) {
    candidates.push_back(requested);
  }
#ifdef _WIN32
  char exe_path[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0) {
    const std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
    candidates.push_back(exe_dir / "assets" / "glyphs");
    candidates.push_back(exe_dir.parent_path() / "assets" / "glyphs");
    candidates.push_back(exe_dir.parent_path().parent_path() / "assets" / "glyphs");
  }
#endif
  candidates.emplace_back("assets/glyphs");
  candidates.emplace_back("Gears2-Native/assets/glyphs");
  candidates.emplace_back("COGBox/Gears2-Native/assets/glyphs");
  candidates.emplace_back("../assets/glyphs");
  candidates.emplace_back("../../assets/glyphs");
  candidates.emplace_back("../Gears2-Native/assets/glyphs");
  candidates.emplace_back("../../Gears2-Native/assets/glyphs");

  for (const auto& cand : candidates) {
    if (std::filesystem::exists(cand / "xbox" / "a.png", ec)) {
      return cand;
    }
  }
  return requested;
}

const AtlasLayout* GetLayoutForKind(AtlasKind kind) {
  for (const auto& layout : GetAllAtlasLayouts()) {
    if (layout.kind == kind) {
      return &layout;
    }
  }
  return nullptr;
}

// Premultiplied RGBA float canvas used for 4x supersampled procedural keycaps (ported from cogbox/src/keycaps.cpp).
struct Canvas {
  int w;
  int h;
  std::vector<float> px;  // r, g, b, a (premultiplied)
  Canvas(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4, 0.0f) {}

  void blend(int x, int y, float r, float g, float b, float cov) {
    if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0.0f) return;
    float* p = &px[(size_t(y) * w + x) * 4];
    const float k = 1.0f - cov;
    p[0] = r * cov + p[0] * k;
    p[1] = g * cov + p[1] * k;
    p[2] = b * cov + p[2] * k;
    p[3] = cov + p[3] * k;
  }
};

bool in_round_rect(float x, float y, float x0, float y0, float x1, float y1, float r) {
  if (x < x0 || y < y0 || x >= x1 || y >= y1) return false;
  r = std::min(r, std::min((x1 - x0) / 2.0f, (y1 - y0) / 2.0f));
  const float cx = std::clamp(x, x0 + r, x1 - r);
  const float cy = std::clamp(y, y0 + r, y1 - r);
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

void fill_round_rect(Canvas& c, float x0, float y0, float x1, float y1, float r, float cr, float cg, float cb) {
  for (int y = std::max(0, int(y0)); y < std::min(c.h, int(std::ceil(y1))); ++y) {
    for (int x = std::max(0, int(x0)); x < std::min(c.w, int(std::ceil(x1))); ++x) {
      if (in_round_rect(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r)) {
        c.blend(x, y, cr, cg, cb, 1.0f);
      }
    }
  }
}

void key_rect(Canvas& c, float x0, float y0, float x1, float y1, float r, float fill, float line, float width) {
  fill_round_rect(c, x0, y0, x1, y1, r, line, line, line);
  fill_round_rect(c, x0 + width, y0 + width, x1 - width, y1 - width, std::max(0.0f, r - width), fill, fill, fill);
}

void fill_triangle(Canvas& c, float ax, float ay, float bx, float by, float cx, float cy, float col) {
  const int minx = std::max(0, int(std::min({ax, bx, cx})));
  const int maxx = std::min(c.w - 1, int(std::max({ax, bx, cx})) + 1);
  const int miny = std::max(0, int(std::min({ay, by, cy})));
  const int maxy = std::min(c.h - 1, int(std::max({ay, by, cy})) + 1);
  auto edge = [](float x0, float y0, float x1, float y1, float x, float y) {
    return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
  };
  for (int y = miny; y <= maxy; ++y) {
    for (int x = minx; x <= maxx; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float e0 = edge(ax, ay, bx, by, px, py);
      const float e1 = edge(bx, by, cx, cy, px, py);
      const float e2 = edge(cx, cy, ax, ay, px, py);
      if ((e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) || (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f)) {
        c.blend(x, y, col, col, col, 1.0f);
      }
    }
  }
}

stbtt_fontinfo& font() {
  static stbtt_fontinfo f;
  static bool init = stbtt_InitFont(&f, font_bodyb_data, stbtt_GetFontOffsetForIndex(font_bodyb_data, 0)) != 0;
  (void)init;
  return f;
}

void draw_text(Canvas& c, const std::string& text, float x0, float y0, float x1, float y1, float col) {
  stbtt_fontinfo& f = font();
  float px = (y1 - y0) * 0.62f;
  int ascent = 0, descent = 0, gap = 0;
  stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
  float scale = 0.0f, width = 0.0f;
  for (; px > 6.0f; px -= 1.0f) {
    scale = stbtt_ScaleForPixelHeight(&f, px);
    width = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
      int adv = 0, lsb = 0;
      stbtt_GetCodepointHMetrics(&f, static_cast<unsigned char>(text[i]), &adv, &lsb);
      width += adv * scale;
      if (i + 1 < text.size()) {
        width += stbtt_GetCodepointKernAdvance(&f, static_cast<unsigned char>(text[i]),
                                               static_cast<unsigned char>(text[i + 1])) *
                 scale;
      }
    }
    if (width <= (x1 - x0) * 0.86f) break;
  }
  int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
  stbtt_GetCodepointBitmapBox(&f, 'H', scale, scale, &hx0, &hy0, &hx1, &hy1);
  const float baseline = (y0 + y1) / 2.0f + (hy1 - hy0) / 2.0f;
  float pen = (x0 + x1) / 2.0f - width / 2.0f;
  for (size_t i = 0; i < text.size(); ++i) {
    const int cp = static_cast<unsigned char>(text[i]);
    int gw = 0, gh = 0, gx = 0, gy = 0;
    unsigned char* bm =
        stbtt_GetCodepointBitmapSubpixel(&f, scale, scale, pen - std::floor(pen), 0.0f, cp, &gw, &gh, &gx, &gy);
    if (!bm) gw = gh = 0;
    for (int y = 0; y < gh; ++y) {
      for (int x = 0; x < gw; ++x) {
        c.blend(int(std::floor(pen)) + gx + x, int(std::lround(baseline)) + gy + y, col, col, col,
                bm[y * gw + x] / 255.0f);
      }
    }
    stbtt_FreeBitmap(bm, nullptr);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f, cp, &adv, &lsb);
    pen += adv * scale;
    if (i + 1 < text.size()) {
      pen += stbtt_GetCodepointKernAdvance(&f, cp, static_cast<unsigned char>(text[i + 1])) * scale;
    }
  }
}

std::string label_of(const std::string& k) {
  static const std::pair<const char*, const char*> map[] = {
      {"Space", "SPACE"},      {"Backspace", "BKSP"},   {"Escape", "ESC"},     {"Return", "ENTER"},
      {"NumpadEnter", "ENTER"}, {"Tab", "TAB"},         {"Delete", "DEL"},     {"Insert", "INS"},
      {"Home", "HOME"},        {"End", "END"},          {"PageUp", "PGUP"},    {"PageDown", "PGDN"},
      {"Shift", "SHIFT"},      {"Control", "CTRL"},     {"Alt", "ALT"},        {"Quote", "'"},
      {"Comma", ","},          {"Minus", "-"},          {"Period", "."},       {"Slash", "/"},
      {"Semicolon", ";"},      {"Plus", "="},           {"LBracket", "["},     {"Backslash", "\\"},
      {"RBracket", "]"},       {"Backtick", "`"},       {"CapsLock", "CAPS"},  {"NumpadPlus", "N+"},
      {"NumpadMinus", "N-"},   {"NumpadStar", "N*"},    {"NumpadSlash", "N/"}};
  for (const auto& m : map) {
    if (k == m.first) return m.second;
  }
  if (k.rfind("Numpad", 0) == 0) return "N" + k.substr(6);
  std::string u = k;
  for (char& ch : u) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return u;
}

std::vector<uint8_t> draw_cap(int w, int h, const std::string& k) {
  Canvas c(w * kSS, h * kSS);
  const float W = float(c.w), H = float(c.h), S = float(kSS);
  if (k == "LMB" || k == "RMB" || k == "MMB" || k == "MOUSE") {
    const float bw = std::min(W, H * 0.72f), bh = H * 0.92f, x0 = (W - bw) / 2.0f, y0 = (H - bh) / 2.0f;
    const float lw = std::max(2.0f, S);
    key_rect(c, x0, y0, x0 + bw, y0 + bh, bw / 2.0f, 235.0f / 255.0f, 30.0f / 255.0f, lw);
    const float mid = x0 + bw / 2.0f, top = y0 + bh * 0.45f;
    if (k == "MOUSE") {
    } else if (k == "MMB") {
      fill_round_rect(c, mid - bw / 8.0f, y0 + bh / 10.0f, mid + bw / 8.0f, top - bh / 10.0f, bw / 10.0f,
                      220.0f / 255.0f, 60.0f / 255.0f, 40.0f / 255.0f);
    } else {
      const float hx0 = k == "LMB" ? x0 + lw : mid + lw / 2.0f;
      const float hx1 = k == "LMB" ? mid - lw / 2.0f : x0 + bw - lw;
      for (int y = int(y0 + lw); y < int(top - lw / 2.0f); ++y) {
        for (int x = int(hx0); x < int(hx1); ++x) {
          if (in_round_rect(x + 0.5f, y + 0.5f, x0 + lw, y0 + lw, x0 + bw - lw, y0 + bh - lw, bw / 2.0f - lw)) {
            c.blend(x, y, 220.0f / 255.0f, 60.0f / 255.0f, 40.0f / 255.0f, 1.0f);
          }
        }
      }
    }
    for (int y = int(top - lw / 2.0f); y < int(top + lw / 2.0f); ++y) {
      for (int x = int(x0); x < int(x0 + bw); ++x) {
        if (in_round_rect(x + 0.5f, y + 0.5f, x0, y0, x0 + bw, y0 + bh, bw / 2.0f)) {
          c.blend(x, y, 30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f);
        }
      }
    }
    for (int y = int(y0); y < int(top); ++y) {
      for (int x = int(mid - lw / 2.0f); x < int(mid + lw / 2.0f); ++x) {
        c.blend(x, y, 30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f);
      }
    }
  } else {
    const float m = std::floor(std::min(W, H) * 0.06f), r = std::floor(std::min(W, H) * 0.22f);
    fill_round_rect(c, m, m + S, W - m, H - m, r, 20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f);
    key_rect(c, m, m, W - m, H - m - 2.0f * S, r, 232.0f / 255.0f, 25.0f / 255.0f, std::max(2.0f, S));
    const float tx0 = m, ty0 = m, tx1 = W - m, ty1 = H - m - 2.0f * S, ink = 20.0f / 255.0f;
    const float cx = (tx0 + tx1) / 2.0f, cy = (ty0 + ty1) / 2.0f, a = (ty1 - ty0) * 0.24f;
    if (k == "Left") {
      fill_triangle(c, cx - a, cy, cx + a * 0.8f, cy - a, cx + a * 0.8f, cy + a, ink);
    } else if (k == "Right") {
      fill_triangle(c, cx + a, cy, cx - a * 0.8f, cy - a, cx - a * 0.8f, cy + a, ink);
    } else if (k == "Up") {
      fill_triangle(c, cx, cy - a, cx - a, cy + a * 0.8f, cx + a, cy + a * 0.8f, ink);
    } else if (k == "Down") {
      fill_triangle(c, cx, cy + a, cx - a, cy - a * 0.8f, cx + a, cy - a * 0.8f, ink);
    } else {
      draw_text(c, !k.empty() && k[0] == '\x01' ? k.substr(1) : label_of(k), tx0, ty0, tx1, ty1, ink);
    }
  }
  std::vector<uint8_t> out(size_t(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (int sy = 0; sy < kSS; ++sy) {
        for (int sx = 0; sx < kSS; ++sx) {
          const float* p = &c.px[(size_t(y * kSS + sy) * c.w + x * kSS + sx) * 4];
          for (int i = 0; i < 4; ++i) acc[i] += p[i];
        }
      }
      const float n = float(kSS * kSS), a = acc[3] / n;
      uint8_t* o = &out[(size_t(y) * w + x) * 4];
      if (a * 255.0f < 2.0f) continue;
      for (int i = 0; i < 3; ++i) {
        o[i] = uint8_t(std::clamp(acc[i] / n / a * 255.0f + 0.5f, 0.0f, 255.0f));
      }
      o[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 2.0f, 255.0f));
    }
  }
  return out;
}

std::string FirstKey(const std::string& list) {
  const std::string first = list.substr(0, list.find(','));
  const size_t a = first.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const size_t b = first.find_last_not_of(" \t\r\n");
  return first.substr(a, b - a + 1);
}

std::string ResolveSingleKeybind(const char* cvar, const std::map<std::string, std::string>& overrides) {
  if (!cvar || !*cvar) return {};
  auto it = overrides.find(cvar);
  if (it != overrides.end() && !it->second.empty()) {
    return FirstKey(it->second);
  }
  const std::string cvar_val = rex::cvar::GetFlagByName(cvar);
  if (!cvar_val.empty()) {
    return FirstKey(cvar_val);
  }
  for (const auto& [k, def] : kDefaultKeybinds) {
    if (std::strcmp(cvar, k) == 0) {
      return FirstKey(def);
    }
  }
  return {};
}

std::string ResolveCellKeyLabel(const char* cvar, const std::map<std::string, std::string>& overrides) {
  if (!cvar || !*cvar) return {};
  if (std::strcmp(cvar, "dpad_vertical") == 0) {
    return "\x01" + label_of(ResolveSingleKeybind("keybind_dpad_up", overrides)) + "/" +
           label_of(ResolveSingleKeybind("keybind_dpad_down", overrides));
  }
  if (std::strcmp(cvar, "dpad_horizontal") == 0) {
    return "\x01" + label_of(ResolveSingleKeybind("keybind_dpad_left", overrides)) + "/" +
           label_of(ResolveSingleKeybind("keybind_dpad_right", overrides));
  }
  if (std::strcmp(cvar, "move_left") == 0) {
    auto it = overrides.find("move_left");
    if (it != overrides.end() && !it->second.empty()) return "\x01" + it->second;
    return "\x01WASD";
  }
  if (std::strcmp(cvar, "move_right") == 0) {
    auto it = overrides.find("move_right");
    if (it != overrides.end() && !it->second.empty()) return it->second;
    return "MOUSE";
  }
  return ResolveSingleKeybind(cvar, overrides);
}

void ClearCellRectOnOverlay(uint8_t* ov_rgba, int ow, int oh, const AtlasLayout& layout, const PromptCell& cell) {
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  if (cell.clear_box) {
    const int pad = (layout.kind == AtlasKind::kHud512 || layout.kind == AtlasKind::kChainsawDuel) ? 1 : 0;
    cx0 = std::clamp(cell.box[0] * kScale, 0, ow);
    cy0 = std::clamp(cell.box[1] * kScale, 0, oh);
    cx1 = std::clamp((cell.box[2] + pad) * kScale, 0, ow);
    cy1 = std::clamp((cell.box[3] + pad) * kScale, 0, oh);
  } else {
    cx0 = std::clamp(cell.draw_rect[0] * kScale, 0, ow);
    cy0 = std::clamp(cell.draw_rect[1] * kScale, 0, oh);
    cx1 = std::clamp(cell.draw_rect[2] * kScale, 0, ow);
    cy1 = std::clamp(cell.draw_rect[3] * kScale, 0, oh);
  }
  for (int y = cy0; y < cy1; ++y) {
    for (int x = cx0; x < cx1; ++x) {
      uint8_t* p = &ov_rgba[(size_t(y) * ow + x) * 4];
      p[0] = 0;
      p[1] = 0;
      p[2] = 0;
      p[3] = 1;  // alpha 1 = clear the game's original texel to transparent black
    }
  }
}

}  // namespace

const char* DeviceStyleName(DeviceStyle style) {
  switch (style) {
    case DeviceStyle::kOriginal:
      return "original";
    case DeviceStyle::kXbox:
      return "xbox";
    case DeviceStyle::kPlayStation:
      return "playstation";
    case DeviceStyle::kSwitch:
      return "switch";
    case DeviceStyle::kSteamDeck:
      return "steamdeck";
    case DeviceStyle::kSteamController:
      return "steamcontroller";
    case DeviceStyle::kKeyboard:
      return "keyboard";
  }
  return "original";
}

bool PromptOverlayCompositor::Initialize(const std::filesystem::path& glyphs_dir,
                                         const std::map<std::string, std::string>& keybind_overrides) {
  std::lock_guard<std::mutex> lock(mutex_);
  resolved_glyphs_dir_ = ResolveGlyphsDir(glyphs_dir);

  auto crop_to_content = [](const Image& src) -> Image {
    int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
    for (int y = 0; y < src.height; ++y) {
      for (int x = 0; x < src.width; ++x) {
        if (src.rgba[(size_t(y) * src.width + x) * 4 + 3] > 8) {
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
      }
    }
    if (x1 < x0) return src;
    Image out;
    out.width = x1 - x0 + 1;
    out.height = y1 - y0 + 1;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
      std::memcpy(&out.rgba[size_t(y) * out.width * 4], &src.rgba[(size_t(y + y0) * src.width + x0) * 4],
                  size_t(out.width) * 4);
    }
    return out;
  };

  auto load_png = [&](const std::filesystem::path& path, Image* out) -> bool {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &ch, 4);
    if (!decoded || w <= 0 || h <= 0) {
      if (decoded) stbi_image_free(decoded);
      return false;
    }
    Image raw;
    raw.width = w;
    raw.height = h;
    raw.rgba.assign(decoded, decoded + size_t(w) * h * 4);
    stbi_image_free(decoded);
    *out = crop_to_content(raw);
    return true;
  };

  const std::pair<DeviceStyle, const char*> kControllerProfiles[] = {
      {DeviceStyle::kXbox, "xbox"},
      {DeviceStyle::kPlayStation, "playstation"},
      {DeviceStyle::kSwitch, "switch"},
      {DeviceStyle::kSteamDeck, "steamdeck"},
      {DeviceStyle::kSteamController, "steamcontroller"},
  };

  for (const auto& [style, folder] : kControllerProfiles) {
    std::unordered_map<std::string, Image> glyphs;
    for (const char* btn : kCanonicalButtons) {
      const std::filesystem::path p = resolved_glyphs_dir_ / folder / (std::string(btn) + ".png");
      Image img;
      if (!load_png(p, &img)) {
        REXLOG_WARN("prompt_compositor: failed to load glyph PNG '{}'", p.string());
        return false;
      }
      glyphs.emplace(btn, std::move(img));
    }
    ComposeControllerStyleLocked(style, glyphs);
  }

  ComposeKeyboardStyleLocked(keybind_overrides);
  initialized_ = true;
  REXLOG_INFO("prompt_compositor: initialized from '{}', active style={}",
              resolved_glyphs_dir_.string(), DeviceStyleName(active_style_));

  if (active_style_ != DeviceStyle::kOriginal) {
    ApplyActiveStyleLocked();
  }
  return true;
}

void PromptOverlayCompositor::ComposeControllerStyleLocked(DeviceStyle style,
                                                           const std::unordered_map<std::string, Image>& glyphs) {
  auto blit_overlay = [](uint8_t* dst_rgba, int dst_w, int dst_h, const Image& src, int dx, int dy, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || src.width <= 0 || src.height <= 0) return;
    for (int y = 0; y < dh; ++y) {
      const int ty = dy + y;
      if (ty < 0 || ty >= dst_h) continue;
      for (int x = 0; x < dw; ++x) {
        const int tx = dx + x;
        if (tx < 0 || tx >= dst_w) continue;

        const float fx0 = float(x) * src.width / float(dw);
        const float fx1 = float(x + 1) * src.width / float(dw);
        const float fy0 = float(y) * src.height / float(dh);
        const float fy1 = float(y + 1) * src.height / float(dh);

        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float weight = 0.0f;
        for (int py = int(std::floor(fy0)); py < int(std::ceil(fy1)) && py < src.height; ++py) {
          for (int px = int(std::floor(fx0)); px < int(std::ceil(fx1)) && px < src.width; ++px) {
            const float w = (std::min(fx1, px + 1.0f) - std::max(fx0, float(px))) *
                            (std::min(fy1, py + 1.0f) - std::max(fy0, float(py)));
            if (w <= 0.0f) continue;
            const uint8_t* p = &src.rgba[(size_t(py) * src.width + px) * 4];
            const float a = p[3] / 255.0f;
            acc[0] += w * p[0] * a;
            acc[1] += w * p[1] * a;
            acc[2] += w * p[2] * a;
            acc[3] += w * a;
            weight += w;
          }
        }
        if (weight <= 0.0f) continue;
        const float sa = acc[3] / weight;
        if (sa <= 1e-4f) continue;
        const float src_rgb[3] = {acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]};
        uint8_t* q = &dst_rgba[(size_t(ty) * dst_w + tx) * 4];
        const float da = q[3] <= 1 ? 0.0f : (q[3] / 255.0f);
        const float oa = sa + da * (1.0f - sa);
        if (oa * 255.0f >= 2.0f) {
          for (int c = 0; c < 3; ++c) {
            const float v = (src_rgb[c] * sa + q[c] * da * (1.0f - sa)) / oa;
            q[c] = uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f)));
          }
          q[3] = uint8_t(std::clamp(std::lround(oa * 255.0f), 2L, 255L));
        }
      }
    }
  };

  const AtlasKind kKinds[kKindCount] = {
      AtlasKind::kHud512,
      AtlasKind::kFont256G3J,
      AtlasKind::kFont256G2,
      AtlasKind::kChainsawDuel,
  };

  for (AtlasKind kind : kKinds) {
    const AtlasLayout* layout = GetLayoutForKind(kind);
    if (!layout) continue;
    const uint32_t ow = layout->width * kScale;
    const uint32_t oh = layout->height * kScale;
    auto buf = std::make_shared<std::vector<uint8_t>>(8 + size_t(ow) * oh * 4, 0);
    std::memcpy(buf->data(), &ow, 4);
    std::memcpy(buf->data() + 4, &oh, 4);
    uint8_t* rgba = buf->data() + 8;

    for (const PromptCell& cell : layout->cells) {
      ClearCellRectOnOverlay(rgba, static_cast<int>(ow), static_cast<int>(oh), *layout, cell);
      auto it = glyphs.find(cell.button);
      if (it == glyphs.end()) continue;
      const Image& content = it->second;
      const int dx0 = cell.draw_rect[0] * kScale;
      const int dy0 = cell.draw_rect[1] * kScale;
      const int bw = (cell.draw_rect[2] - cell.draw_rect[0]) * kScale;
      const int bh = (cell.draw_rect[3] - cell.draw_rect[1]) * kScale;
      const float scale = std::min(float(bw) / float(content.width), float(bh) / float(content.height));
      const int w = std::max(1, int(std::lround(content.width * scale)));
      const int h = std::max(1, int(std::lround(content.height * scale)));
      blit_overlay(rgba, static_cast<int>(ow), static_cast<int>(oh), content, dx0 + (bw - w) / 2, dy0 + (bh - h) / 2,
                   w, h);
    }
    overlays_[static_cast<size_t>(style)][static_cast<size_t>(kind)] = std::move(buf);
  }
}

void PromptOverlayCompositor::ComposeKeyboardStyleLocked(const std::map<std::string, std::string>& keybind_overrides) {
  const AtlasKind kKinds[kKindCount] = {
      AtlasKind::kHud512,
      AtlasKind::kFont256G3J,
      AtlasKind::kFont256G2,
      AtlasKind::kChainsawDuel,
  };

  for (AtlasKind kind : kKinds) {
    const AtlasLayout* layout = GetLayoutForKind(kind);
    if (!layout) continue;
    const uint32_t ow = layout->width * kScale;
    const uint32_t oh = layout->height * kScale;
    auto buf = std::make_shared<std::vector<uint8_t>>(8 + size_t(ow) * oh * 4, 0);
    std::memcpy(buf->data(), &ow, 4);
    std::memcpy(buf->data() + 4, &oh, 4);
    uint8_t* rgba = buf->data() + 8;

    for (const PromptCell& cell : layout->cells) {
      ClearCellRectOnOverlay(rgba, static_cast<int>(ow), static_cast<int>(oh), *layout, cell);
      const std::string key = ResolveCellKeyLabel(cell.cvar, keybind_overrides);
      if (key.empty() || key == "None") continue;
      const int dx0 = cell.draw_rect[0] * kScale;
      const int dy0 = cell.draw_rect[1] * kScale;
      const int bw = (cell.draw_rect[2] - cell.draw_rect[0]) * kScale;
      const int bh = (cell.draw_rect[3] - cell.draw_rect[1]) * kScale;
      const std::vector<uint8_t> cap = draw_cap(bw, bh, key);
      for (int y = 0; y < bh; ++y) {
        if (dy0 + y < 0 || dy0 + y >= static_cast<int>(oh)) continue;
        for (int x = 0; x < bw; ++x) {
          if (dx0 + x < 0 || dx0 + x >= static_cast<int>(ow)) continue;
          const uint8_t* s = &cap[(size_t(y) * bw + x) * 4];
          if (s[3] >= 2) {
            std::memcpy(&rgba[(size_t(dy0 + y) * ow + dx0 + x) * 4], s, 4);
          }
        }
      }
    }
    overlays_[static_cast<size_t>(DeviceStyle::kKeyboard)][static_cast<size_t>(kind)] = std::move(buf);
  }
}

bool PromptOverlayCompositor::IsInitialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

const AtlasLayout* PromptOverlayCompositor::ResolveLayoutLocked(uint64_t hash) const {
  if (const AtlasLayout* layout = GetAtlasLayout(hash)) {
    return layout;
  }
  auto it = hash_aliases_.find(hash);
  if (it != hash_aliases_.end()) {
    return GetAtlasLayout(it->second);
  }
  return nullptr;
}

std::span<const uint8_t> PromptOverlayCompositor::GetOverlay(uint64_t hash, DeviceStyle style) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || style == DeviceStyle::kOriginal || static_cast<size_t>(style) >= kStyleCount) {
    return {};
  }
  const AtlasLayout* layout = ResolveLayoutLocked(hash);
  if (!layout) return {};
  const auto& ptr = overlays_[static_cast<size_t>(style)][static_cast<size_t>(layout->kind)];
  if (!ptr) return {};
  return std::span<const uint8_t>(ptr->data(), ptr->size());
}

std::shared_ptr<const std::vector<uint8_t>> PromptOverlayCompositor::GetOverlayShared(uint64_t hash,
                                                                                      DeviceStyle style) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || style == DeviceStyle::kOriginal || static_cast<size_t>(style) >= kStyleCount) {
    return nullptr;
  }
  const AtlasLayout* layout = ResolveLayoutLocked(hash);
  if (!layout) return nullptr;
  return overlays_[static_cast<size_t>(style)][static_cast<size_t>(layout->kind)];
}

void PromptOverlayCompositor::ApplyActiveStyleLocked() {
  if (active_style_ == DeviceStyle::kOriginal) {
    SetSuppressDiskOverlaysForPrompts(true);
    for (const auto& layout : GetAllAtlasLayouts()) {
      SetDynamicOverlayShared(layout.hash, nullptr);
    }
    for (const auto& [alias_hash, target_hash] : hash_aliases_) {
      SetDynamicOverlayShared(alias_hash, nullptr);
    }
    return;
  }

  SetSuppressDiskOverlaysForPrompts(false);
  for (const auto& layout : GetAllAtlasLayouts()) {
    const auto& ov = overlays_[static_cast<size_t>(active_style_)][static_cast<size_t>(layout.kind)];
    SetDynamicOverlayShared(layout.hash, ov);
  }
  for (const auto& [alias_hash, target_hash] : hash_aliases_) {
    if (const AtlasLayout* layout = GetAtlasLayout(target_hash)) {
      const auto& ov = overlays_[static_cast<size_t>(active_style_)][static_cast<size_t>(layout->kind)];
      SetDynamicOverlayShared(alias_hash, ov);
    }
  }
}

void PromptOverlayCompositor::SetActiveStyle(DeviceStyle style) {
  REXLOG_INFO("prompt_compositor: SetActiveStyle({})", DeviceStyleName(style));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_style_ = style;
    if (initialized_ || style == DeviceStyle::kOriginal) {
      ApplyActiveStyleLocked();
      return;
    }
  }
  // Auto-initialize from default glyphs path when SetActiveStyle is called before Initialize().
  Initialize();
}

DeviceStyle PromptOverlayCompositor::ActiveStyle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_style_;
}

void PromptOverlayCompositor::RebuildKeyboardOverlays(const std::map<std::string, std::string>& keybind_overrides) {
  std::lock_guard<std::mutex> lock(mutex_);
  ComposeKeyboardStyleLocked(keybind_overrides);
  if (active_style_ == DeviceStyle::kKeyboard) {
    ApplyActiveStyleLocked();
  }
}

void PromptOverlayCompositor::RegisterAtlasAlias(uint64_t custom_hash, uint64_t catalog_hash) {
  {
    std::lock_guard<std::mutex> alias_lock(g_alias_mutex);
    g_global_aliases.insert(custom_hash);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  hash_aliases_[custom_hash] = catalog_hash;
  if (initialized_ || active_style_ == DeviceStyle::kOriginal) {
    ApplyActiveStyleLocked();
  }
}

void PromptOverlayCompositor::ClearAtlasAliases() {
  {
    std::lock_guard<std::mutex> alias_lock(g_alias_mutex);
    g_global_aliases.clear();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [alias_hash, _] : hash_aliases_) {
    SetDynamicOverlayShared(alias_hash, nullptr);
  }
  hash_aliases_.clear();
}

PromptOverlayCompositor& GlobalPromptCompositor() {
  static PromptOverlayCompositor instance;
  return instance;
}

bool IsPromptAtlasOrAlias(uint64_t hash) {
  if (IsKnownPromptAtlas(hash)) return true;
  std::lock_guard<std::mutex> lock(g_alias_mutex);
  return g_global_aliases.count(hash) != 0;
}

}  // namespace xenos::renderer::prompt
