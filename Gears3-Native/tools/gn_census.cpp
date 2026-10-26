// gn_census <corpus_dir>: feature census of a shader corpus (<vs|ps>-<hash>.ucode files, host-order dwords) using our
// xenos::ucode decoder (Gears 2 layer 0). Game-free tool; the corpus is game-derived and stays out of the repository.
// Counts, per feature, how many shaders use it - the list Gears 2 checked before its translator work (Briefs 9-10):
// decoder issues, memexport, aL / a0 relative addressing, oPts writes, loops, calls/jumps, predication, kills, cube,
// texture offsets, LOD/filter overrides, max4, transcendental ops, 16-bit float vertex data, the two Canary-vs-rexglue
// edge cases. Prints "feature: N shaders (examples)" lines.
#include <xenos/ucode.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xenos::ucode;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: gn_census <corpus_dir>\n"); return 2; }
  std::map<std::string, std::vector<std::string>> feat;
  size_t vs = 0, ps = 0, total_ins = 0;
  auto add = [&](std::set<std::string>& s, const std::string& f) { s.insert(f); };
  for (const auto& e : fs::directory_iterator(argv[1])) {
    if (e.path().extension() != ".ucode") continue;
    const std::string name = e.path().stem().string();
    const bool vertex = name.rfind("vs-", 0) == 0;
    (vertex ? vs : ps)++;
    std::ifstream f(e.path(), std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint32_t> dw(b.size() / 4);
    std::memcpy(dw.data(), b.data(), dw.size() * 4);
    Program p = Decode(dw.data(), dw.size(), vertex);
    std::set<std::string> s;
    for (const Issue& i : p.issues) add(s, std::string("issue:") + IssueKindName(i.kind));
    for (const ControlFlow& cf : p.control_flow) {
      switch (cf.opcode) {
        case CfOpcode::kLoopStart: add(s, "cf:loop"); break;
        case CfOpcode::kCondCall: case CfOpcode::kReturn: add(s, "cf:call/return"); break;
        case CfOpcode::kCondJmp: add(s, "cf:jump"); break;
        case CfOpcode::kAlloc: if (cf.alloc_type == AllocType::kMemory) add(s, "memexport (alloc memory)"); break;
        default: break;
      }
      if (cf.exec_type == ControlFlow::ExecType::kPredicated) add(s, "cf:predicated exec");
      if (cf.exec_type == ControlFlow::ExecType::kConditional) add(s, "cf:bool-conditional exec");
      for (const Instruction& in : cf.instructions) {
        total_ins++;
        if (in.kind == Instruction::Kind::kAlu) {
          const Alu& a = in.alu;
          if (a.predicated) add(s, "alu:predicated");
          for (const Result* r : {&a.vector_result, &a.scalar_result}) {
            if (r->target == Target::kPointSizeEdgeFlagKillVertex && r->write_mask) add(s, "export:oPts");
            if (r->target == Target::kExportAddress || r->target == Target::kExportData) add(s, "memexport (eA/eM write)");
            if (r->target == Target::kDepth && r->write_mask) add(s, "export:oDepth");
            if (r->target == Target::kColor && r->index > 0 && r->write_mask) add(s, "export:MRT (oC1+)");
            if (r->addressing == Addressing::kLoopRelative) add(s, "addr:aL-relative dest");
          }
          auto ops = [&](const Operand* o, uint32_t n) {
            for (uint32_t k = 0; k < n; ++k) {
              if (o[k].addressing == Addressing::kAddressRegisterRelative) add(s, "addr:a0-relative");
              if (o[k].addressing == Addressing::kLoopRelative) add(s, "addr:aL-relative");
            }
          };
          if (!a.IsVectorOpDefaultNop()) {
            ops(a.vector_operands, a.vector_operand_count);
            switch (a.vector_opcode) {
              case AluVectorOpcode::kMax4: add(s, "op:max4"); break;
              case AluVectorOpcode::kCube: add(s, "op:cube"); break;
              case AluVectorOpcode::kKillEq: case AluVectorOpcode::kKillGt: case AluVectorOpcode::kKillGe:
              case AluVectorOpcode::kKillNe: add(s, "op:kill"); break;
              case AluVectorOpcode::kMaxA: add(s, "op:maxa (sets a0)"); break;
              case AluVectorOpcode::kDst: add(s, "op:dst"); break;
              default: break;
            }
          }
          if (!a.IsScalarOpDefaultNop()) {
            ops(a.scalar_operands, a.scalar_operand_count);
            switch (a.scalar_opcode) {
              case AluScalarOpcode::kExp: case AluScalarOpcode::kLog: case AluScalarOpcode::kLogc:
              case AluScalarOpcode::kRcp: case AluScalarOpcode::kRcpc: case AluScalarOpcode::kRcpf:
              case AluScalarOpcode::kRsq: case AluScalarOpcode::kRsqc: case AluScalarOpcode::kRsqf:
                add(s, "op:exp/log/rcp/rsq"); break;
              case AluScalarOpcode::kKillsEq: case AluScalarOpcode::kKillsGt: case AluScalarOpcode::kKillsGe:
              case AluScalarOpcode::kKillsNe: case AluScalarOpcode::kKillsOne: add(s, "op:kill"); break;
              case AluScalarOpcode::kMaxAs: case AluScalarOpcode::kMaxAsf: add(s, "op:maxas (sets a0)"); break;
              case AluScalarOpcode::kSin: case AluScalarOpcode::kCos: add(s, "op:sin/cos"); break;
              default: break;
            }
          }
        } else if (in.kind == Instruction::Kind::kVertexFetch) {
          const VertexFetch& v = in.vfetch;
          if (v.data_format == 31 || v.data_format == 32) add(s, "vfetch:16-bit float");
          if (v.exp_adjust) add(s, "vfetch:exp_adjust");
          if (v.address.addressing != Addressing::kAbsolute) add(s, "vfetch:relative address");
        } else {
          const TextureFetch& t = in.tfetch;
          if (t.opcode != FetchOpcode::kTextureFetch) add(s, std::string("tfetch:op ") + std::to_string(uint32_t(t.opcode)));
          if (t.dimension == FetchDimension::kCube) add(s, "tfetch:cube");
          if (t.dimension == FetchDimension::k3DOrStacked) add(s, "tfetch:3D/stacked");
          if (t.dimension == FetchDimension::k1D) add(s, "tfetch:1D");
          if (t.offset_x != 0 || t.offset_y != 0 || t.offset_z != 0) add(s, "tfetch:offset");
          if (!t.use_computed_lod || t.use_register_lod || t.lod_bias != 0) add(s, "tfetch:LOD override/bias");
          if (t.use_register_gradients) add(s, "tfetch:register gradients");
          if (t.mag_filter != TextureFilter::kUseFetchConst || t.min_filter != TextureFilter::kUseFetchConst ||
              t.mip_filter != TextureFilter::kUseFetchConst || t.aniso_filter != AnisoFilter::kUseFetchConst)
            add(s, "tfetch:filter override");
          if (t.unnormalized_coordinates) add(s, "tfetch:unnormalized coords");
          if (t.coordinates.addressing != Addressing::kAbsolute) add(s, "tfetch:relative coords");
        }
      }
    }
    for (const auto& x : s) feat[x].push_back(name);
  }
  std::printf("corpus: %zu shaders (%zu vertex, %zu pixel), %zu instructions\n", vs + ps, vs, ps, total_ins);
  for (auto& [k, v] : feat) {
    std::sort(v.begin(), v.end());
    std::printf("%-34s %5zu  e.g. %s%s\n", k.c_str(), v.size(), v[0].c_str(), v.size() > 1 ? (", " + v[1]).c_str() : "");
  }
  return 0;
}
