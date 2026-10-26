// Layer 0 (game-free): Xbox 360 GPU (Xenos) shader microcode, decoded into plain structs, plus a disassembler.
// Stage 3 step S3.2 (notes/36): the decoder our HLSL translator (S3.3) will build on. The disassembly text is
// byte-identical to the SDK's Shader::ucode_disassembly(), which is how the decoder is checked (tools/gn_disasm_check
// over the game's shader corpus).
//
// The microcode layout, the parsing rules and the disassembly format are adapted from Xenia (Copyright Ben Vanik and
// Xenia project contributors) via rexglue, BSD-3-Clause: rexglue include/rex/graphics/format/ucode.h,
// src/graphics/format/ucode.cpp, src/graphics/pipeline/shader/translator.cpp and translator_disasm.cpp. This file and
// ucode.cpp are our own code written from those (MIT), with the original copyright kept here as the licence requires.
//
// Standalone on purpose: no SDK headers, so it can be used by tools, tests and the translator without the runtime (and
// without the rex::graphics::xenos / ::xenos namespace clash).
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace xenos::ucode {

// ---- Enumerations (values as in the microcode) ------------------------------------------------------------------

enum class CfOpcode : uint32_t {
  kNop = 0,
  kExec = 1,
  kExecEnd = 2,
  kCondExec = 3,
  kCondExecEnd = 4,
  kCondExecPred = 5,
  kCondExecPredEnd = 6,
  kLoopStart = 7,
  kLoopEnd = 8,
  kCondCall = 9,
  kReturn = 10,
  kCondJmp = 11,
  kAlloc = 12,
  kCondExecPredClean = 13,
  kCondExecPredCleanEnd = 14,
  kMarkVsFetchDone = 15,
};

enum class AllocType : uint32_t { kNone = 0, kVsPosition = 1, kVsInterpolatorsOrPsColors = 2, kMemory = 3 };

enum class FetchOpcode : uint32_t {
  kVertexFetch = 0,
  kTextureFetch = 1,
  kGetTextureBorderColorFrac = 16,
  kGetTextureComputedLod = 17,
  kGetTextureGradients = 18,
  kGetTextureWeights = 19,
  kSetTextureLod = 24,
  kSetTextureGradientsHorz = 25,
  kSetTextureGradientsVert = 26,
};

enum class FetchDimension : uint32_t { k1D = 0, k2D = 1, k3DOrStacked = 2, kCube = 3 };
// Texture coordinate components a fetch of this dimension reads.
inline uint32_t FetchDimensionComponentCount(FetchDimension d) {
  return d == FetchDimension::k1D ? 1 : d == FetchDimension::k2D ? 2 : 3;
}

// 3 = take the filter from the fetch constant.
enum class TextureFilter : uint32_t { kPoint = 0, kLinear = 1, kBaseMap = 2, kUseFetchConst = 3 };
// 7 = take the filter from the fetch constant.
enum class AnisoFilter : uint32_t {
  kDisabled = 0, kMax1to1 = 1, kMax2to1 = 2, kMax4to1 = 3, kMax8to1 = 4, kMax16to1 = 5, kUseFetchConst = 7
};

enum class AluVectorOpcode : uint32_t {
  kAdd = 0, kMul = 1, kMax = 2, kMin = 3, kSeq = 4, kSgt = 5, kSge = 6, kSne = 7, kFrc = 8, kTrunc = 9, kFloor = 10,
  kMad = 11, kCndEq = 12, kCndGe = 13, kCndGt = 14, kDp4 = 15, kDp3 = 16, kDp2Add = 17, kCube = 18, kMax4 = 19,
  kSetpEqPush = 20, kSetpNePush = 21, kSetpGtPush = 22, kSetpGePush = 23, kKillEq = 24, kKillGt = 25, kKillGe = 26,
  kKillNe = 27, kDst = 28, kMaxA = 29,
};

enum class AluScalarOpcode : uint32_t {
  kAdds = 0, kAddsPrev = 1, kMuls = 2, kMulsPrev = 3, kMulsPrev2 = 4, kMaxs = 5, kMins = 6, kSeqs = 7, kSgts = 8,
  kSges = 9, kSnes = 10, kFrcs = 11, kTruncs = 12, kFloors = 13, kExp = 14, kLogc = 15, kLog = 16, kRcpc = 17,
  kRcpf = 18, kRcp = 19, kRsqc = 20, kRsqf = 21, kRsq = 22, kMaxAs = 23, kMaxAsf = 24, kSubs = 25, kSubsPrev = 26,
  kSetpEq = 27, kSetpNe = 28, kSetpGt = 29, kSetpGe = 30, kSetpInv = 31, kSetpPop = 32, kSetpClr = 33, kSetpRstr = 34,
  kKillsEq = 35, kKillsGt = 36, kKillsGe = 37, kKillsNe = 38, kKillsOne = 39, kSqrt = 40, kMulsc0 = 42, kMulsc1 = 43,
  kAddsc0 = 44, kAddsc1 = 45, kSubsc0 = 46, kSubsc1 = 47, kSin = 48, kCos = 49, kRetainPrev = 50,
};

// Where a component comes from: a source component or a constant.
enum class Swizzle : uint8_t { kX = 0, kY = 1, kZ = 2, kW = 3, k0 = 4, k1 = 5 };

enum class Addressing : uint8_t { kAbsolute, kAddressRegisterRelative /* [n+a0] */, kLoopRelative /* [n+aL] */ };

enum class Target : uint8_t {
  kNone, kRegister, kInterpolator, kPosition, kPointSizeEdgeFlagKillVertex, kExportAddress, kExportData, kColor, kDepth
};

enum class Source : uint8_t { kRegister, kConstantFloat, kVertexFetchConstant, kTextureFetchConstant };

// ---- Operands ---------------------------------------------------------------------------------------------------

struct Result {
  Target target = Target::kNone;
  uint32_t index = 0;
  Addressing addressing = Addressing::kAbsolute;
  bool clamp = false;
  // Write mask exactly as in the microcode (components the target may not have are kept).
  uint32_t write_mask = 0;
  Swizzle components[4] = {Swizzle::kX, Swizzle::kY, Swizzle::kZ, Swizzle::kW};
};

struct Operand {
  Source source = Source::kRegister;
  uint32_t index = 0;
  Addressing addressing = Addressing::kAbsolute;
  bool negate = false;
  bool absolute = false;
  uint32_t component_count = 4;
  Swizzle components[4] = {Swizzle::kX, Swizzle::kY, Swizzle::kZ, Swizzle::kW};
  // Only a full 4-component xyzw read counts as "standard" (no swizzle printed).
  bool IsStandardSwizzle() const {
    return component_count == 4 && components[0] == Swizzle::kX && components[1] == Swizzle::kY &&
           components[2] == Swizzle::kZ && components[3] == Swizzle::kW;
  }
};

// ---- Instructions inside exec blocks ----------------------------------------------------------------------------

struct VertexFetch {
  bool mini = false;  // vfetch_mini: register and fetch constant come from the previous vfetch_full
  bool predicated = false;
  bool predicate_condition = false;
  Result result;
  Operand address;               // register holding the vertex index (1 component)
  uint32_t fetch_constant = 0;   // 0-95 (const_index * 3 + select)
  uint32_t data_format = 0;      // xenos VertexFormat; 0 = undefined
  int32_t offset = 0;            // in dwords
  uint32_t stride = 0;           // in dwords (from the full fetch for minis)
  int32_t exp_adjust = 0;
  uint32_t prefetch_count = 0;   // as encoded (the real count is +1)
  bool index_rounded = false;
  bool is_signed = false;
  bool is_integer = false;       // !normalized
  bool signed_rf_mode = false;
};

struct TextureFetch {
  FetchOpcode opcode = FetchOpcode::kTextureFetch;
  FetchDimension dimension = FetchDimension::k1D;
  bool predicated = false;
  bool predicate_condition = false;
  bool has_result = false;
  Result result;
  Operand coordinates;
  bool has_fetch_constant = false;
  uint32_t fetch_constant = 0;   // 0-31
  bool has_attributes = false;
  bool fetch_valid_only = true;
  bool unnormalized_coordinates = false;
  TextureFilter mag_filter = TextureFilter::kUseFetchConst;
  TextureFilter min_filter = TextureFilter::kUseFetchConst;
  TextureFilter mip_filter = TextureFilter::kUseFetchConst;
  AnisoFilter aniso_filter = AnisoFilter::kUseFetchConst;
  TextureFilter vol_mag_filter = TextureFilter::kUseFetchConst;
  TextureFilter vol_min_filter = TextureFilter::kUseFetchConst;
  bool use_computed_lod = true;
  bool use_register_lod = false;
  bool use_register_gradients = false;
  float lod_bias = 0.0f;
  float offset_x = 0.0f, offset_y = 0.0f, offset_z = 0.0f;
};

// One ALU slot: a vector operation and a scalar operation issued together (either may be a default no-op).
struct Alu {
  bool predicated = false;
  bool predicate_condition = false;
  AluVectorOpcode vector_opcode = AluVectorOpcode::kAdd;
  Result vector_result;  // also carries the constant 0/1 writes of exports
  uint32_t vector_operand_count = 0;
  Operand vector_operands[3];
  AluScalarOpcode scalar_opcode = AluScalarOpcode::kRetainPrev;
  Result scalar_result;
  uint32_t scalar_operand_count = 0;
  Operand scalar_operands[2];
  bool IsVectorOpDefaultNop() const;
  bool IsScalarOpDefaultNop() const;
};

struct Instruction {
  enum class Kind : uint8_t { kAlu, kVertexFetch, kTextureFetch };
  Kind kind = Kind::kAlu;
  uint32_t address = 0;    // instruction slot (3 dwords each)
  bool serialize = false;  // wait for previous results (sequence bit 1)
  Alu alu;
  VertexFetch vfetch;
  TextureFetch tfetch;
};

// ---- Control flow -----------------------------------------------------------------------------------------------

struct ControlFlow {
  CfOpcode opcode = CfOpcode::kNop;
  uint32_t index = 0;  // control flow instruction index (pair * 2 + half)
  // Exec family.
  enum class ExecType : uint8_t { kUnconditional, kConditional /* on a bool constant */, kPredicated };
  ExecType exec_type = ExecType::kUnconditional;
  uint32_t exec_address = 0;
  uint32_t exec_count = 0;
  uint32_t sequence = 0;
  bool exec_end = false;
  bool predicate_clean = true;
  bool yield = false;
  std::vector<Instruction> instructions;
  // Shared by conditional exec / call / jump.
  uint32_t bool_constant = 0;
  bool condition = false;
  // Loops, calls, jumps.
  uint32_t loop_constant = 0;
  uint32_t target = 0;  // loop skip / loop body / call / jump address (control flow index)
  bool repeat = false;
  bool predicated_break = false;
  // Calls and jumps: kUnconditional, kConditional (bool constant), kPredicated.
  ExecType branch_type = ExecType::kUnconditional;
  // Alloc.
  AllocType alloc_type = AllocType::kNone;
  uint32_t alloc_count = 0;
};

// Something the decoder met that it does not fully understand, or where the reference implementations disagree.
// Nothing is dropped silently: every such case is listed here (the S3.2 coverage check reports them).
struct Issue {
  enum class Kind : uint8_t {
    kUnknownFetchOpcode,       // texture-fetch slot with an opcode outside the known set
    kUnknownVectorOpcode,      // vector opcode 30/31
    kUnknownScalarOpcode,      // scalar opcode 41 or 51-63
    kInvalidFetchSwizzle,      // fetch destination swizzle value 6 (treated as 0)
    kUnknownVertexFormat,      // vfetch data format with no name in the reference table
    kUnknownExportRegister,    // export to a register that is not valid for this shader type
    kInstructionPastEnd,       // an exec points past the end of the microcode (read as zeros)
    kMiniFetchWithoutFull,     // vfetch_mini before any vfetch_full (inherits zeros)
    kMarkVsFetchDone,          // control flow opcode 15, which the reference prints as an empty line prefix
    kCanaryTfetch1DTwoComponents,  // tfetch1D with a non-replicated XY swizzle: Canary reads 2 components, rexglue 1
    kCanaryScalarOperandZ,         // 2-component scalar operand with a 3-operand vector op: Canary's `b` is Z, rexglue X
  };
  Kind kind;
  uint32_t address;  // control flow index or instruction slot
  std::string detail;
};

// Choices where Xenia Canary and rexglue decode differently (see notes on Issue::Kind). Default = rexglue, which is
// what the SDK's renderer (our reference) actually runs.
struct DecodeOptions {
  bool canary_tfetch1d_two_components = false;
  bool canary_scalar_operand_z = false;
};

struct Program {
  bool is_vertex_shader = true;
  std::vector<Issue> issues;
  // Control flow instructions in order, up to the guessed end (the first exec's instruction address, as freedreno and
  // Xenia do).
  std::vector<ControlFlow> control_flow;
  // Control flow indices that are branch or loop targets.
  std::set<uint32_t> labels;
};

// Decodes microcode dwords (host byte order, as Shader::ucode_dwords()).
Program Decode(const uint32_t* dwords, size_t dword_count, bool is_vertex_shader, const DecodeOptions& options = {});
const char* IssueKindName(Issue::Kind kind);
// D3D-style disassembly, byte-identical to the SDK's Shader::ucode_disassembly().
std::string Disassemble(const Program& program);

const char* VectorOpcodeName(AluVectorOpcode op);
const char* ScalarOpcodeName(AluScalarOpcode op);

}  // namespace xenos::ucode
