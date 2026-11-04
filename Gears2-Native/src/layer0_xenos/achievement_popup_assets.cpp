// Layer 0 (game-free): the achievement popups' fonts and sound - see xenos/achievement_popup_assets.h. Our code (MIT).
#include <xenos/achievement_popup_assets.h>
#include <xenos/achievements_data.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <imgui.h>

#include <atomic>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REXCVAR_DECLARE(std::string, cogbox_dir);  // achievements_export.cpp

namespace {

ImFont* g_regular = nullptr;
ImFont* g_semibold = nullptr;

std::filesystem::path FontsDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf) / "Fonts";
#endif
  return "C:\\Windows\\Fonts";
}

bool Exists(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}

// Decodes UTF-8 into code points (bad bytes skipped).
void AddCodepoints(const std::string& s, std::vector<uint32_t>* out) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
    else { ++i; continue; }
    if (i + len > s.size()) break;
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out->push_back(cp);
    i += len;
  }
}

// ImGui glyph ranges (pairs, 0-terminated) for the given code points.
std::vector<ImWchar> RangesFor(const std::vector<uint32_t>& cps) {
  ImFontGlyphRangesBuilder b;
  for (uint32_t cp : cps) {
    if (cp <= 0xFFFF) b.AddChar(static_cast<ImWchar>(cp));
  }
  ImVector<ImWchar> r;
  b.BuildRanges(&r);
  return std::vector<ImWchar>(r.begin(), r.end());
}

}  // namespace

void XenosConfigureAchievementFonts(ImFontAtlas* atlas) {
  if (!atlas) return;
  const std::filesystem::path dir = FontsDir();
  struct Pair { const char* regular; const char* semibold; };
  static const Pair chain[] = {{"segoeui.ttf", "seguisb.ttf"}, {"tahoma.ttf", "tahomabd.ttf"}, {"arial.ttf", "arialbd.ttf"}};
  // Latin, Latin-1, Latin Extended A/B, Greek, Cyrillic, general punctuation (the ellipsis, dashes), currency signs.
  static const ImWchar kWestern[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0};

  // Korean / Chinese characters: those the cached achievement lists use (part 1's catalogs, any game).
  std::vector<uint32_t> hangul, han;
  const std::filesystem::path cogbox = XenosCogboxDir();
  for (const auto& g : xenos::ach::kGames) {
    const auto cat = xenos::ach::LoadCatalog(cogbox, g.title_id);
    if (!cat) continue;
    std::vector<uint32_t> cps;
    for (const auto& e : cat->achievements) {
      AddCodepoints(e.label, &cps);
      AddCodepoints(e.description, &cps);
      AddCodepoints(e.unachieved, &cps);
    }
    AddCodepoints(cat->game, &cps);
    for (uint32_t cp : cps) {
      if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) hangul.push_back(cp);
      else if (cp >= 0x2E80 && cp <= 0x9FFF) han.push_back(cp);
      else if (cp >= 0xFF00 && cp <= 0xFFEF) han.push_back(cp);  // full-width forms
    }
  }
  static std::vector<ImWchar> hangul_ranges, han_ranges;  // the atlas keeps the pointers until it is built
  hangul_ranges = RangesFor(hangul);
  han_ranges = RangesFor(han);

  auto add = [&](const char* file, bool merge_cjk) -> ImFont* {
    const std::filesystem::path p = dir / file;
    if (!Exists(p)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFont* f = atlas->AddFontFromFileTTF(rex::path_to_utf8(p).c_str(), kAchievementFontPixels, &cfg, kWestern);
    if (!f || !merge_cjk) return f;
    ImFontConfig m;
    m.MergeMode = true;
    m.OversampleH = 1;
    if (hangul_ranges.size() > 1 && Exists(dir / "malgun.ttf"))
      atlas->AddFontFromFileTTF(rex::path_to_utf8(dir / "malgun.ttf").c_str(), kAchievementFontPixels, &m, hangul_ranges.data());
    if (han_ranges.size() > 1) {
      for (const char* cjk : {"msyh.ttc", "msjh.ttc", "simsun.ttc"}) {
        if (!Exists(dir / cjk)) continue;
        atlas->AddFontFromFileTTF(rex::path_to_utf8(dir / cjk).c_str(), kAchievementFontPixels, &m, han_ranges.data());
        break;
      }
    }
    return f;
  };
  for (const Pair& p : chain) {
    if (!Exists(dir / p.regular)) continue;
    g_regular = add(p.regular, true);
    g_semibold = add(p.semibold, true);
    if (!g_semibold) g_semibold = g_regular;
    REXLOG_INFO("achievement popups: font {} ({} Korean, {} Chinese characters from the cached lists)", p.regular,
                hangul.size(), han.size());
    return;
  }
  REXLOG_WARN("achievement popups: no Segoe UI, Tahoma or Arial in {}; ImGui's own font is used",
              rex::path_to_utf8(dir));
}

ImFont* XenosAchievementFont(bool semibold) { return semibold ? g_semibold : g_regular; }

std::filesystem::path XenosCogboxDir() {
  const std::string dir = REXCVAR_GET(cogbox_dir);
  if (!dir.empty()) return rex::to_path(dir);
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path().parent_path().parent_path();
#endif
  return {};
}

void XenosPlayAchievementSound(const char* style_name) {
#ifdef _WIN32
  const std::filesystem::path sounds = XenosCogboxDir() / "assets" / "sounds";
  std::filesystem::path file = sounds / (std::string("achievement-") + (style_name ? style_name : "") + ".wav");
  if (!Exists(file)) file = sounds / "achievement.wav";
  if (!Exists(file)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_INFO("achievement popups: no sound file in {}", rex::path_to_utf8(sounds));
    return;
  }
  using PlaySoundWFn = BOOL(WINAPI*)(LPCWSTR, HMODULE, DWORD);
  static PlaySoundWFn play = [] {
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    return winmm ? reinterpret_cast<PlaySoundWFn>(GetProcAddress(winmm, "PlaySoundW")) : nullptr;
  }();
  constexpr DWORD kAsync = 0x0001, kNoDefault = 0x0002, kFilename = 0x00020000;  // mmsystem.h SND_*
  if (!play || !play(file.c_str(), nullptr, kFilename | kAsync | kNoDefault)) {
    REXLOG_INFO("achievement popups: could not play {}", rex::path_to_utf8(file));
  }
#else
  (void)style_name;
#endif
}
