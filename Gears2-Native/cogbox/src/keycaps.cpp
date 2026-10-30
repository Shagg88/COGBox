// COGBox - keyboard button prompts drawn by the launcher (job 661; jobs 629 / 660). Each game shows its controller
// buttons from one 256x256 sheet (Judgment 5D9324DD, Gears 3 A52EE00B, Gears 2 A133822C). The game's renderer
// (--gow2_texture_overlay_dir) draws an overlay over the player's own copy of that sheet: alpha 0 keeps the game's
// texel, 1 clears it, 2+ is ours. Here each sheet's overlay is drawn at 4x the sheet's size (the renderer then makes the
// host texture 4x bigger, job 661), with a key cap for the key the player set for each button - the Gears layout by
// default, their own keys otherwise. Our own artwork: a light rounded key with its label in Barlow SemiBold (the
// launcher's font), or a mouse with the pressed button in red. Cell positions: the job 629 overlays (Gears 3 and
// Judgment share one layout; Gears 2's was read from its overlay, job 660).
#include "gears_settings.h"
#include "gen/font_bodyb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace cogbox {
namespace gears {
namespace {

struct Cell { int x0, y0, x1, y1; const char *cvar; };
const Cell kG3J[] = {
    { 200, 54, 227, 81, "keybind_a" }, { 227, 54, 254, 81, "keybind_b" }, { 197, 109, 221, 133, "keybind_x" },
    { 222, 109, 248, 133, "keybind_y" }, { 200, 83, 223, 106, "keybind_lstick_press" }, { 223, 83, 248, 106, "keybind_lstick_press" },
    { 0, 81, 23, 108, "keybind_rstick_press" }, { 31, 134, 56, 162, "keybind_left_trigger" }, { 57, 134, 81, 162, "keybind_right_trigger" },
    { 81, 139, 109, 157, "keybind_left_shoulder" }, { 109, 139, 138, 157, "keybind_right_shoulder" },
    { 203, 188, 229, 218, "keybind_left_trigger" }, { 229, 188, 256, 218, "keybind_right_trigger" },
    { 0, 219, 29, 238, "keybind_left_shoulder" }, { 29, 219, 58, 238, "keybind_right_shoulder" },
    { 103, 55, 135, 78, "keybind_back" }, { 147, 55, 179, 78, "keybind_start" }, { 44, 55, 71, 78, "keybind_back" },
    { 74, 55, 104, 78, "keybind_start" },
    // job 662 (Shagg 00:48 "ALL those prompts"): the d-pad (weapon keys) and the pressed sticks
    { 218, 28, 238, 51, "keybind_dpad_up" }, { 232, 160, 251, 182, "keybind_dpad_up" },
    { 194, 30, 217, 51, "keybind_dpad_right" }, { 30, 195, 52, 214, "keybind_dpad_right" },
    { 0, 57, 21, 80, "keybind_dpad_down" }, { 80, 195, 99, 217, "keybind_dpad_down" },
    { 21, 57, 44, 78, "keybind_dpad_left" }, { 0, 195, 22, 214, "keybind_dpad_left" },
    { 158, 188, 177, 213, "dpad_vertical" }, { 177, 191, 202, 210, "dpad_horizontal" },
    { 80, 161, 109, 191, "keybind_rstick_press" }, { 143, 161, 172, 190, "keybind_lstick_press" } };
const Cell kG2[] = {
    { 202, 55, 227, 80, "keybind_a" }, { 228, 55, 254, 80, "keybind_b" }, { 196, 109, 221, 136, "keybind_x" },
    { 222, 109, 248, 136, "keybind_y" }, { 200, 83, 223, 106, "keybind_lstick_press" }, { 223, 83, 246, 106, "keybind_lstick_press" },
    { 0, 84, 21, 105, "keybind_rstick_press" }, { 31, 134, 57, 164, "keybind_left_trigger" }, { 57, 134, 81, 164, "keybind_right_trigger" },
    { 81, 137, 109, 158, "keybind_left_shoulder" }, { 109, 137, 138, 158, "keybind_right_shoulder" },
    { 203, 189, 228, 219, "keybind_left_trigger" }, { 228, 189, 255, 219, "keybind_right_trigger" },
    { 0, 219, 29, 239, "keybind_left_shoulder" }, { 29, 219, 57, 239, "keybind_right_shoulder" },
    { 45, 55, 72, 76, "keybind_back" }, { 75, 55, 102, 76, "keybind_start" }, { 106, 55, 136, 76, "keybind_back" },
    { 149, 55, 179, 76, "keybind_start" },
    // job 662: the d-pad, and the sticks (moved: WASD / the mouse; pressed: their keys)
    { 233, 161, 251, 184, "keybind_dpad_up" }, { 3, 189, 24, 211, "keybind_dpad_left" },
    { 29, 192, 52, 211, "keybind_dpad_right" }, { 81, 192, 99, 216, "keybind_dpad_down" },
    { 79, 168, 110, 191, "move_right" }, { 109, 164, 140, 191, "keybind_rstick_press" },
    { 141, 168, 172, 192, "move_left" }, { 172, 164, 203, 191, "keybind_lstick_press" } };
struct Sheet { const char *hash; const Cell *cells; size_t count; };
const Sheet kSheets[] = { { "5D9324DDFEA00FBF", kG3J, sizeof kG3J / sizeof kG3J[0] },
                          { "A52EE00BE48FE5C5", kG3J, sizeof kG3J / sizeof kG3J[0] },
                          { "A133822CBA91C272", kG2, sizeof kG2 / sizeof kG2[0] } };
constexpr int kScale = 4;   // the overlay is 4x the sheet
constexpr int kSS = 4;      // supersampling of each cap

// A premultiplied RGBA float canvas, kSS x the cap's size.
struct Canvas {
    int w, h;
    std::vector<float> px;   // r, g, b, a (premultiplied)
    Canvas(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4, 0.0f) {}
    void blend(int x, int y, float r, float g, float b, float cov) {
        if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0) return;
        float *p = &px[(size_t(y) * w + x) * 4];
        const float k = 1.0f - cov;
        p[0] = r * cov + p[0] * k; p[1] = g * cov + p[1] * k; p[2] = b * cov + p[2] * k; p[3] = cov + p[3] * k;
    }
};
bool in_round_rect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || y < y0 || x >= x1 || y >= y1) return false;
    r = std::min(r, std::min((x1 - x0) / 2, (y1 - y0) / 2));
    const float cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
void fill_round_rect(Canvas &c, float x0, float y0, float x1, float y1, float r, float cr, float cg, float cb) {
    for (int y = std::max(0, int(y0)); y < std::min(c.h, int(std::ceil(y1))); ++y)
        for (int x = std::max(0, int(x0)); x < std::min(c.w, int(std::ceil(x1))); ++x)
            if (in_round_rect(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r)) c.blend(x, y, cr, cg, cb, 1.0f);
}
// PIL-style outlined rounded rectangle: the outline inside the shape, then the fill inset by its width.
void key_rect(Canvas &c, float x0, float y0, float x1, float y1, float r, float fill, float line, float width) {
    fill_round_rect(c, x0, y0, x1, y1, r, line, line, line);
    fill_round_rect(c, x0 + width, y0 + width, x1 - width, y1 - width, std::max(0.0f, r - width), fill, fill, fill);
}
void fill_triangle(Canvas &c, float ax, float ay, float bx, float by, float cx, float cy, float col) {
    const int minx = std::max(0, int(std::min({ ax, bx, cx }))), maxx = std::min(c.w - 1, int(std::max({ ax, bx, cx })) + 1);
    const int miny = std::max(0, int(std::min({ ay, by, cy }))), maxy = std::min(c.h - 1, int(std::max({ ay, by, cy })) + 1);
    auto edge = [](float x0, float y0, float x1, float y1, float x, float y) { return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0); };
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py), e2 = edge(cx, cy, ax, ay, px, py);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) c.blend(x, y, col, col, col, 1.0f);
        }
}

stbtt_fontinfo &font() {
    static stbtt_fontinfo f;
    static bool init = stbtt_InitFont(&f, font_bodyb_data, stbtt_GetFontOffsetForIndex(font_bodyb_data, 0)) != 0;
    (void)init;
    return f;
}
// Label text centred in (x0, y0)-(x1, y1), as large as fits (the cap's face: 62% of its height at most).
void draw_text(Canvas &c, const std::string &text, float x0, float y0, float x1, float y1, float col) {
    stbtt_fontinfo &f = font();
    float px = (y1 - y0) * 0.62f;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
    float scale = 0, width = 0;
    for (; px > 6; px -= 1) {
        scale = stbtt_ScaleForPixelHeight(&f, px);
        width = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f, (unsigned char)text[i], &adv, &lsb);
            width += adv * scale;
            if (i + 1 < text.size()) width += stbtt_GetCodepointKernAdvance(&f, (unsigned char)text[i], (unsigned char)text[i + 1]) * scale;
        }
        if (width <= (x1 - x0) * 0.86f) break;
    }
    // vertical: centre the capital height (the font's 'H' box)
    int hx0, hy0, hx1, hy1;
    stbtt_GetCodepointBitmapBox(&f, 'H', scale, scale, &hx0, &hy0, &hx1, &hy1);
    const float baseline = (y0 + y1) / 2 + (hy1 - hy0) / 2.0f;
    float pen = (x0 + x1) / 2 - width / 2;
    for (size_t i = 0; i < text.size(); ++i) {
        const int cp = (unsigned char)text[i];
        int gw = 0, gh = 0, gx = 0, gy = 0;
        unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(&f, scale, scale, pen - std::floor(pen), 0, cp, &gw, &gh, &gx, &gy);
        if (!bm) gw = gh = 0;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                c.blend(int(std::floor(pen)) + gx + x, int(std::lround(baseline)) + gy + y, col, col, col, bm[y * gw + x] / 255.0f);
        stbtt_FreeBitmap(bm, nullptr);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f, cp, &adv, &lsb);
        pen += adv * scale;
        if (i + 1 < text.size()) pen += stbtt_GetCodepointKernAdvance(&f, cp, (unsigned char)text[i + 1]) * scale;
    }
}

std::string label_of(const std::string &k) {
    static const std::pair<const char *, const char *> map[] = {
        { "Space", "SPACE" }, { "Backspace", "BKSP" }, { "Escape", "ESC" }, { "Return", "ENTER" }, { "NumpadEnter", "ENTER" },
        { "Tab", "TAB" }, { "Delete", "DEL" }, { "Insert", "INS" }, { "Home", "HOME" }, { "End", "END" }, { "PageUp", "PGUP" },
        { "PageDown", "PGDN" }, { "Shift", "SHIFT" }, { "Control", "CTRL" }, { "Alt", "ALT" }, { "Quote", "'" }, { "Comma", "," },
        { "Minus", "-" }, { "Period", "." }, { "Slash", "/" }, { "Semicolon", ";" }, { "Plus", "=" }, { "LBracket", "[" },
        { "Backslash", "\\" }, { "RBracket", "]" }, { "Backtick", "`" }, { "CapsLock", "CAPS" }, { "NumpadPlus", "N+" },
        { "NumpadMinus", "N-" }, { "NumpadStar", "N*" }, { "NumpadSlash", "N/" } };
    for (const auto &m : map) if (k == m.first) return m.second;
    if (k.rfind("Numpad", 0) == 0) return "N" + k.substr(6);
    std::string u = k;
    for (char &ch : u) ch = char(std::toupper((unsigned char)ch));
    return u;
}

// One cap, w x h texels (straight RGBA; alpha < 2 means "nothing of ours"), for key `k` ("LMB" / "RMB" / "MMB" = mouse).
std::vector<uint8_t> draw_cap(int w, int h, const std::string &k) {
    Canvas c(w * kSS, h * kSS);
    const float W = float(c.w), H = float(c.h), S = float(kSS);
    if (k == "LMB" || k == "RMB" || k == "MMB" || k == "MOUSE") {   // a mouse, the pressed button red (MOUSE: none)
        const float bw = std::min(W, H * 0.72f), bh = H * 0.92f, x0 = (W - bw) / 2, y0 = (H - bh) / 2;
        const float lw = std::max(2.0f, S);
        key_rect(c, x0, y0, x0 + bw, y0 + bh, bw / 2, 235 / 255.0f, 30 / 255.0f, lw);
        const float mid = x0 + bw / 2, top = y0 + bh * 0.45f;
        if (k == "MOUSE") {}
        else if (k == "MMB") fill_round_rect(c, mid - bw / 8, y0 + bh / 10, mid + bw / 8, top - bh / 10, bw / 10, 220 / 255.0f, 60 / 255.0f, 40 / 255.0f);
        else {
            const float hx0 = k == "LMB" ? x0 + lw : mid + lw / 2, hx1 = k == "LMB" ? mid - lw / 2 : x0 + bw - lw;
            for (int y = int(y0 + lw); y < int(top - lw / 2); ++y)
                for (int x = int(hx0); x < int(hx1); ++x)
                    if (in_round_rect(x + 0.5f, y + 0.5f, x0 + lw, y0 + lw, x0 + bw - lw, y0 + bh - lw, bw / 2 - lw))
                        c.blend(x, y, 220 / 255.0f, 60 / 255.0f, 40 / 255.0f, 1.0f);
        }
        for (int y = int(top - lw / 2); y < int(top + lw / 2); ++y)
            for (int x = int(x0); x < int(x0 + bw); ++x)
                if (in_round_rect(x + 0.5f, y + 0.5f, x0, y0, x0 + bw, y0 + bh, bw / 2)) c.blend(x, y, 30 / 255.0f, 30 / 255.0f, 30 / 255.0f, 1.0f);
        for (int y = int(y0); y < int(top); ++y)
            for (int x = int(mid - lw / 2); x < int(mid + lw / 2); ++x) c.blend(x, y, 30 / 255.0f, 30 / 255.0f, 30 / 255.0f, 1.0f);
    } else {
        const float m = std::floor(std::min(W, H) * 0.06f), r = std::floor(std::min(W, H) * 0.22f);
        fill_round_rect(c, m, m + S, W - m, H - m, r, 20 / 255.0f, 20 / 255.0f, 20 / 255.0f);   // shadow / side
        key_rect(c, m, m, W - m, H - m - 2 * S, r, 232 / 255.0f, 25 / 255.0f, std::max(2.0f, S));
        const float tx0 = m, ty0 = m, tx1 = W - m, ty1 = H - m - 2 * S, ink = 20 / 255.0f;
        const float cx = (tx0 + tx1) / 2, cy = (ty0 + ty1) / 2, a = (ty1 - ty0) * 0.24f;
        if (k == "Left") fill_triangle(c, cx - a, cy, cx + a * 0.8f, cy - a, cx + a * 0.8f, cy + a, ink);
        else if (k == "Right") fill_triangle(c, cx + a, cy, cx - a * 0.8f, cy - a, cx - a * 0.8f, cy + a, ink);
        else if (k == "Up") fill_triangle(c, cx, cy - a, cx - a, cy + a * 0.8f, cx + a, cy + a * 0.8f, ink);
        else if (k == "Down") fill_triangle(c, cx, cy + a, cx - a, cy - a * 0.8f, cx + a, cy - a * 0.8f, ink);
        else draw_text(c, !k.empty() && k[0] == '\x01' ? k.substr(1) : label_of(k), tx0, ty0, tx1, ty1, ink);
    }
    std::vector<uint8_t> out(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int sy = 0; sy < kSS; ++sy)
                for (int sx = 0; sx < kSS; ++sx) {
                    const float *p = &c.px[(size_t(y * kSS + sy) * c.w + x * kSS + sx) * 4];
                    for (int i = 0; i < 4; ++i) acc[i] += p[i];
                }
            const float n = float(kSS * kSS), a = acc[3] / n;
            uint8_t *o = &out[(size_t(y) * w + x) * 4];
            if (a * 255.0f < 2.0f) continue;   // nothing of ours here
            for (int i = 0; i < 3; ++i) o[i] = uint8_t(std::clamp(acc[i] / n / a * 255.0f + 0.5f, 0.0f, 255.0f));
            o[3] = uint8_t(std::clamp(a * 255.0f + 0.5f, 2.0f, 255.0f));
        }
    return out;
}

std::string compose(const std::string &promptDir, const std::string &game, const std::map<std::string, std::string> &keys) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(promptDir) / ("hires-" + game);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const int OW = 256 * kScale, OH = 256 * kScale;
    int written = 0;
    for (const Sheet &sh : kSheets) {
        std::vector<uint8_t> ov(size_t(OW) * OH * 4, 0);
        for (size_t i = 0; i < sh.count; ++i) {
            const Cell &cl = sh.cells[i];
            const int x0 = cl.x0 * kScale, y0 = cl.y0 * kScale, w = (cl.x1 - cl.x0) * kScale, h = (cl.y1 - cl.y0) * kScale;
            auto first = [&](const char *cv) {   // the first key bound to a button
                auto it = keys.find(cv);
                const std::string v = it != keys.end() ? it->second : "";
                return v.substr(0, v.find(','));
            };
            std::string k;
            const std::string cv = cl.cvar;
            if (cv == "dpad_vertical") k = "\x01" + label_of(first("keybind_dpad_up")) + "/" + label_of(first("keybind_dpad_down"));
            else if (cv == "dpad_horizontal") k = "\x01" + label_of(first("keybind_dpad_left")) + "/" + label_of(first("keybind_dpad_right"));
            else if (cv == "move_left") k = "\x01WASD";   // the launcher's fixed movement keys (W/A/S/D and the arrows)
            else if (cv == "move_right") k = "MOUSE";
            else k = first(cl.cvar);
            for (int y = y0; y < y0 + h; ++y)
                for (int x = x0; x < x0 + w; ++x) { uint8_t *p = &ov[(size_t(y) * OW + x) * 4]; p[0] = p[1] = p[2] = 0; p[3] = 1; }
            if (k.empty() || k == "None") continue;   // unbound: the button is just cleared
            const std::vector<uint8_t> cap = draw_cap(w, h, k);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint8_t *s = &cap[(size_t(y) * w + x) * 4];
                    if (s[3] >= 2) std::memcpy(&ov[(size_t(y0 + y) * OW + x0 + x) * 4], s, 4);
                }
        }
        std::ofstream f(dir / (std::string(sh.hash) + ".overlay"), std::ios::binary | std::ios::trunc);
        const uint32_t hw = OW, hh = OH;
        f.write("COGBOXOV", 8);
        f.write(reinterpret_cast<const char *>(&hw), 4);
        f.write(reinterpret_cast<const char *>(&hh), 4);
        f.write(reinterpret_cast<const char *>(ov.data()), std::streamsize(ov.size()));
        if (f) ++written;
    }
    return written == int(sizeof kSheets / sizeof kSheets[0]) ? dir.u8string() : "";
}

const bool g_registered = (g_prompt_composer = &compose, true);

}  // namespace
}  // namespace gears
}  // namespace cogbox
