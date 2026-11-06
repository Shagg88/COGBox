// COGBox - the front screen: the games as boxes on a turning carousel, with Play and Settings under the one in
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
    static const ImVec4 tints[G_COUNT] = { ImVec4(0.20f, 0.30f, 0.42f, 1), ImVec4(0.42f, 0.30f, 0.16f, 1), ImVec4(0.40f, 0.12f, 0.10f, 1),
                                           ImVec4(0.26f, 0.30f, 0.24f, 1) };
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
    // Gears 1 (no number): "GEARS OF" over a big "WAR"
    const char *top = g == G_GEARS1 ? "GEARS OF" : "GEARS OF WAR", *big = g == G_GEARS2 ? "2" : g == G_GEARS3 ? "3" : g == G_GEARS1 ? "WAR" : "JUDGMENT";
    float sTop = h * 0.075f, sBig = g == G_JUDGMENT ? h * 0.13f : g == G_GEARS1 ? h * 0.20f : h * 0.30f;
    ImVec2 st = F_TITLE->CalcTextSizeA(sTop, FLT_MAX, 0, top);
    float topY = o.y + h * (g == G_JUDGMENT ? 0.26f : g == G_GEARS1 ? 0.22f : 0.18f);
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
// The carousel works in slots of the library (the added games in release order, library_games); a.front stays a game
// index, a.carPos is a slot position in the library.
std::vector<int> library_games(const App &a)
{
    std::vector<int> v;
    for (int s = 0; s < G_COUNT; s++)
        if (game_in_library(a.st[game_at_slot(s)])) v.push_back(game_at_slot(s));
    return v;
}

int library_slot(const App &a, int g)
{
    const std::vector<int> v = library_games(a);
    for (size_t i = 0; i < v.size(); i++) if (v[i] == g) return (int)i;
    return -1;
}

static float wrap_d(float d, int count)   // -count/2 .. count/2
{
    const float n = (float)std::max(1, count), half = n * 0.5f;
    while (d > half) d -= n;
    while (d < -half) d += n;
    return d;
}

static void turn(App &a, int dir)
{
    const std::vector<int> lib = library_games(a);
    const int n = (int)lib.size();
    if (n == 0) return;
    const int s = std::max(0, library_slot(a, a.front));
    a.front = lib[((s + dir) % n + n) % n];
    a.ini.seti("Front", "Box", a.front);
}

// 0.4.6 (Shagg 5 Nov 18:07): a longer explanation of a drawn line shows when the mouse rests on it
static void hover_tip(ImVec2 p0, ImVec2 p1, const char *text)
{
    if (!ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(p0, p1) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(S(460), FLT_MAX));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(440));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// No game added yet: a short welcome in place of the carousel, with the way to add one
static void empty_draw(App &a, ImVec2 size, bool nav)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float cx = size.x * 0.5f, wrap = std::min(size.x - S(120), S(720));
    float y = size.y * 0.30f;
    th_cog(dl, ImVec2(cx, y), S(34), 10, (float)ImGui::GetTime() * 0.15f, U(col::redHi, 0.9f), U(ImVec4(0.03f, 0.03f, 0.035f, 1)));
    y += S(54);
    auto centred = [&](ImFont *f, float fs, ImVec4 c, const char *t) {
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, wrap, t);
        dl->AddText(f, fs, ImVec2(cx - ts.x * 0.5f, y), U(c), t, nullptr, wrap);
        y += ts.y;
    };
    centred(F_HEAD, FS(F_HEAD) * 1.25f, col::text, tr("Your library is empty"));
    y += S(8);
    // 0.4.6 (Shagg 4 Nov): short; drag and drop first (ADD A GAME below opens the same import, for controllers)
    centred(F_BODY, FS(F_BODY), col::amber, tr("Drag and drop your game, Title Update or DLC files here to install them."));
    y += S(4);
    {   // 0.4.6: one short line; the cloud-folder advice is its tooltip
        const float y0 = y;
        centred(F_BODY, FS(F_BODY), col::dim, tr("COGBox does not include game files. See the README for details."));
        hover_tip(ImVec2(cx - wrap * 0.5f, y0), ImVec2(cx + wrap * 0.5f, y),
                  tr("Install COGBox outside cloud-synced folders such as OneDrive, Dropbox or Google Drive - syncing can lock, move or partly update files while they are in use."));
    }
    y += S(22);
    const float bw = T(180), bh = T(36);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
    const bool busy = a.job.state.load() == 1;
    (void)bw; (void)bh; (void)busy; (void)nav;   // 0.4.6 (Shagg 5 Nov): drag and drop only
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
        // choose between Play, Settings and the Achievements side button under the box (a.frontSel), A presses the chosen one
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) turn(a, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) turn(a, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) a.frontSel = std::max(0, a.frontSel - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) a.frontSel = std::min(2, a.frontSel + 1);   // achievements part 4: 3
        if (ImGui::IsWindowHovered() && io.MouseWheel != 0) turn(a, io.MouseWheel > 0 ? -1 : 1);
    }
    // Shagg 1 Nov: only the added games are on the carousel. When that set changes (a copy added, a drive unplugged),
    // the carousel jumps to the box in front instead of turning through slots that moved.
    const std::vector<int> lib = library_games(a);
    const int nLib = (int)lib.size();
    {
        static unsigned shown = ~0u;
        unsigned mask = 0;
        for (int g : lib) mask |= 1u << g;
        if (nLib > 0 && library_slot(a, a.front) < 0) { a.front = lib[0]; shown = ~0u; }
        if (mask != shown) { shown = mask; a.carPos = (float)std::max(0, library_slot(a, a.front)); a.restSince = now; }
    }
    // ease the short way round
    float target = (float)std::max(0, library_slot(a, a.front)), d = wrap_d(target - a.carPos, nLib);
    float step = std::min(1.0f, io.DeltaTime * 9.0f);
    if (fabsf(d) < 0.002f) { if (a.carPos != target) a.restSince = now; a.carPos = target; }
    else a.carPos += d * step;
    a.carPos = fmodf(a.carPos + std::max(1, nLib), (float)std::max(1, nLib));

    // the corner mark
    {
        ImVec2 m(S(26), S(20));
        th_cog(dl, ImVec2(m.x + S(16), m.y + S(18)), S(16), 10, (float)now * 0.15f, U(col::redHi), U(ImVec4(0.03f, 0.03f, 0.035f, 1)));
        dl->AddText(F_TITLE, FS(F_TITLE), ImVec2(m.x + S(42), m.y), U(col::text), "COGBOX");
    }

    if (nLib == 0) empty_draw(a, size, nav);
    else {
        // the boxes, farthest first
        // the box gives up height to the bigger text under it (name, status line, buttons, hint line)
        float under = FS(F_HEAD) * 1.25f + FS(F_BODY) + T(36) + FS(F_SMALL) * 2 + S(70);   // Shagg 1 Nov: + the drop tip
        float H = std::min(std::min(size.y * 0.56f, size.y - under - S(100)), size.x * 0.33f / kCoverAspect);
        float cx = size.x * 0.5f, cy = S(90) + H * 0.5f, spacing = H * kCoverAspect * 1.25f;
        std::vector<int> order(lib.begin(), lib.end());
        auto dist = [&](int g) { return fabsf(wrap_d(library_slot(a, g) - a.carPos, nLib)); };
        std::sort(order.begin(), order.end(), [&](int x, int y) { return dist(x) > dist(y); });
        for (int g : order) {
            float dd = wrap_d(library_slot(a, g) - a.carPos, nLib), ad = std::min(1.0f, fabsf(dd));
            // 0.4.6 (Shagg 4 Nov): at most three whole covers - the front one and one each side; a fourth fades out
            // while the carousel turns and is not drawn at all once it would be past the side ones
            const float away = fabsf(dd);
            if (away >= 1.5f) continue;
            const float edgeFade = away > 1.0f ? std::max(0.0f, 1.0f - (away - 1.0f) * 2.0f) : 1.0f;
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
            float alpha = (1.0f - 0.45f * ad) * edgeFade;
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
        std::string line, lineTip;
        switch (st.ready) {
        case R_READY:
            line = st.useUpdate ? std::string(gd.updateName) : "Disc version";
            if (st.playSeconds > 0) line += "   |   " + human_duration(st.playSeconds) + " played";
            if (st.lastPlayed > 0) line += ", " + last_played_text(st.lastPlayed);
            // Shagg 29 Oct 18:07: an untested game version or title update (or an update made for another disc) still plays,
            // with a warning - unless the player said that exact file works
            // 0.4.6: two words on the line, the advice in its tooltip
            if ((st.xexKnown && !st.xexMatch && !st.xexConfirmed) || (st.useUpdate && st.xexpKnown && !st.xexpMatch && !st.xexpConfirmed)) {
                line += "   |   untested version";
                lineTip = "an untested version - it may not work as it should. Please don't report problems you get with it; use the expected one first";
            }
            break;
        case R_NO_COPY: line = gd.tagline; break;
        case R_COPY_MISSING: line = "Your copy isn't where it was - is its drive connected?"; break;
        case R_NEED_UPDATE: line = std::string("Needs ") + gd.updateName + " - add it from your console or its package file"; break;
        case R_NO_PORT: line = "Not in this release of COGBox yet"; break;
        }
        {
            const std::string shown = tr(line);   // 0.2.0
            const float lw = F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, shown.c_str()).x, y0 = y;
            y += centred(F_BODY, FS(F_BODY), st.ready == R_READY && lineTip.empty() ? col::dim : col::amber, shown, y) + S(14);
            if (!lineTip.empty()) hover_tip(ImVec2(cx - lw * 0.5f, y0), ImVec2(cx + lw * 0.5f, y), tr(lineTip.c_str()));
        }

        float bw = T(140), bh = T(36), gap = S(14);
        bool running = a.run.proc != nullptr, busy = a.job.state.load() == 1;
        // the controller's choice shows only while a controller is connected; A presses it
        bool sel0 = a.padConnected && a.frontSel == 0, sel1 = a.padConnected && a.frontSel == 1;
        bool sel2 = a.padConnected && a.frontSel == 2;   // achievements part 4
        bool padA = nav && a.padConnected && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
        ImGui::SetCursorScreenPos(ImVec2(cx - bw - gap * 0.5f, y));
        if (st.ready == R_READY) {
            // A presses the choice LB / RB made (a.frontSel); Enter plays
            if (th_button_red(running ? "RUNNING" : "PLAY", ImVec2(bw, bh), !running && !busy, F_HEAD, sel0) ||
                (nav && !running && !busy && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || (padA && a.frontSel == 0))))
                play_start(a, g);
        } else if (st.ready != R_NO_PORT) {
            // 0.4.6 (Shagg 5 Nov): drag and drop is the only way to add a copy, update or DLC
            th_button_red(st.ready == R_NEED_UPDATE ? "DROP THE UPDATE HERE" : "DROP YOUR COPY HERE", ImVec2(bw, bh), false, F_HEAD, sel0);
        } else {
            th_button_red("PLAY", ImVec2(bw, bh), false, F_HEAD, sel0);
        }
        ImGui::SameLine(0, gap);
        if (th_button("SETTINGS", ImVec2(bw, bh), st.ready != R_NO_PORT, F_HEAD, sel1) ||
            (nav && st.ready != R_NO_PORT && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (padA && a.frontSel == 1)))) {
            a.page = g; a.zoomTo = SCR_PAGE; a.zoomStart = now; a.tab = 0; a.barSel = -1;
        }
        // achievements part 4 (0.4.6 style): a small side button right of SETTINGS, not a third big one; it opens the
        // ACHIEVEMENTS tab on All. LB / RB reach it as the third choice.
        if (st.ready != R_NO_PORT) {
            ImGui::SetCursorScreenPos(ImVec2(cx + bw + gap * 1.5f, y + (bh - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, sel2 ? ImVec4(1, 1, 1, 0.10f) : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, sel2 ? col::text : col::dim);
            const bool hit = ImGui::Button(tr("Achievements"));
            ImGui::PopStyleColor(2);
            hover_tip(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), tr("Your achievements in all four games."));
            if (hit || (nav && padA && a.frontSel == 2)) {
                a.page = g; a.zoomTo = SCR_PAGE; a.zoomStart = now; a.tab = 4; a.barSel = -1;   // gs::Tab::Achievements
                achievements_tab_open_all();
            }
        }
        // Shagg 1 Nov: the drop tip, under the buttons (and the folder button when it shows)
        {
            // 0.4.6: the empty screen's short line; the full list of what can be dropped is its tooltip
            const std::string tip = tr("Drag and drop your game, Title Update or DLC files here to install them.");
            const float ty = y + bh + S(14), tw = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, tip.c_str()).x;
            const float th = centred(F_SMALL, FS(F_SMALL), col::dim, tip, ty);
            hover_tip(ImVec2(cx - tw * 0.5f, ty), ImVec2(cx + tw * 0.5f, ty + th),
                      tr("Tip: drag and drop a disc image, Games on Demand copy, title update, DLC package or game folder onto this window to add it."));
        }
    }   // the carousel (nLib > 0)

    // 0.4.6 (Shagg 4 Nov): one toolbar, top right, right to left: EXIT, box art, ADD GAME, MUSIC - the launcher's own
    // actions together (EXIT was alone at the bottom left); the same keys / pad buttons as before
    float artW = 0;   // the whole toolbar's width (the update notice sits left of it)
    {
        const float bh = T(28), gap = S(10), y = S(22);
        float right = size.x - S(24);
        auto place = [&](float w) { right -= w; ImGui::SetCursorScreenPos(ImVec2(right, y)); const float x = right; right -= gap; return x; };
        // EXIT (asks first, like Esc / Start; to-do 21)
        {
            const std::string lbl = tr("EXIT");
            ImVec2 es(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, lbl.c_str()).x + S(36), bh);
            place(es.x);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // the D-pad keeps turning the carousel
            const bool can = !a.run.proc && !a.modal.open && a.job.state.load() != 1;
            if (th_button((lbl + "##frontexit").c_str(), es, can, F_BOLD) && can)
                ask(a, "quit", -1, "Quit COGBox?", "Close COGBox?", { "Quit", "Cancel" });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", a.run.proc ? tr("Close the game first.") : tr("Close COGBox."));
            ImGui::PopItemFlag();
        }
        // Get box art (Y)
        {
            const char *lbl = "GET BOX ART";
            bool any = false;
            for (int i = 0; i < G_COUNT; i++) any |= a.cover[i].tex != nullptr;
            ImVec2 bs(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, tr(any ? "UPDATE ART" : lbl)).x + S(36), bh);
            place(bs.x);
            if (th_button(any ? "UPDATE ART" : lbl, bs, true, F_BOLD) || (nav && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))) {
                art_download_start();
                set_message(a, "Getting the box art and backgrounds from the x360db archive on GitHub...", false);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Downloads each game's Xbox Marketplace box art and background from the x360db archive on GitHub\n(the one Xenia Manager uses) into the covers folder. Your own covers\\gears2.png etc. still win.");
        }
        // Shagg 1 Nov: ADD GAME (Back / View) - the carousel has no boxes for games not added yet (same import as a drop)
        if (nLib > 0) {
            const bool busy = a.job.state.load() == 1;
            ImVec2 as(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, tr("ADD GAME")).x + S(36), bh);
            (void)as; (void)busy;   // 0.4.6: no ADD GAME - drop files on the window
            // Shagg 1 Nov: music on / off (music.cpp) - when a game in the library has a copy to read it from
            if (music_any_source(a)) {
                const char *ml = music_on(a) ? "MUSIC: ON" : "MUSIC: OFF";
                ImVec2 ms(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, tr(ml)).x + S(36), bh);
                place(ms.x);
                if (th_button(ml, ms, true, F_BOLD) || (nav && ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false)) ||
                    (!ImGui::GetIO().WantTextInput && !a.modal.open && ImGui::IsKeyPressed(ImGuiKey_M, false)))
                    music_set_on(a, !music_on(a));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s", tr("Plays the chosen game's own menu music, read from your copy, in the library and on its settings page. M switches it."));
            }
        }
        artW = size.x - S(24) - (right + gap);
    }
    // a newer COGBox on GitHub: a small notice beside the box art button; it opens this game's About tab, where Update is
    // (Shagg 21:57). COGBOX_SHOW_UPDATE=<version> shows it without a release (test switch, off for players).
    {
        std::string newer = update_newer();
        if (newer.empty() && getenv("COGBOX_SHOW_UPDATE")) newer = getenv("COGBOX_SHOW_UPDATE");
        if (!newer.empty() && a.st[a.front].ready != R_NO_PORT) {
            // 0.2.2 (Shagg): one click updates - downloads, unpacks and restarts COGBox by itself
            float prog = 0; std::string uerr;
            const int us_state = update_install_state(&prog, &uerr);
            static bool restarting = false;
            if (us_state == 3 && !restarting) { restarting = true; if (update_restart()) a.quit = true; }
            char pct[16];
            snprintf(pct, sizeof pct, "%d%%", int(prog * 100.0f + 0.5f));
            std::string lbl = us_state == 1 ? tr("DOWNLOADING") + std::string(" ") + pct
                            : us_state == 2 ? tr("INSTALLING") + std::string("...")
                            : us_state == 3 ? tr("RESTARTING") + std::string("...")
                                            : tr("UPDATE TO") + std::string(" ") + newer;
            ImVec2 us(F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, lbl.c_str()).x + S(36), T(28));
            ImGui::SetCursorScreenPos(ImVec2(size.x - artW - S(24) - S(12) - us.x, S(22)));
            const bool idle = us_state != 1 && us_state != 2 && us_state != 3;
            if (th_button_red(lbl.c_str(), us, idle, F_BOLD) && idle) {
                if (a.run.proc) set_message(a, "Close the game first, then update.", true);
                else { update_install_start(newer); set_message(a, "Updating COGBox - it restarts by itself when done.", false); }
            }
            if (us_state == -1 && !uerr.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", (tr("The update failed:") + std::string(" ") + uerr).c_str());
            else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", (std::string("COGBox ") + newer + " " + tr("is out. Click to download and install it; COGBox restarts by itself.")).c_str());
        }
    }
    // L-4: COGBox or a game's files inside a cloud-synced folder (OneDrive, Dropbox, Google Drive, iCloud) - a short
    // amber line, bottom left, with the advice as its tooltip. Checked every few seconds (a copy can be added any time).
    {
        static double checkedAt = -100;
        static bool cloud = false;
        if (now - checkedAt > 5) {
            checkedAt = now;
            auto lower = [](std::string x) { for (char &c : x) c = (char)tolower((unsigned char)c); return x; };
            std::vector<std::string> roots;   // the cloud folders this PC names, then the usual folder names
            for (const char *v : { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" })
                if (const char *e = getenv(v)) if (*e) roots.push_back(lower(e));
            auto in_cloud = [&](const std::string &path) {
                if (path.empty()) return false;
                const std::string l = lower(path) + "\\";
                for (const std::string &r : roots) if (l.rfind(r + "\\", 0) == 0) return true;
                for (const char *m : { "\\onedrive\\", "\\onedrive - ", "\\dropbox\\", "\\google drive\\", "\\my drive\\",
                                       "\\icloud drive\\", "\\iclouddrive\\" })
                    if (l.find(m) != std::string::npos) return true;
                return false;
            };
            cloud = in_cloud(exe_dir());
            for (int g = 0; g < G_COUNT && !cloud; g++) cloud = in_cloud(a.st[g].dataDir) || in_cloud(a.st[g].updateDir);
        }
        if (cloud) {
            const std::string t = tr("In a cloud folder");
            const ImVec2 ts = F_BODY->CalcTextSizeA(FS(F_BODY), FLT_MAX, 0, t.c_str());
            const ImVec2 p0(S(24), size.y - S(64) - ts.y);
            ImGui::GetWindowDrawList()->AddText(F_BODY, FS(F_BODY), p0, U(col::amber), t.c_str());
            hover_tip(p0, ImVec2(p0.x + ts.x, p0.y + ts.y),
                      tr("Install COGBox outside cloud-synced folders such as OneDrive, Dropbox or Google Drive - syncing can lock, move or partly update files while they are in use."));
        }
    }
    // Title updates required (0.4.0): once per session per game, when the carousel stops on a game that has no title
    // update yet (replaces 0.2.0's Judgment / Gears 1 "needed" and Gears 2 / 3 "recommended" notices)
    {
        bool *told = a.tuTold;   // 0.4.6: shared with the import notice (install.cpp), so it is said once
        for (int g = 0; g < G_COUNT; g++) {
            const GameState &st = a.st[g];
            if (told[g] || st.ready != R_NEED_UPDATE || a.front != g || a.carPos != (float)library_slot(a, g) || a.modal.open || a.zoomTo != SCR_FRONT) continue;
            told[g] = true;
            char tid[16];
            snprintf(tid, sizeof tid, "%08X", (unsigned)game_def(g).titleId);
            (void)tid;
            ask(a, "info", g, "Title update needed",   // 0.4.6 (Shagg 4 Nov): short - the steps are in the README
                std::string(game_def(g).updateName) + " " + tr("is required for the smoothest experience. See the README for details."), { "OK" });
        }
    }
    // the hint line - 0.4.6 (Shagg 4 Nov): a short legend with button / key pictures for the device in use (Xbox
    // colours, PlayStation shapes, key caps), only the main actions (no "press", no install text)
    {
        struct Item { int kind; const char *key; std::string label; };   // kind 0 = pad button, 1 = keyboard key
        std::vector<Item> it;
        const bool pad = a.padConnected && a.lastPad;
        if (pad) {
            it.push_back({ 0, "D-pad", tr("choose") });
            it.push_back({ 0, "A", tr("play") });
            it.push_back({ 0, "X", tr("settings") });
            it.push_back({ 0, "Y", tr("box art") });
            if (music_any_source(a)) it.push_back({ 0, "RS", tr("music") });
            it.push_back({ 0, "LT+RT", tr("fullscreen") });   // drawn with the pad's own names (L2+R2 on PlayStation)
            it.push_back({ 0, "Start", tr("quit") });
        } else {
            it.push_back({ 1, "< >", tr("choose") });
            it.push_back({ 1, "Enter", tr("play") });
            it.push_back({ 1, "F11", tr("fullscreen") });
            it.push_back({ 1, "Esc", tr("quit") });
        }
        float fs = FS(F_SMALL);
        const float gap = S(22), pad_x = S(6);
        auto key_text = [&](const Item &x) -> std::string {
            if (x.kind != 0) return x.key;
            if (!strcmp(x.key, "LT+RT")) return std::string(pad_name(a, "LT")) + "+" + pad_name(a, "RT");
            return pad_name(a, x.key);
        };
        float ih = fs * 1.35f;
        auto icon_w = [&](const Item &x) {
            const std::string n = key_text(x);
            const bool face = x.kind == 0 && (!strcmp(x.key, "A") || !strcmp(x.key, "B") || !strcmp(x.key, "X") || !strcmp(x.key, "Y"));
            return face ? ih : F_BOLD->CalcTextSizeA(fs, FLT_MAX, 0, n.c_str()).x + pad_x * 2;
        };
        auto measure = [&] {
            float t = 0;
            for (const Item &x : it) t += icon_w(x) + S(6) + F_SMALL->CalcTextSizeA(fs, FLT_MAX, 0, x.label.c_str()).x + gap;
            return t - gap;
        };
        float total = measure();
        // review 0.4.6: a narrow window / long language - a smaller legend that fits instead of running off the edges
        if (total > size.x - S(32)) {
            fs *= std::max(0.6f, (size.x - S(32)) / total);
            ih = fs * 1.35f;
            total = measure();
        }
        float x0 = std::max(S(16), (size.x - total) * 0.5f);
        const float y0 = size.y - ih - S(10);
        for (const Item &x : it) {
            const float w = icon_w(x);
            const ImVec2 c(x0 + w * 0.5f, y0 + ih * 0.5f);
            const bool face = x.kind == 0 && (!strcmp(x.key, "A") || !strcmp(x.key, "B") || !strcmp(x.key, "X") || !strcmp(x.key, "Y"));
            if (face) {
                const float r = ih * 0.5f;
                if (!a.padPs) {   // Xbox: the coloured face button with its letter
                    const ImU32 fill = !strcmp(x.key, "A") ? IM_COL32(80, 170, 60, 255) : !strcmp(x.key, "B") ? IM_COL32(200, 50, 50, 255)
                                     : !strcmp(x.key, "X") ? IM_COL32(40, 110, 200, 255) : IM_COL32(220, 180, 40, 255);
                    dl->AddCircleFilled(c, r, fill, 24);
                    const ImVec2 ts = F_BOLD->CalcTextSizeA(fs, FLT_MAX, 0, x.key);
                    dl->AddText(F_BOLD, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), x.key);
                } else {          // PlayStation: the shape on a dark button
                    dl->AddCircleFilled(c, r, IM_COL32(40, 40, 46, 255), 24);
                    const float q = r * 0.48f;
                    if (!strcmp(x.key, "A")) {   // cross
                        const ImU32 k = IM_COL32(120, 160, 230, 255);
                        dl->AddLine(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), k, S(2));
                        dl->AddLine(ImVec2(c.x - q, c.y + q), ImVec2(c.x + q, c.y - q), k, S(2));
                    } else if (!strcmp(x.key, "B")) dl->AddCircle(c, q, IM_COL32(230, 90, 100, 255), 20, S(2));
                    else if (!strcmp(x.key, "X")) dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), IM_COL32(220, 120, 200, 255), 0, 0, S(2));
                    else dl->AddTriangle(ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y + q * 0.8f), ImVec2(c.x - q, c.y + q * 0.8f), IM_COL32(60, 200, 170, 255), S(2));
                }
            } else {   // a key cap / shoulder, trigger, stick or menu button: its name in a rounded box
                const std::string ns = key_text(x);
                const char *n = ns.c_str();
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + ih), IM_COL32(48, 48, 54, 255), S(4));
                dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + ih), IM_COL32(110, 110, 118, 255), S(4), 0, S(1));
                const ImVec2 ts = F_BOLD->CalcTextSizeA(fs, FLT_MAX, 0, n);
                dl->AddText(F_BOLD, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(235, 235, 235, 255), n);
            }
            x0 += w + S(6);
            const ImVec2 ls = F_SMALL->CalcTextSizeA(fs, FLT_MAX, 0, x.label.c_str());
            dl->AddText(F_SMALL, fs, ImVec2(x0, y0 + (ih - ls.y) * 0.5f), U(col::dim), x.label.c_str());
            x0 += ls.x + gap;
        }
    }
    {   // job 529 (Shagg 21:17): the maker's mark, bottom right, just above the hint line
        const char *mark = "Made by Shagg";
        ImVec2 ms = F_BOLD->CalcTextSizeA(FS(F_BOLD), FLT_MAX, 0, mark);
        ImVec2 mp(size.x - ms.x - S(24), size.y - FS(F_SMALL) * 1.35f - ms.y - S(20));   // review 0.4.6: above the legend's button pictures
        ImDrawList *wl = ImGui::GetWindowDrawList();
        wl->AddText(F_BOLD, FS(F_BOLD), ImVec2(mp.x + S(1), mp.y + S(1)), U(col::shadow, 0.8f), mark);
        wl->AddText(F_BOLD, FS(F_BOLD), mp, U(col::dim, 0.85f), mark);
        // 0.2.2 (Shagg): the version, just above the mark
        const char *ver = "v" COGBOX_VERSION;
        ImVec2 vs = F_SMALL->CalcTextSizeA(FS(F_SMALL), FLT_MAX, 0, ver);
        ImVec2 vp(size.x - vs.x - S(24), mp.y - vs.y - S(2));
        wl->AddText(F_SMALL, FS(F_SMALL), ImVec2(vp.x + S(1), vp.y + S(1)), U(col::shadow, 0.8f), ver);
        wl->AddText(F_SMALL, FS(F_SMALL), vp, U(col::faint, 0.9f), ver);
    }
}
