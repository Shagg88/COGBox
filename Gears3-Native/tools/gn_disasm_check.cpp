// gn_disasm_check <corpus_dir>: Stage 3 step S3.2 check (notes/36). For every <vs|ps>-<hash>.ucode in the folder written
// by --gow2_shader_dump_dir, decode it with our xenos::ucode decoder, disassemble, and compare with the SDK's own
// disassembly in the matching .sdk.txt. Prints PASS/FAIL per shader (with the first differing line) and "N/M identical";
// exit code 0 only if every shader matches. The corpus is game-derived and stays out of the repository.
// Also lists every decoder issue (unknown opcodes/fields, and cases where Xenia Canary decodes differently) per shader,
// and which shaders' text would change under Canary's rules ("CANARY-DIFF").
#include <xenos/ucode.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gn_disasm_check <corpus_dir> [--quiet]\n");
    return 2;
  }
  const bool quiet = argc > 2 && std::string(argv[2]) == "--quiet";
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(argv[1]))
    if (e.path().extension() == ".ucode") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  size_t identical = 0, missing = 0, canary_diff = 0;
  std::map<std::string, std::vector<std::string>> issue_shaders;  // issue kind -> shaders
  for (const fs::path& path : files) {
    const std::string name = path.stem().string();
    const bool vertex = name.rfind("vs-", 0) == 0;
    const std::string raw = ReadFile(path);
    std::vector<uint32_t> dwords(raw.size() / 4);
    std::copy(raw.data(), raw.data() + dwords.size() * 4, reinterpret_cast<char*>(dwords.data()));
    fs::path ref_path = path;
    ref_path.replace_extension(".sdk.txt");
    if (!fs::exists(ref_path)) {
      ++missing;
      std::printf("SKIP %s (no .sdk.txt)\n", name.c_str());
      continue;
    }
    const std::string expected = ReadFile(ref_path);
    const xenos::ucode::Program program = xenos::ucode::Decode(dwords.data(), dwords.size(), vertex);
    const std::string ours = xenos::ucode::Disassemble(program);
    std::map<std::string, int> kinds;
    for (const auto& issue : program.issues) {
      if (!kinds[xenos::ucode::IssueKindName(issue.kind)]++) issue_shaders[xenos::ucode::IssueKindName(issue.kind)].push_back(name);
      std::printf("ISSUE %s @%u: %s (%s)\n", name.c_str(), issue.address, xenos::ucode::IssueKindName(issue.kind),
                  issue.detail.c_str());
    }
    xenos::ucode::DecodeOptions canary;
    canary.canary_tfetch1d_two_components = canary.canary_scalar_operand_z = true;
    if (xenos::ucode::Disassemble(xenos::ucode::Decode(dwords.data(), dwords.size(), vertex, canary)) != ours) {
      ++canary_diff;
      std::printf("CANARY-DIFF %s\n", name.c_str());
    }
    if (ours == expected) {
      ++identical;
      if (!quiet) std::printf("PASS %s\n", name.c_str());
      continue;
    }
    // First differing line, for diagnosis.
    std::istringstream a(expected), b(ours);
    std::string la, lb;
    int line = 0;
    while (true) {
      const bool ga = bool(std::getline(a, la)), gb = bool(std::getline(b, lb));
      ++line;
      if (!ga && !gb) break;
      if (!ga || !gb || la != lb) {
        std::printf("FAIL %s line %d\n  sdk : %s\n  ours: %s\n", name.c_str(), line, ga ? la.c_str() : "<end>",
                    gb ? lb.c_str() : "<end>");
        break;
      }
    }
    if (a.eof() && b.eof() && la == lb) std::printf("FAIL %s (differs only in trailing bytes)\n", name.c_str());
  }
  const size_t compared = files.size() - missing;
  for (const auto& [kind, shaders] : issue_shaders) {
    std::printf("ISSUE-SUMMARY %s: %zu shader(s), e.g. %s\n", kind.c_str(), shaders.size(), shaders.front().c_str());
  }
  std::printf("CANARY-DIFF-SUMMARY %zu shader(s) would disassemble differently under Canary's rules\n", canary_diff);
  std::printf("%zu/%zu identical\n", identical, compared);
  return (compared && identical == compared) ? 0 : 1;
}
