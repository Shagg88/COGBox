// gn_hlsl_check <corpus_dir> <out_dir> [--dxc <path>] [--only <name>] [--shard i/n]: Stage 3 step S3.3 check
// (notes/36, 39). For every <vs|ps>-<hash>.ucode in the corpus folder (written by --gow2_shader_dump_dir): decode with
// our xenos::ucode decoder, translate with xenos::hlsl, write <out_dir>/<name>.hlsl, and compile it twice with DXC:
//   DXIL   : dxc -T vs_6_6|ps_6_6 -E main -HV 2021 -Gis -denorm ftz
//   SPIR-V : dxc -T vs_6_6|ps_6_6 -E main -HV 2021 -spirv -fspv-target-env=vulkan1.3
// Prints "<name> DXIL PASS|FAIL SPIRV PASS|FAIL" with the first compiler error, a summary per target, and writes
// <out_dir>/simplifications.csv (hash,feature,note - hashes only, no shader content) listing every shader that uses a
// placeholder, stub or simplified path, so S3.4's interpreter comparisons know which mismatches are expected.
// Exit code 0 only if every shader compiles for both targets. The corpus and the generated HLSL are game-derived and
// stay out of the repository (keep <out_dir> under out\).
#include <xenos/hlsl.h>
#include <xenos/ucode.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Runs a command with stderr+stdout to a log file; returns the exit code and the first error-looking line. Every
// argument is quoted (PowerShell split "-fspv-target-env=vulkan1.3" at the dot when it was passed bare, job 079), and on
// Windows the whole line is wrapped in one more pair of quotes because cmd.exe /c strips the outermost pair.
static int Run(const std::string& cmd, const fs::path& log, std::string& first_error) {
  std::string full = cmd + " > " + Quote(log.string()) + " 2>&1";
#ifdef _WIN32
  full = "\"" + full + "\"";
#endif
  const int rc = std::system(full.c_str());
  first_error.clear();
  if (rc != 0) {
    std::ifstream f(log);
    std::string line;
    while (std::getline(f, line)) {
      if (line.find("error") != std::string::npos) {
        first_error = line;
        break;
      }
    }
    if (first_error.empty()) first_error = "(no error line; see " + log.string() + ")";
  }
  return rc;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: gn_hlsl_check <corpus_dir> <out_dir> [--dxc <path>] [--only <name>]\n");
    return 2;
  }
  const fs::path corpus = argv[1], out_dir = argv[2];
  std::string dxc = "dxc", only;
  size_t shard = 0, shards = 1;  // --shard i/n: every n-th shader starting at i (to use several cores)
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dxc" && i + 1 < argc) dxc = argv[++i];
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--shard" && i + 1 < argc) std::sscanf(argv[++i], "%zu/%zu", &shard, &shards);
  }
  fs::create_directories(out_dir);
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(corpus))
    if (e.path().extension() == ".ucode") files.push_back(e.path());
  std::sort(files.begin(), files.end());

  size_t total = 0, dxil_ok = 0, spirv_ok = 0, pc_loop = 0;
  std::map<std::string, size_t> feature_counts;
  std::ofstream csv(out_dir / (shards > 1 ? std::format("simplifications.{}.csv", shard) : std::string("simplifications.csv")),
                    std::ios::binary);
  csv << "hash,feature,note\n";
  size_t file_index = 0;
  for (const fs::path& path : files) {
    const std::string name = path.stem().string();
    if (!only.empty() && name != only) continue;
    if (shards > 1 && (file_index++ % shards) != shard) continue;
    const bool vertex = name.rfind("vs-", 0) == 0;
    const std::string raw = ReadFile(path);
    std::vector<uint32_t> dwords(raw.size() / 4);
    std::copy(raw.data(), raw.data() + dwords.size() * 4, reinterpret_cast<char*>(dwords.data()));
    const xenos::ucode::Program program = xenos::ucode::Decode(dwords.data(), dwords.size(), vertex);
    const xenos::hlsl::Translation t = xenos::hlsl::TranslateToHlsl(program);
    ++total;
    if (t.uses_pc_loop) ++pc_loop;
    const fs::path hlsl_path = out_dir / (name + ".hlsl");
    std::ofstream(hlsl_path, std::ios::binary) << t.hlsl;
    for (const auto& s : t.simplifications) {
      csv << name << "," << s.feature << ",\"" << s.note << "\"\n";
      ++feature_counts[s.feature];
    }
    const std::string profile = vertex ? "vs_6_6" : "ps_6_6";
    const std::string common = Quote(dxc) + " " + Quote("-T") + " " + Quote(profile) + " " + Quote("-E") + " " +
                               Quote("main") + " " + Quote("-HV") + " " + Quote("2021") + " " + Quote(hlsl_path.string());
    const std::string cmd_dxil = common + " " + Quote("-Gis") + " " + Quote("-denorm") + " " + Quote("ftz") + " " +
                                 Quote("-Fo") + " " + Quote((out_dir / (name + ".dxil")).string());
    const std::string cmd_spirv = common + " " + Quote("-spirv") + " " + Quote("-fspv-target-env=vulkan1.3") + " " +
                                  Quote("-Fo") + " " + Quote((out_dir / (name + ".spv")).string());
    if (total == 1) {
      // The exact command lines, once, so the log shows what ran.
      std::printf("CMD DXIL : %s\nCMD SPIRV: %s\n", cmd_dxil.c_str(), cmd_spirv.c_str());
    }
    std::string err_dxil, err_spirv;
    const int rc_dxil = Run(cmd_dxil, out_dir / (name + ".dxil.log"), err_dxil);
    const int rc_spirv = Run(cmd_spirv, out_dir / (name + ".spv.log"), err_spirv);
    if (rc_dxil == 0) ++dxil_ok;
    if (rc_spirv == 0) ++spirv_ok;
    std::printf("%s DXIL %s SPIRV %s%s\n", name.c_str(), rc_dxil == 0 ? "PASS" : "FAIL", rc_spirv == 0 ? "PASS" : "FAIL",
                t.simplifications.empty() ? "" : " (simplified)");
    if (rc_dxil != 0) std::printf("  dxil : %s\n", err_dxil.c_str());
    if (rc_spirv != 0) std::printf("  spirv: %s\n", err_spirv.c_str());
  }
  for (const auto& [feature, count] : feature_counts) std::printf("SIMPLIFIED %s: %zu shader(s)\n", feature.c_str(), count);
  std::printf("PC-LOOP control flow: %zu shader(s)\n", pc_loop);
  std::printf("DXIL: %zu/%zu compiled\n", dxil_ok, total);
  std::printf("SPIR-V: %zu/%zu compiled\n", spirv_ok, total);
  return (total && dxil_ok == total && spirv_ok == total) ? 0 : 1;
}
