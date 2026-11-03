// Shared progress protocol for the standalone compiler and COGBox. No game data.
#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace xenos::preparation {
inline std::filesystem::path ProgressPath(const std::filesystem::path& cache, const std::string& session) {
  return cache / (session.empty() ? "prepare-progress.txt" : "prepare-progress." + session + ".txt");
}
inline std::filesystem::path CancelPath(const std::filesystem::path& cache, const std::string& session) {
  return cache / (session.empty() ? "prepare-cancel.flag" : "prepare-cancel." + session + ".flag");
}
struct Progress {
  std::string state = "starting";
  uint64_t seconds = 0, files = 0, files_total = 0, files_failed = 0;
  uint64_t variants = 0, variants_total = 0, variants_failed = 0;
  uint64_t pipelines = 0, pipelines_total = 0, pipelines_missing = 0, pipelines_failed = 0;

  bool Finished() const {
    return state == "done" || state == "partial" || state == "cancelled" || state == "failed";
  }
  std::string Serialize() const {
    std::ostringstream s;
    s << "COGBOX_PREPARE 2 " << state << ' ' << seconds << ' ' << files << ' ' << files_total << ' ' << files_failed
      << ' ' << variants << ' ' << variants_total << ' ' << variants_failed << ' ' << pipelines << ' '
      << pipelines_total << ' ' << pipelines_missing << ' ' << pipelines_failed << '\n';
    return s.str();
  }
  static bool Parse(const std::string& text, Progress& result) {
    std::istringstream s(text);
    std::string magic, version, state, token;
    if (!(s >> magic >> version >> state) || magic != "COGBOX_PREPARE" || version != "2") return false;
    if (state != "starting" && state != "reading" && state != "building" && state != "saving" &&
        state != "done" && state != "partial" && state != "cancelled" && state != "failed") return false;
    Progress p;
    p.state = state;
    for (uint64_t* value : {&p.seconds, &p.files, &p.files_total, &p.files_failed, &p.variants, &p.variants_total,
                            &p.variants_failed, &p.pipelines, &p.pipelines_total, &p.pipelines_missing, &p.pipelines_failed}) {
      if (!(s >> token)) return false;
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), *value);
      if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return false;
    }
    if (s >> token) return false;
    if (p.files > p.files_total || p.files_failed > p.files || p.variants > p.variants_total ||
        p.variants_failed > p.variants || p.pipelines > p.pipelines_total ||
        p.pipelines_missing > p.pipelines_total || p.pipelines_failed > p.pipelines_total) return false;
    if (p.state == "done" && (p.files != p.files_total || p.variants != p.variants_total ||
        p.pipelines != p.pipelines_total || p.files_failed || p.variants_failed || p.pipelines_missing || p.pipelines_failed)) return false;
    result = p;
    return true;
  }
};

inline bool WriteProgress(const std::filesystem::path& path, const Progress& p) {
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f || !(f << p.Serialize()) || !f.flush()) return false;
  }
#ifdef _WIN32
  // A reader may briefly hold the previous file open without delete sharing.
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    Sleep(2);
  }
  return false;
#else
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  return !ec;
#endif
}
}  // namespace xenos::preparation
