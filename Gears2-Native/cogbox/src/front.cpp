// COGBox - the front screen: the three games as boxes on a turning carousel, with Play and Settings under the one in
// front. Each box is drawn flat, then every vertex of it is carried through a small 3D turn and perspective, so the
// cover, its spine and its reflection on the floor bend together.
#include "app.h"
#include "theme.h"
#include "i18n.h"   // 0.2.0
#include "update.h"
#include "imgui_internal.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

static const float kCoverAspect = 0.70f;      // an Xbox 360 case: about 0.70 wide per 1 high
static const float kDepth = 0.075f;           // the case's thickness, per height

// ---- covers ----------------------------------------------------------------------------------------------------------
static SDL_Texture *load_texture(SDL_Renderer *ren, const std::string &path, int &w, int &h)
{
    std::string data;
    if (!read_file(path, data, 32u << 20)) return nullptr;
    int n = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!px) return nullptr;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px, w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(px);
    return t;
}

// covers\gears2.png (or .jpg): the player's own scan, which wins; covers\gears2-boxart.jpg / gears2-background.jpg: the
// art "Get box art" downloads.
void covers_load(App &a)
{
    std::string dir = join(exe_dir(), "covers");
    for (int g = 0; g < G_COUNT; g++) {
        if (a.cover[g].tex) { SDL_DestroyTexture(a.cover[g].tex); a.cover[g] = Cover(); }
        if (a.backdrop[g].tex) { SDL_DestroyTexture(a.backdrop[g].tex); a.backdrop[g] = Cover(); }
        std::string k = game_def(g).key;
        for (const char *n : { ".png", ".jpg", ".jpeg", "-boxart.jpg" }) {
            std::string p = join(dir, k + n);
            if (!file_exists(p)) continue;
            a.cover[g].tex = load_texture(a.ren, p, a.cover[g].w, a.cover[g].h);
            if (a.cover[g].tex) break;
        }
        for (const char *n : { "-background.png", "-background.jpg" }) {
            std::string p = join(dir, k + n);
            if (!file_exists(p)) continue;
            a.backdrop[g].tex = load_texture(a.ren, p, a.backdrop[g].w, a.backdrop[g].h);
            if (a.backdrop[g].tex) break;
        }
    }
}

// ---- Get box art: the Xbox Marketplace's box art and background for each game, from the x360db archive on GitHub
// (github.com/xenia-manager/x360db - the archive Xenia Manager uses). Only on the player's request; nothing is sent.
#include <winhttp.h>
static std::atomic<int> g_artState{0};   // 0 idle, 1 working, 2 done, 3 failed
static bool http_get_raw(const std::wstring &path, std::string &out)
{
    out.clear();
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"COGBox launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 8000, 8000, 10000, 20000);
    HINTERNET c = WinHttpConnect(s, L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            char buf[65536];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &got)) { ok = false; break; }
                if (!got) break;
                out.append(buf, got);
                if (out.size() > (16u << 20)) { ok = false; break; }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok && out.size() > 1000;
}

void art_download_start()
{
    if (g_artState.load() == 1) return;
    g_artState = 1;
    std::thread([] {
        std::string dir = join(exe_dir(), "covers");
        make_dirs(dir);
        int got = 0;
        for (int g = 0; g < G_COUNT; g++) {
            char tid[16];
            snprintf(tid, sizeof tid, "%08X", game_def(g).titleId);
            for (const char *n : { "boxart.jpg", "background.jpg" }) {
                std::string data;
                if (http_get_raw(L"/xenia-manager/x360db/HEAD/titles/" + widen(tid) + L"/artwork/" + widen(n), data) &&
                    write_file(join(dir, std::string(game_def(g).key) + "-" + n), data)) got++;
            }
        }
        g_artState = got ? 2 : 3;
    }).detach();
}

int art_download_poll()   // 2 done, 3 failed (each reported once), else 0 / 1
{
    int s = g_artState.load();
    if (s >= 2) g_artState = 0;
    return s;
}

// ---- the 3D carry ----------------------------------------------------------------------------------------------------
struct Proj {
    ImVec2 c;          // the box's centre on screen
    float H;           // its height on screen when facing straight on
    float yaw;         // radians; > 0 turns its right edge away
    float f;           // focal length
    ImVec2 flat0;      // where the flat drawing was made (top-left), and its size
    ImVec2 flatSize;
    ImVec2 map(float u, float v, float z) const   // u, v in -0.5..0.5 of the cover; z towards the back, per height
    {
        float x = u * H * kCoverAspect, zz = z * H;
        float xr = x * cosf(yaw) + zz * sinf(yaw), zr = -x * sinf(yaw) + zz * cosf(yaw);
        float k = f / (f + zr);
        return ImVec2(c.x + xr * k, c.y + v * H * k);
    }
};

// Carries the vertices [v0, end) drawn flat in p.flat0..+flatSize onto the face at depth z; 'fade' (reflection)
// multiplies alpha by how far down the cover a vertex is.
static void carry(ImDrawList *dl, int v0, const Proj &p, float z, bool mirror, float fadeTop, float fadeBottom)
{
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert &vt = dl->VtxBuffer[i];
        float u = (vt.pos.x - p.flat0.x) / p.flatSize.x - 0.5f, v = (vt.pos.y - p.flat0.y) / p.flatSize.y - 0.5f;
        if (mirror) {
            float t = v + 0.5f;   // 0 at the cover's top .. 1 at its bottom; the mirror shows the bottom nearest the floor
            float al = fadeBottom + (fadeTop - fadeBottom) * (1.0f - t);
            al = std::max(0.0f, std::min(1.0f, al));
            vt.col = (vt.col & 0x00FFFFFF) | ((ImU32)(((vt.col >> 24) & 0xFF) * al) << 24);
            v = 1.0f - v;      // below the box: its bottom edge touches the box's
        }
        vt.pos = p.map(u, v, z);
    }
}

// The cover drawn flat in a w x h rectangle at 'o': the player's scan, or one made from shapes and type.
static void draw_cover_flat(App &a, ImDrawList *dl, int g, ImVec2 o, float w, float h, float light)
{
    const Cover &cv = a.cover[g];
    ImU32 shade = U(ImVec4(light, light, light, 1));
    if (cv.tex) {
        // thin vertical strips, so the picture bends with the turn instead of skewing as two triangles
        const int strips = 24;
        for (int i = 0; i < strips; i++) {
            float u0 = (float)i / strips, u1 = (float)(i + 1) / strips;
            for (int j = 0; j < 4; j++) {
                float t0 = j / 4.0f, t1 = (j + 1) / 4.0f;
                dl->AddImage((ImTextureID)(intptr_t)cv.tex, ImVec2(o.x + u0 * w, o.y + t0 * h), ImVec2(o.x + u1 * w, o.y + t1 * h),
                             ImVec2(u0, t0), ImVec2(u1, t1), shade);
            }
        }
        return;
    }
    const GameDef &d = game_def(g);
    static const ImVec4 tints[G_COUNT] = { ImVec4(0.20f, 0.30f, 0.42f, 1), ImVec4(0.42f, 0.30f, 0.16f, 1), ImVec4(0.40f, 0.12f, 0.10f, 1) };
    ImVec4 t = tints[g];
    auto L = [&](ImVec4 c) { return U(ImVec4(c.x * light, c.y * light, c.z * light, c.w)); };
    ImVec2 b(o.x + w, o.y + h);
    // the case's band across the top (in the case's green) and the cover below it
    float band = h * 0.075f;
    dl->AddRectFilledMultiColor(ImVec2(o.x, o.y + band), b, L(ImVec4(t.x * 0.55f, t.y * 0.55f, t.z * 0.55f, 1)), L(ImVec4(t.x * 0.55f, t.y * 0.55f, t.z * 0.55f, 1)),
                                L(ImVec4(0.03f, 0.03f, 0.035f, 1)), L(ImVec4(0.03f, 0.03f, 0.035f, 1)));
    // the city burning on the horizon, from the background's own shapes
    for (int i = 0; i < 14; i++) {
        float x0 = o.x + w * (i / 14.0f), bw = w / 14.0f * (0.7f + 0.3f * sinf(i * 3.1f + g));
        float bh = h * (0.08f + 0.14f * (0.5f + 0.5f * sinf(i * 1.7f + g * 2.3f)));
        dl->AddRectFilled(ImVec2(x0, o.y + h * 0.80f - bh), ImVec2(x0 + bw, o.y + h * 0.80f), L(ImVec4(0.02f, 0.02f, 0.025f, 1)));
    }
    dl->AddRectFilled(ImVec2(o.x, o.y + h * 0.80f), b, L(ImVec4(0.02f, 0.02f, 0.025f, 1)));
    dl->AddRectFilledMultiColor(ImVec2(o.x, o.y + h * 0.62f), ImVec2(b.x, o.y + h * 0.80f), L(ImVec4(t.x, t.y, t.z, 0.0f)), L(ImVec4(t.x, t.y, t.z, 0.0f)),
                                L(ImVec4(t.x * 1.6f, t.y * 1.2f, t.z * 0.8f, 0.55f)), L(ImVec4(t.x * 1.6f, t.y * 1.2f, t.z * 0.8f, 0.55f)));
    // a big machine cog behind the title
    th_cog(dl, ImVec2(o.x + w * 0.5f, o.y + h * 0.43f), w * 0.40f, 12, 0.13f * g, L(ImVec4(0.55f, 0.56f, 0.58f, 0.22f)), L(ImVec4(0.03f, 0.03f, 0.035f, 0.5f)));
    dl->AddRectFilledMultiColor(ImVec2(o.x, o.y), ImVec2(b.x, o.y + band), L(ImVec4(0.30f, 0.62f, 0.14f, 1)), L(ImVec4(0.30f, 0.62f, 0.14f, 1)),
                                L(ImVec4(0.16f, 0.40f, 0.07f, 1)), L(ImVec4(0.16f, 0.40f, 0.07f, 1)));
    dl->AddLine(ImVec2(o.x, o.y + band), ImVec2(b.x, o.y + band), L(ImVec4(0.85f, 0.88f, 0.8f, 0.7f)), h * 0.004f);
    // the title: "GEARS OF WAR" and the big number / word (plain type, no logo)
    const char *top = "GEARS OF WAR", *big = g == G_GEARS2 ? "2" : g == G_GEARS3 ? "3" : "JUDGMENT";
    float sTop = h * 0.075f, sBig = g == G_JUDGMENT ? h * 0.13f : h * 0.30f;
    ImVec2 st = F_TITLE->CalcTextSizeA(sTop, FLT_MAX, 0, top);
    float topY = o.y + h * (g == G_JUDGMENT ? 0.26f : 0.18f);
    dl->AddText(F_TITLE, sTop, ImVec2(o.x + (w - st.x) * 0.5f + h * 0.004f, topY + h * 0.004f), IM_COL32(0, 0, 0, 160), top);
    dl->AddText(F_TITLE, sTop, ImVec2(o.x + (w - st.x) * 0.5f, topY), L(ImVec4(0.90f, 0.88f, 0.84f, 1)), top);
    ImVec2 sb = F_TITLE->CalcTextSizeA(sBig, FLT_MAX, 0, big);
    if (sb.x > w * 0.9f) { sBig *= w * 0.9f / sb.x; sb = F_TITLE->CalcTextSizeA(sBig, FLT_MAX, 0, big); }
    float bigY = topY + st.y * 1.05f;
    dl->AddText(F_TITLE, sBig, ImVec2(o.x + (w - sb.x) * 0.5f + h * 0.006f, bigY + h * 0.006f), IM_COL32(0, 0, 0, 170), big);
    dl->AddText(F_TITLE, sBig, ImVec2(o.x + (w - sb.x) * 0.5f, bigY), L(ImVec4(0.66f, 0.07f, 0.09f, 1)), big);
    // a hazard strip and the year at the foot
    th_hazard(dl, ImVec2(o.x, o.y + h * 0.93f), ImVec2(b.x, o.y + h * 0.945f), light * 0.8f);
    float sy = h * 0.035f;
    ImVec2 ys = F_HEAD->CalcTextSizeA(sy, FLT_MAX, 0, d.year);
    dl->AddText(F_HEAD, sy, ImVec2(b.x - ys.x - w * 0.05f, o.y + h * 0.87f), L(ImVec4(0.7f, 0.7f, 0.68f, 1)), d.year);
    dl->AddText(F_HEAD, sy, ImVec2(o.x + w * 0.05f, o.y + h * 0.87f), L(ImVec4(0.7f, 0.7f, 0.68f, 1)), "PC PORT");
    dl->AddRect(o, b, L(ImVec4(1, 1, 1, 0.08f)), 0, 0, h * 0.003f);
}

void draw_box(App &a, int g, ImVec2 center, float H, float yawDeg, float light, bool reflection)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    Proj p;
    p.c = center; p.H = H; p.yaw = yawDeg * 3.14159265f / 180.0f; p.f = H * 2.6f;
    float w = H * kCoverAspect;
    // the flat drawing is made off to one side at the box's own size (the carry moves it into place)
    p.flat0 = ImVec2(center.x - w * 0.5f, center.y - H * 0.5f);
    p.flatSize = ImVec2(w, H);

    // reflection on the floor first (under everything)
    if (reflection) {
        int v0 = dl->VtxBuffer.Size;
        draw_cover_flat(a, dl, g, p.flat0, w, H, light * 0.5f);
        carry(dl, v0, p, 0, true, 0.0f, 0.28f);
    }
    // the spine: on whichever side faces the viewer
    bool rightSide = p.yaw < 0;   // turned left: its right edge comes forward and the right spine shows
    if (fabsf(p.yaw) > 0.02f) {
        float u = rightSide ? 0.5f : -0.5f;
        ImVec2 q0 = p.map(u, -0.5f, 0), q1 = p.map(u, -0.5f, kDepth), q2 = p.map(u, 0.5f, kDepth), q3 = p.map(u, 0.5f, 0);
        ImU32 sc = U(ImVec4(0.06f * light, 0.07f * light, 0.06f * light, 1));
        dl->AddQuadFilled(q0, q1, q2, q3, sc);
        // the green band runs round the spine too
        ImVec2 b0 = p.map(u, -0.5f, 0), b1 = p.map(u, -0.5f, kDepth), b2 = p.map(u, -0.425f, kDepth), b3 = p.map(u, -0.425f, 0);
        dl->AddQuadFilled(b0, b1, b2, b3, U(ImVec4(0.22f * light, 0.48f * light, 0.10f * light, 1)));
        dl->AddLine(q0, q3, U(ImVec4(1, 1, 1, 0.12f * light)), S(1));
    }
    // the cover
    int v0 = dl->VtxBuffer.Size;
    draw_cover_flat(a, dl, g, p.flat0, w, H, light);
    carry(dl, v0, p, 0, false, 1, 1);
    // a sheen across the plastic sleeve
    ImVec2 s0 = p.map(-0.5f, -0.5f, 0), s1 = p.map(0.5f, -0.5f, 0), s2 = p.map(0.5f, 0.5f, 0), s3 = p.map(-0.5f, 0.5f, 0);
    (void)s2;
    dl->AddQuadFilled(s0, ImVec2(s0.x + (s1.x - s0.x) * 0.45f, s0.y + (s1.y - s0.y) * 0.45f),
                      ImVec2(s3.x + (s1.x - s0.x) * 0.15f, s3.y), s3, IM_COL32(255, 255, 255, (int)(10 * light)));
}

// ---- the carousel ----------------------------------------------------------------------------------------------------
static float wrap_d(float d)   // -1.5 .. 1.5 for three boxes
{
    while (d > 1.5f) d -= 3;
    while (d < -1.5f) d += 3;
    return d;
}

static void turn(App &a, int dir)
{
    a.front = ((a.front + dir) % G_COUNT + G_COUNT) % G_COUNT;
    a.ini.seti("Front", "Box", a.front);
}

static std::string last_played_text(int64_t t)
{
    if (t <= 0) return "";
    int64_t ago = unix_now() - t;
    if (ago < 3600) return "played just now";
    if (ago < 86400) return "played today";
    if (ago < 2 * 86400) return "played yesterday";
    char b[64];
    snprintf(b, sizeof b, "played %d days ago", (int)(ago / 86400));
    return b;
}

void backdrop_draw(App &a, ImDrawList *dl, ImVec2 size, int g, float alpha)
{
    const Cover &b = a.backdrop[g];
    if (!b.tex || alpha <= 0) return;
    float s = std::max(size.x / b.w, size.y / b.h), w = b.w * s, h = b.h * s;
    ImVec2 p((size.x - w) * 0.5f, (size.y - h) * 0.5f);
    int v = (int)(255 * alpha);
    dl->AddImage((ImTextureID)(intptr_t)b.tex, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(150, 150, 150, v));
}

void front_draw(App &a, ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiIO &io = ImGui::GetIO();
    double now = ImGui::GetTime();
    // input: arrows / D-pad / stick / wheel turn it (not while a box or menu has the keyboard)
    bool nav = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !a.modal.open && a.job.state.load() != 1 &&
               a.zoomTo == SCR_FRONT;   // not while zooming into a page
    if (nav) {
        // Shagg 21:44: the D-pad (and left stick, arrow keys, mouse wheel, a click on a side box) turns the carousel; LB / RB
        // choose between Play and Settings under the box (a.frontSel), A presses the chosen one
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) turn(a, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) turn(a, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) a.frontSel = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) a.frontSel = 1;
        if (ImGui::IsWindowHovered() && io.MouseWheel != 0) turn(a, io.MouseWheel > 0 ? -1 : 1);
    }
    // ease the short way round
    float target = (float)a.front, d = wrap_d(target - a.carPos);
    float step = std::min(1.0f, io.DeltaTime * 9.0f);
    if (fabsf(d) < 0.002f) { if (a.carPos != target) a.restSince = now; a.carPos = target; }
    else a.carPos += d * step;
    a.carPos = fmodf(a.carPos + 3, 3);

    // the corner mark
    {
        ImVec2 m(S(26), S(20));
        th_cog(dl, ImVec2(m.x + S(16), m.y + S(18)), S(16), 10, (float)now * 0.15f, U(col::redHi), U(ImVec4(0.03f, 0.03f, 0.035f, 1)));
        dl->AddText(F_TITLE, FS(F_TITLE), ImVec2(m.x + S(42), m.y), U(col::text), "COGBOX");
        dl->AddText(F_SMALL, FS(F_SMALL), ImVec2(m.x + S(44), m.y + FS(F_TITLE) + S(2)), U(col::dim), (tr(std::string("Gears of War native PC ports  -  launcher")) + " " COGBOX_VERSION).c_str());
    }

    // the boxes, farthest first
    // the box gives up height to the bigger text under it (name, status line, buttons, hint line)
    float under = FS(F_HEAD) * 1.25f + FS(F_BODY) + T(36) + FS(F_SMALL) + S(60);
    float H = std::min(std::min(size.y * 0.56f, size.y - under - S(100)), size.x * 0.33f / kCoverAspect);
    float cx = size.x * 0.5f, cy = S(90) + H * 0.5f, spacing = H * kCoverAspect * 1.25f;
    int order[G_COUNT];
    for (int i = 0; i < G_COUNT; i++) order[i] = i;
    std::sort(order, order + G_COUNT, [&](int x, int y) { return fabsf(wrap_d(x - a.carPos)) > fabsf(wrap_d(y - a.carPos)); });
    for (int k = 0; k < G_COUNT; k++) {
        int g = order[k];
        float dd = wrap_d(g - a.carPos), ad = std::min(1.0f, fabsf(dd));
        float scale = 1.0f - 0.30f * ad, h = H * scale;
        float yaw = std::max(-1.0f, std::min(1.0f, dd)) * 35.0f;
        float light = 1.0f - 0.45f * ad;
        bool ready = a.st[g].ready == R_READY;
        if (!ready) light *= 0.55f;
        ImVec2 c(cx + dd * spacing, cy + (H - h) * 0.5f);
        // a click on a side box turns to it; on the front box, plays (when ready) or opens the page
        ImVec2 r0(c.x - h * kCoverAspect * 0.5f, c.y - h * 0.5f), r1(c.x + h * kCoverAspect * 0.5f, c.y + h * 0.5f);
        bool hov = nav && ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsWindowHovered();
        if (hov && ad < 0.1f) light = std::min(1.0f, light * 1.12f + 0.04f);
        int v0 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        draw_box(a, g, c, h, yaw, light, true);
        // boxes not in front: see-through (Shagg 18:41), more so the further round they are
        float alpha = 1.0f - 0.45f * ad;
        ImDrawList *bdl = ImGui::GetWindowDrawList();
        for (int vi = v0; alpha < 0.999f && vi < bdl->VtxBuffer.Size; vi++) {
            ImU32 &c32 = bdl->VtxBuffer[vi].col;
            c32 = (c32 & 0x00FFFFFF) | ((ImU32)(((c32 >> 24) & 0xFF) * alpha) << 24);
        }
        if (hov && ImGui::IsMouseClicked(0)) {
            if (ad > 0.5f) turn(a, dd > 0 ? 1 : -1);
        }
        if (hov && ad < 0.1f && ImGui::IsMouseDoubleClicked(0) && ready) play_start(a, g);
    }

    // under the front box: what the player has, play time, then Play and Settings
    int g = a.front;
    const GameDef &gd = game_def(g);
    GameState &st = a.st[g];
    float y = cy + H * 0.5f + S(14);
    auto centred = [&](ImFont *f, float fs, ImVec4 c, const std::string &t, float yy) {
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str());
        dl->AddText(f, fs, ImVec2(cx - ts.x * 0.5f, yy), U(c), t.c_str());
        return ts.y;
    };
    y += centred(F_HEAD, FS(F_HEAD) * 1.25f, col::text, gd.name, y) + S(2);
    std::string line;
    switch (st.ready) {
    case R_READY:
        line = st.useUpdate ? std::string(gd.updateName) : "Disc version";
        if (st.playSeconds > 0) line += "   |   " + human_duration(st.playSeconds) + " played";
        if (st.lastPlayed > 0) line += ", " + last_played_text(st.lastPlayed);
        if (st.xexKnown && !st.xexMatch) line += "   |   a different release of the game";
        break;
    case R_NO_COPY: line = gd.tagline; break;
    case R_COPY_MISSING: line = "Your copy isn't where it was - is its drive connected?"; break;
    case R_NEED_UPDATE: line = std::string("Needs ") + gd.updateName + " - add it from your console or its package file"; break;
    case R_NO_PORT: line = "Not in this release of COGBox yet"; break;
    }
    y += centred(F_BODY, FS(F_BODY), st.ready == R_READY ? col::dim : col::amber, tr(line), y) + S(14);   // 0.2.0

    float bw = T(140), bh = T(36), gap = S(14);
    bool running = a.run.proc != nullptr, busy = a.job.state.load() == 1;
    // the controller's choice shows only while a controller is connected; A presses it
    bool sel0 = a.padConnected && a.frontSel == 0, sel1 = a.padConnected && a.frontSel == 1;
    bool padA = nav && a.padConnected && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw - gap * 0.5f, y));
    if (st.ready == R_READY) {
        // A presses the choice LB / RB made (a.frontSel); Enter plays
        if (th_button_red(running ? "RUNNING" : "PLAY", ImVec2(bw, bh), !running && !busy, F_HEAD, sel0) ||
            (nav && !running && !busy && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || (padA && a.frontSel == 0))))
            play_start(a, g);
    } else if (st.ready != R_NO_PORT) {
        bool addHit = th_button_red(st.ready == R_NEED_UPDATE ? "ADD UPDATE" : "ADD YOUR COPY", ImVec2(bw, bh), !busy, F_HEAD, sel0) ||
                      (nav && !busy && padA && a.frontSel == 0);
        if (addHit) {
            std::string p = pick_file(L"Choose your copy: a disc image (.iso), a Games on Demand file, a title update, or default.xex in a folder",
                                      L"Xbox 360 games and updates\0*.iso;*.xex;*.*\0All files\0*.*\0");
            if (!p.empty()) install_start(a, p, g);
        }
    } else {
        th_button_red("PLAY", ImVec2(bw, bh), false, F_HEAD, sel0);
    }
    ImGui::SameLine(0, gap);
    if (th_button("SETTINGS", ImVec2(bw, bh), st.ready != R_NO_PORT, F_HEAD, sel1) ||
        (nav && st.ready != R_NO_PORT && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (padA && a.frontSel == 1)))) {
        a.page = g; a.zoomTo = SCR_PAGE; a.zoomStart = now; a.tab = 0; a.barSel = -1;
    }
    // the side buttons: add a folder instead of a file
    if (st.ready != R_READY && st.ready != R_NO_PORT) {
        ImGui::SetCursorScreenPos(ImVec2(cx - S(130), y + bh + S(12)));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        if (ImGui::Button("...or a folder you already extracted", ImVec2(S(260), 0)) && !busy) {
            std::string p = pick_folder(L"Choose the folder with your game's default.xex (or a Games on Demand / title update folder)");
            if (!p.empty()) install_start(a, p, g);
        }
        ImGui::PopStyleColor(2);
    }

    // Get box art (Y): top right
    float artW = 0;
    {
        const char *lbl = "GET BOX ART";
        bool any = false;
        for (int i = 0; i < G_COUNT; i++) any |= a.cover[i].tex != nullptr;
        ImVec2 bs(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, any ? "UPDATE ART" : lbl).x + S(36), T(28));
        artW = bs.x;
        ImGui::SetCursorScreenPos(ImVec2(size.x - bs.x - S(24), S(22)));
        if (th_button(any ? "UPDATE ART" : lbl, bs, true, F_BOLD) || (nav && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))) {
            art_download_start();
            set_message(a, "Getting the box art and backgrounds from the x360db archive on GitHub...", false);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Downloads each game's Xbox Marketplace box art and background from the x360db archive on GitHub\n(the one Xenia Manager uses) into the covers folder. Your own covers\\gears2.png etc. still win.");
    }
    // a newer COGBox on GitHub: a small notice beside the box art button; it opens this game's About tab, where Update is
    // (Shagg 21:57). COGBOX_SHOW_UPDATE=<version> shows it without a release (test switch, off for players).
    {
        std::string newer = update_newer();
        if (newer.empty() && getenv("COGBOX_SHOW_UPDATE")) newer = getenv("COGBOX_SHOW_UPDATE");
        if (!newer.empty() && st.ready != R_NO_PORT) {
            std::string lbl = "UPDATE TO " + newer;
            ImVec2 us(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, lbl.c_str()).x + S(36), T(28));
            ImGui::SetCursorScreenPos(ImVec2(size.x - artW - S(24) - S(12) - us.x, S(22)));
            if (th_button_red(lbl.c_str(), us, true, F_BOLD)) { a.page = g; a.zoomTo = SCR_PAGE; a.zoomStart = now; a.tab = 4; a.barSel = -1; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("COGBox %s is out (this is " COGBOX_VERSION "). Opens the About tab, where Update downloads and installs it.", newer.c_str());
        }
    }
    // 0.2.0 (GitHub #1): once per session, when the carousel stops on Judgment and it has no Title Update 4
    {
        static bool toldTu4 = false;
        if (!toldTu4 && a.front == G_JUDGMENT && a.carPos == (float)G_JUDGMENT && a.st[G_JUDGMENT].ready == R_NEED_UPDATE &&
            !a.modal.open && a.zoomTo == SCR_FRONT) {
            toldTu4 = true;
            ask(a, "info", G_JUDGMENT, "Title Update 4 needed",
                "Gears of War: Judgment needs Title Update 4 for now - the disc version crashes at startup in this release.\n\n"
                "Add it with ADD UPDATE: the update package from your console's Content\\0000000000000000\\4D530A26\\000B0000 "
                "folder. We're working on getting the disc version to run too.", { "OK" });
        }
    }
    // 0.2.0 (Shagg 12:33): Gears 2 / 3 added without their Title Update 6 - once per session per game, until "Don't
    // show this again" (CogBox.ini [Notices] TuRecommendNever_<game>)
    {
        static bool told[G_COUNT] = {};
        for (int g : { (int)G_GEARS2, (int)G_GEARS3 }) {
            const GameState &st = a.st[g];
            if (told[g] || a.front != g || a.carPos != (float)g || a.modal.open || a.zoomTo != SCR_FRONT) continue;
            if (st.dataDir.empty() || !st.updateDir.empty()) continue;
            if (a.ini.geti("Notices", std::string("TuRecommendNever_") + game_def(g).key, 0)) { told[g] = true; continue; }
            told[g] = true;
            ask(a, "turec", g, "Title Update 6 recommended",
                std::string(game_def(g).name) + " runs without its title update, but Title Update 6 is highly recommended: "
                "it's the build COGBox's ports are made and tested for, with the best performance and compatibility.\n\n"
                "Add it with ADD UPDATE: the update package from your console's Content\\0000000000000000\\" +
                (g == G_GEARS2 ? "4D53082D" : "4D5308AB") + "\\000B0000 folder.", { "OK" });
            a.modal.check = "Don't show this again";
        }
    }
    // the hint line
    std::string padHint = std::string(tr("D-pad: choose a game")) + "      " + pad_name(a, "LB") + " / " + pad_name(a, "RB") + ": " + tr("Play or Settings") + "      " +
                          pad_name(a, "A") + ": " + tr("press") + "      " + pad_name(a, "X") + ": " + tr("settings") + "      " + pad_name(a, "Y") + ": " + tr("box art") + "      " +
                          pad_name(a, "LT") + " + " + pad_name(a, "RT") + ": " + tr("fullscreen") + "      " + pad_name(a, "Start") + ": " + tr("quit");   // job 523 / 525 / 526; 0.2.0 tr
    const char *hint = a.padConnected && a.lastPad ? padHint.c_str()   // 0.2.0: the device used last
                                      : tr("Arrows / wheel: turn      Enter: play      F11: fullscreen      Esc: quit      drop a disc image or folder on the window to add it");
    centred(F_SMALL, FS(F_SMALL), col::faint, hint, size.y - FS(F_SMALL) - S(10));
    {   // job 529 (Shagg 21:17): the maker's mark, bottom right, just above the hint line
        const char *mark = "Made by Shagg";
        ImVec2 ms = F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, mark);
        ImVec2 mp(size.x - ms.x - S(24), size.y - FS(F_SMALL) - ms.y - S(18));
        ImDrawList *wl = ImGui::GetWindowDrawList();
        wl->AddText(F_BOLD, FS(F_BOLD), ImVec2(mp.x + S(1), mp.y + S(1)), U(col::shadow, 0.8f), mark);
        wl->AddText(F_BOLD, FS(F_BOLD), mp, U(col::dim, 0.85f), mark);
    }
}
