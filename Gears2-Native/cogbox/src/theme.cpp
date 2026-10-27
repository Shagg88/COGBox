// COGBox - the look (see theme.h)
#include "theme.h"
#include "imgui_internal.h"
#include "gen/font_title.h"
#include "gen/font_head.h"
#include "gen/font_body.h"
#include "gen/font_bodyb.h"

#include <cmath>
#include <cstdint>
#include <algorithm>

float g_ui = 1.0f;
ImFont *F_BODY, *F_BOLD, *F_SMALL, *F_HEAD, *F_TITLE, *F_HUGE;

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
namespace col {
const ImVec4 text    = rgb(226, 221, 208);
const ImVec4 dim     = rgb(150, 146, 136);
const ImVec4 faint   = rgb(96, 94, 90);
const ImVec4 red     = rgb(158, 20, 26);
const ImVec4 redHi   = rgb(206, 34, 40);
const ImVec4 redDark = rgb(84, 10, 14);
const ImVec4 amber   = rgb(226, 160, 52);
const ImVec4 good    = rgb(124, 178, 98);
const ImVec4 bad     = rgb(224, 74, 64);
const ImVec4 steel   = rgb(34, 37, 41);
const ImVec4 steelHi = rgb(54, 58, 64);
const ImVec4 edge    = rgb(112, 118, 126);
const ImVec4 panel   = rgb(14, 15, 17, 0.90f);
const ImVec4 shadow  = rgb(0, 0, 0, 0.55f);
}

ImU32 U(const ImVec4 &c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

void theme_fonts(ImGuiIO &io)
{
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    static const ImWchar ranges[] = { 0x0020, 0x024F, 0x2000, 0x206F, 0x20AC, 0x20AC, 0x2122, 0x2122, 0x2190, 0x21FF, 0 };
    F_BODY  = io.Fonts->AddFontFromMemoryTTF((void *)font_body_data, sizeof font_body_data, T(18), &fc, ranges);
    F_BOLD  = io.Fonts->AddFontFromMemoryTTF((void *)font_bodyb_data, sizeof font_bodyb_data, T(18), &fc, ranges);
    F_SMALL = io.Fonts->AddFontFromMemoryTTF((void *)font_body_data, sizeof font_body_data, T(16.5f), &fc, ranges);
    F_HEAD  = io.Fonts->AddFontFromMemoryTTF((void *)font_head_data, sizeof font_head_data, T(22), &fc, ranges);
    F_TITLE = io.Fonts->AddFontFromMemoryTTF((void *)font_title_data, sizeof font_title_data, T(34), &fc, ranges);
    F_HUGE  = io.Fonts->AddFontFromMemoryTTF((void *)font_title_data, sizeof font_title_data, T(64), &fc, ranges);
    io.FontDefault = F_BODY;
    ImGui::GetStyle().FontSizeBase = F_BODY->LegacySize;
}

void theme_style()
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = S(2); s.GrabRounding = S(2); s.PopupRounding = S(2); s.ScrollbarRounding = S(2);
    s.TabRounding = 0; s.ChildRounding = 0;
    s.WindowBorderSize = 0; s.FrameBorderSize = 1; s.PopupBorderSize = 1;
    s.FramePadding = ImVec2(S(8), S(5)); s.ItemSpacing = ImVec2(S(10), S(8)); s.WindowPadding = ImVec2(S(14), S(12));
    s.ScrollbarSize = S(12); s.GrabMinSize = S(14);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = col::text;
    c[ImGuiCol_TextDisabled] = col::faint;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.075f, 0.085f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(col::edge.x, col::edge.y, col::edge.z, 0.45f);
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.10f, 0.11f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.19f, 0.21f, 1.0f);
    c[ImGuiCol_CheckMark] = col::redHi;
    c[ImGuiCol_SliderGrab] = col::red;
    c[ImGuiCol_SliderGrabActive] = col::redHi;
    c[ImGuiCol_Button] = col::steel;
    c[ImGuiCol_ButtonHovered] = col::steelHi;
    c[ImGuiCol_ButtonActive] = col::redDark;
    c[ImGuiCol_Header] = ImVec4(col::red.x, col::red.y, col::red.z, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(col::red.x, col::red.y, col::red.z, 0.75f);
    c[ImGuiCol_HeaderActive] = col::redHi;
    c[ImGuiCol_Separator] = ImVec4(col::edge.x, col::edge.y, col::edge.z, 0.35f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = col::steelHi;
    c[ImGuiCol_ScrollbarGrabHovered] = col::edge;
    c[ImGuiCol_ScrollbarGrabActive] = col::redHi;
    c[ImGuiCol_NavCursor] = col::redHi;
    c[ImGuiCol_PlotHistogram] = col::red;
    c[ImGuiCol_TextSelectedBg] = ImVec4(col::red.x, col::red.y, col::red.z, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

// ---- shapes ----------------------------------------------------------------------------------------------------------
static void chamfer_path(ImDrawList *dl, ImVec2 a, ImVec2 b, float c)
{
    dl->PathLineTo(ImVec2(a.x + c, a.y));
    dl->PathLineTo(ImVec2(b.x - c, a.y));
    dl->PathLineTo(ImVec2(b.x, a.y + c));
    dl->PathLineTo(ImVec2(b.x, b.y - c));
    dl->PathLineTo(ImVec2(b.x - c, b.y));
    dl->PathLineTo(ImVec2(a.x + c, b.y));
    dl->PathLineTo(ImVec2(a.x, b.y - c));
    dl->PathLineTo(ImVec2(a.x, a.y + c));
}

void th_plate_fill(ImDrawList *dl, ImVec2 a, ImVec2 b, float c, ImU32 fill)
{
    chamfer_path(dl, a, b, c);
    dl->PathFillConvex(fill);
}

void th_rivet(ImDrawList *dl, ImVec2 c, float r, float al)
{
    dl->AddCircleFilled(ImVec2(c.x + r * 0.3f, c.y + r * 0.4f), r, U(col::shadow, al), 12);
    dl->AddCircleFilled(c, r, U(ImVec4(0.30f, 0.32f, 0.35f, 1), al), 12);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.45f, U(ImVec4(0.62f, 0.64f, 0.67f, 1), al * 0.8f), 8);
}

void th_plate(ImDrawList *dl, ImVec2 a, ImVec2 b, float c, bool lit, float al)
{
    // drop shadow, body with a top-to-bottom shade, brushed lines, edges
    th_plate_fill(dl, ImVec2(a.x + S(3), a.y + S(4)), ImVec2(b.x + S(3), b.y + S(4)), c, U(col::shadow, al));
    ImVec4 top = lit ? col::steelHi : col::steel, bot = ImVec4(top.x * 0.62f, top.y * 0.62f, top.z * 0.62f, 1);
    int vtx0 = dl->VtxBuffer.Size;
    th_plate_fill(dl, a, b, c, IM_COL32(255, 255, 255, (int)(255 * al)));   // the gradient below keeps this alpha
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), U(top, al), U(bot, al));
    for (float y = a.y + S(3); y < b.y - S(2); y += S(3))
        dl->AddLine(ImVec2(a.x + c * 0.6f, y), ImVec2(b.x - c * 0.6f, y), IM_COL32(255, 255, 255, (int)(5 * al)));
    chamfer_path(dl, a, b, c);
    dl->PathStroke(U(lit ? col::redHi : col::edge, (lit ? 0.9f : 0.55f) * al), ImDrawFlags_Closed, S(1.2f));
    dl->AddLine(ImVec2(a.x + c, a.y + S(1)), ImVec2(b.x - c, a.y + S(1)), IM_COL32(255, 255, 255, (int)(40 * al)), S(1));
    float r = S(3.2f), in = c * 0.5f + S(7);
    if (b.x - a.x > 6 * in && b.y - a.y > 3 * in) {
        th_rivet(dl, ImVec2(a.x + in, a.y + in), r, al);
        th_rivet(dl, ImVec2(b.x - in, a.y + in), r, al);
        th_rivet(dl, ImVec2(a.x + in, b.y - in), r, al);
        th_rivet(dl, ImVec2(b.x - in, b.y - in), r, al);
    }
}

void th_hazard(ImDrawList *dl, ImVec2 a, ImVec2 b, float al)
{
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, U(ImVec4(0.08f, 0.08f, 0.08f, 1), al));
    float h = b.y - a.y, w = h * 0.9f;
    for (float x = a.x - h; x < b.x + h; x += 2 * w)
        dl->AddQuadFilled(ImVec2(x, b.y), ImVec2(x + w, b.y), ImVec2(x + w + h, a.y), ImVec2(x + h, a.y), U(ImVec4(0.80f, 0.62f, 0.10f, 1), al * 0.85f));
    dl->PopClipRect();
}

void th_cog(ImDrawList *dl, ImVec2 c, float r, int teeth, float ang, ImU32 colr, ImU32 hole)
{
    float inner = r * 0.78f, toothW = 3.14159265f / teeth * 0.95f;
    for (int i = 0; i < teeth; i++) {
        float t = ang + i * 2 * 3.14159265f / teeth;
        float t0 = t - toothW * 0.5f, t1 = t + toothW * 0.5f;
        ImVec2 p0(c.x + cosf(t0) * inner * 0.98f, c.y + sinf(t0) * inner * 0.98f);
        ImVec2 p1(c.x + cosf(t1) * inner * 0.98f, c.y + sinf(t1) * inner * 0.98f);
        ImVec2 p2(c.x + cosf(t1 - toothW * 0.12f) * r, c.y + sinf(t1 - toothW * 0.12f) * r);
        ImVec2 p3(c.x + cosf(t0 + toothW * 0.12f) * r, c.y + sinf(t0 + toothW * 0.12f) * r);
        dl->AddQuadFilled(p0, p1, p2, p3, colr);
    }
    dl->AddCircleFilled(c, inner, colr, 48);
    dl->AddCircleFilled(c, r * 0.32f, hole, 32);
    // four lightening holes between hub and rim
    for (int i = 0; i < 4; i++) {
        float t = ang + 0.785f + i * 1.5708f;
        dl->AddCircleFilled(ImVec2(c.x + cosf(t) * r * 0.55f, c.y + sinf(t) * r * 0.55f), r * 0.11f, hole, 16);
    }
}

void th_heading(const char *text, float x1)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(F_HEAD, F_HEAD->LegacySize);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.18f), ImVec2(p.x + S(4), p.y + h * 0.86f), U(col::redHi));
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y));
    ImGui::TextUnformatted(text);
    float tx = p.x + S(12) + ImGui::CalcTextSize(text).x + S(12);
    if (x1 > tx) dl->AddLine(ImVec2(tx, p.y + h * 0.55f), ImVec2(x1, p.y + h * 0.55f), U(col::edge, 0.35f), S(1));
    ImGui::PopFont();
}

static bool chamfer_button(const char *label, ImVec2 size, bool enabled, ImFont *font, bool red, bool selected)
{
    ImGui::PushID(label);
    if (!enabled) ImGui::BeginDisabled();
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##b", size, ImGuiButtonFlags_EnableNav);   // job 522: reachable by controller / keyboard (off by default since Dear ImGui 1.90)
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive(), focused = (ImGui::IsItemFocused() && GImGui->NavCursorVisible) || selected;
    if (!enabled) ImGui::EndDisabled();
    ImVec2 b(a.x + size.x, a.y + size.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float c = std::min(size.y * 0.32f, S(10)), al = enabled ? 1.0f : 0.45f;
    th_plate_fill(dl, ImVec2(a.x + S(2), a.y + S(3)), ImVec2(b.x + S(2), b.y + S(3)), c, U(col::shadow, al));
    ImVec4 top, bot;
    if (red) { top = held ? col::redDark : (hov || focused) ? col::redHi : col::red; bot = ImVec4(top.x * 0.55f, top.y * 0.55f, top.z * 0.55f, 1); }
    else { top = held ? col::steel : (hov || focused) ? ImVec4(0.27f, 0.29f, 0.32f, 1) : col::steelHi; bot = ImVec4(top.x * 0.6f, top.y * 0.6f, top.z * 0.6f, 1); }
    int v0 = dl->VtxBuffer.Size;
    th_plate_fill(dl, a, b, c, IM_COL32_WHITE);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), U(top, al), U(bot, al));
    chamfer_path(dl, a, b, c);
    dl->PathStroke(U(focused ? col::text : red ? ImVec4(1, 0.45f, 0.4f, 1) : col::edge, (focused ? 0.95f : 0.6f) * al), ImDrawFlags_Closed, S(focused ? 2.0f : 1.0f));
    dl->AddLine(ImVec2(a.x + c, a.y + S(1)), ImVec2(b.x - c, a.y + S(1)), IM_COL32(255, 255, 255, (int)(50 * al)), S(1));
    ImFont *f = font ? font : F_BOLD;
    ImGui::PushFont(f, f->LegacySize);
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    ImVec2 tp(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f + (held ? S(1) : 0));
    dl->AddText(ImVec2(tp.x + S(1), tp.y + S(1)), U(col::shadow, al), label, end);
    dl->AddText(tp, U(col::text, al), label, end);
    ImGui::PopFont();
    ImGui::PopID();
    return clicked && enabled;
}

bool th_button_red(const char *label, ImVec2 size, bool enabled, ImFont *font, bool selected) { return chamfer_button(label, size, enabled, font, true, selected); }
bool th_button(const char *label, ImVec2 size, bool enabled, ImFont *font, bool selected) { return chamfer_button(label, size, enabled, font, false, selected); }

bool th_tab(const char *label, bool selected, ImVec2 size)
{
    ImGui::PushID(label);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##t", size);
    bool hov = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    ImVec2 b(a.x + size.x, a.y + size.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float c = S(8);
    dl->PathLineTo(ImVec2(a.x, b.y));
    dl->PathLineTo(ImVec2(a.x, a.y + c));
    dl->PathLineTo(ImVec2(a.x + c, a.y));
    dl->PathLineTo(ImVec2(b.x - c, a.y));
    dl->PathLineTo(ImVec2(b.x, a.y + c));
    dl->PathLineTo(ImVec2(b.x, b.y));
    dl->PathFillConvex(U(selected ? col::steelHi : hov ? col::steel : ImVec4(0.09f, 0.10f, 0.11f, 0.9f)));
    if (selected) dl->AddRectFilled(ImVec2(a.x + c, a.y), ImVec2(b.x - c, a.y + S(3)), U(col::redHi));
    if (focused) dl->AddRect(a, b, U(col::text, 0.8f), 0, 0, S(1.5f));
    ImGui::PushFont(F_HEAD, F_HEAD->LegacySize * 0.86f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f + S(1)), U(selected ? col::text : col::dim), label);
    ImGui::PopFont();
    ImGui::PopID();
    return clicked;
}

void th_lamp(ImDrawList *dl, ImVec2 c, float r, int state)
{
    ImVec4 k = state == 1 ? col::good : state == 2 ? col::amber : state == 3 ? col::bad : col::faint;
    if (state) dl->AddCircleFilled(c, r * 2.0f, U(k, 0.18f), 16);
    dl->AddCircleFilled(c, r, U(k), 16);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.35f, IM_COL32(255, 255, 255, 90), 8);
    dl->AddCircle(c, r, U(col::shadow), 16, S(1));
}

// ---- the background ------------------------------------------------------------------------------------------------
static float hash1(float x) { float s = sinf(x * 127.1f) * 43758.5453f; return s - floorf(s); }

// One layer of broken buildings: blocks of varying height, some with sheared tops, window holes, a few spires.
static void skyline(ImDrawList *dl, ImVec2 size, float base, float hmax, float seed, ImU32 fill, ImU32 holes, float drift)
{
    float x = -fmodf(drift, S(400)) - S(400);
    int i = (int)floorf(drift / S(400)) * 7;
    while (x < size.x + S(50)) {
        float r = hash1(seed + i * 1.37f), w = S(40) + r * S(90), h = hmax * (0.35f + 0.65f * hash1(seed + i * 2.11f));
        float top = base - h, shear = (hash1(seed + i * 3.7f) - 0.5f) * h * 0.5f;
        // the block, its top broken at a slant
        dl->AddQuadFilled(ImVec2(x, base), ImVec2(x + w, base), ImVec2(x + w, top + std::max(0.0f, shear)), ImVec2(x, top + std::max(0.0f, -shear)), fill);
        if (hash1(seed + i * 5.3f) > 0.72f)   // a spire or a broken girder
            dl->AddRectFilled(ImVec2(x + w * 0.4f, top - h * 0.25f), ImVec2(x + w * 0.4f + S(4), top + S(2)), fill);
        if (holes)
            for (float wy = top + S(14) + std::fabs(shear); wy < base - S(10); wy += S(14))
                for (float wx = x + S(8); wx < x + w - S(10); wx += S(13))
                    if (hash1(wx * 0.37f + wy * 0.11f + seed) > 0.55f) dl->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + S(5), wy + S(7)), holes);
        x += w + S(4) + hash1(seed + i * 9.1f) * S(30);
        i++;
    }
    dl->AddRectFilled(ImVec2(0, base), ImVec2(size.x, size.y), fill);
}

void th_background(ImDrawList *dl, ImVec2 size, double t, ImVec4 tint)
{
    // sky: near black at the top to the game's colour at the horizon (smoke-lit)
    ImVec4 hz(0.10f + tint.x * 0.22f, 0.10f + tint.y * 0.22f, 0.11f + tint.z * 0.22f, 1);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, U(ImVec4(0.02f, 0.02f, 0.025f, 1)), U(ImVec4(0.02f, 0.02f, 0.025f, 1)), U(hz), U(hz));
    // a low, dull sun / fire glow behind the city
    ImVec2 glow(size.x * 0.68f, size.y * 0.62f);
    for (int i = 6; i >= 1; i--)
        dl->AddCircleFilled(glow, size.y * 0.09f * i, U(ImVec4(tint.x, tint.y * 0.8f, tint.z * 0.6f, 1), 0.035f), 48);
    float drift = (float)t * S(4);
    skyline(dl, size, size.y * 0.80f, size.y * 0.38f, 11.0f, U(ImVec4(0.07f + tint.x * 0.06f, 0.07f + tint.y * 0.06f, 0.08f + tint.z * 0.06f, 1)), 0, drift * 0.3f);
    skyline(dl, size, size.y * 0.86f, size.y * 0.30f, 23.0f, U(ImVec4(0.045f, 0.045f, 0.05f, 1)), U(ImVec4(tint.x, tint.y * 0.7f, 0.1f, 1), 0.10f), drift * 0.6f);
    skyline(dl, size, size.y * 0.94f, size.y * 0.16f, 37.0f, U(ImVec4(0.02f, 0.02f, 0.022f, 1)), 0, drift);
    // haze over the lower half
    dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.55f), size, U(hz, 0.0f), U(hz, 0.0f), U(hz, 0.35f), U(hz, 0.35f));
    // ash falling and embers rising
    for (int i = 0; i < 90; i++) {
        float sx = hash1(i * 1.7f), sp = 0.3f + hash1(i * 3.1f) * 0.7f;
        bool ember = i % 6 == 0;
        float y = ember ? fmodf(1.0f - (float)t * 0.02f * sp - hash1(i * 5.3f), 1.0f) : fmodf((float)t * 0.012f * sp + hash1(i * 5.3f), 1.0f);
        if (y < 0) y += 1;
        float x = fmodf(sx + sinf((float)t * 0.2f + i) * 0.01f + (float)t * 0.004f * sp, 1.0f);
        ImVec2 p(x * size.x, y * size.y);
        if (ember) dl->AddCircleFilled(p, S(1.4f) * sp, U(ImVec4(1.0f, 0.55f, 0.15f, 1), 0.55f * (1 - y * 0.6f)), 6);
        else dl->AddCircleFilled(p, S(1.1f) * sp, U(ImVec4(0.75f, 0.73f, 0.70f, 1), 0.22f), 6);
    }
    // vignette
    float v = std::min(size.x, size.y) * 0.25f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(v, size.y), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));
    dl->AddRectFilledMultiColor(ImVec2(size.x - v, 0), size, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0));
}
