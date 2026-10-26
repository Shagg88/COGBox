// Layer 0 (game-free): DXBC -> SPIR-V for the subset our DXBC writer emits. See dxbc_spirv.h. Our code (MIT).
//
// Model: every DXBC register (r#, x#[n]) is a uvec4 variable; each instruction loads its sources, bitcasts to float
// where the instruction is a float one, computes all four lanes and stores the lanes its write mask names. Float
// arithmetic carries NoContraction (DXBC without "refactoring allowed" is precise; our maths rules forbid FMA
// contraction, notes/38). Where D3D defines a result that SPIR-V leaves undefined (float -> int out of range or NaN,
// shift counts of 32 or more, bit-field extracts past bit 31, integer division by zero) the D3D result is computed.
// Control flow: DXBC SM5 is structured, so if / loop / switch map onto SPIR-V selection and loop constructs; a
// conditional break / return / discard becomes a small selection of its own.

#include <xenos/dxbc_spirv.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <vector>

namespace xenos::dxbc_spirv {

// Patch 0102: the discard measurement switch and its counts.
static std::atomic<bool> g_terminate{false};
static std::atomic<uint64_t> g_ps{0}, g_discards{0}, g_terminated{0};
void SetDiscardTerminates(bool on) { g_terminate.store(on); }
static std::atomic<bool> g_float_controls{true};  // patch 0103
void SetFloatControls(bool on) { g_float_controls.store(on); }
void DiscardCounts(uint64_t& pixel_shaders, uint64_t& discards, uint64_t& terminated) {
  pixel_shaders = g_ps.load();
  discards = g_discards.load();
  terminated = g_terminated.load();
}
namespace {

// ---- SPIR-V numbers (SPIR-V 1.6 specification, GLSL.std.450) ----
enum : uint32_t {
  kOpExtInstImport = 11, kOpExtInst = 12, kOpMemoryModel = 14, kOpEntryPoint = 15, kOpExecutionMode = 16,
  kOpCapability = 17, kOpTypeVoid = 19, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23,
  kOpTypeImage = 25, kOpTypeSampler = 26, kOpTypeSampledImage = 27, kOpTypeArray = 28, kOpTypeRuntimeArray = 29,
  kOpTypeStruct = 30, kOpTypePointer = 32, kOpTypeFunction = 33, kOpConstant = 43, kOpConstantComposite = 44,
  kOpFunction = 54, kOpFunctionEnd = 56, kOpVariable = 59, kOpLoad = 61, kOpStore = 62, kOpAccessChain = 65,
  kOpDecorate = 71, kOpMemberDecorate = 72, kOpVectorShuffle = 79, kOpCompositeConstruct = 80,
  kOpCompositeExtract = 81, kOpSampledImage = 86, kOpImageSampleImplicitLod = 87, kOpImageSampleExplicitLod = 88,
  kOpImageQuerySizeLod = 103, kOpImageQueryLod = 105, kOpImageQueryLevels = 106, kOpConvertFToU = 109,
  kOpConvertFToS = 110, kOpConvertSToF = 111, kOpConvertUToF = 112, kOpBitcast = 124, kOpSNegate = 126,
  kOpFNegate = 127, kOpIAdd = 128, kOpFAdd = 129, kOpISub = 130, kOpIMul = 132, kOpFMul = 133, kOpUDiv = 134,
  kOpFDiv = 136, kOpUMod = 137, kOpIsNan = 156, kOpLogicalOr = 166, kOpLogicalAnd = 167, kOpSelect = 169,
  kOpIEqual = 170, kOpINotEqual = 171, kOpUGreaterThanEqual = 174, kOpSGreaterThanEqual = 175, kOpULessThan = 176,
  kOpSLessThan = 177, kOpFOrdEqual = 180, kOpFUnordNotEqual = 183, kOpFOrdLessThan = 184,
  kOpFOrdGreaterThanEqual = 190, kOpShiftRightLogical = 194, kOpShiftRightArithmetic = 195,
  kOpShiftLeftLogical = 196, kOpBitwiseOr = 197, kOpBitwiseXor = 198, kOpBitwiseAnd = 199, kOpNot = 200,
  kOpAtomicAnd = 240, kOpAtomicOr = 241, kOpLoopMerge = 246, kOpSelectionMerge = 247, kOpLabel = 248,
  kOpBranch = 249, kOpBranchConditional = 250, kOpSwitch = 251, kOpReturn = 253,
  kOpDemoteToHelperInvocation = 5380, kOpTerminateInvocation = 4416,
};
enum : uint32_t {
  kGlslRoundEven = 2, kGlslTrunc = 3, kGlslFAbs = 4, kGlslFloor = 8, kGlslFract = 10, kGlslSin = 13, kGlslCos = 14,
  kGlslExp2 = 29, kGlslLog2 = 30, kGlslSqrt = 31, kGlslUMin = 38, kGlslSMin = 39, kGlslUMax = 41, kGlslSMax = 42,
  kGlslPackHalf2x16 = 58, kGlslNMin = 79, kGlslNMax = 80, kGlslNClamp = 81,
};
enum : uint32_t { kScUniformConstant = 0, kScInput = 1, kScUniform = 2, kScOutput = 3, kScFunction = 7, kScStorageBuffer = 12 };
enum : uint32_t {
  kDecBlock = 2, kDecArrayStride = 6, kDecBuiltIn = 11, kDecNoPerspective = 13, kDecFlat = 14, kDecCentroid = 16,
  kDecNonWritable = 24, kDecLocation = 30, kDecBinding = 33, kDecDescriptorSet = 34, kDecOffset = 35,
  kDecNoContraction = 42,
};
enum : uint32_t { kBiPosition = 0, kBiFragCoord = 15, kBiFrontFacing = 17, kBiFragDepth = 22, kBiVertexIndex = 42 };

// ---- DXBC numbers (D3D10_SB / D3D11_SB token format) ----
enum : uint32_t {
  kDxAdd = 0, kDxAnd = 1, kDxBreak = 2, kDxBreakC = 3, kDxCase = 6, kDxContinue = 7, kDxDefault = 10,
  kDxDiscard = 13, kDxDiv = 14, kDxElse = 18, kDxEndIf = 21, kDxEndLoop = 22, kDxEndSwitch = 23, kDxEq = 24,
  kDxExp = 25, kDxFrc = 26, kDxFToI = 27, kDxFToU = 28, kDxGE = 29, kDxIAdd = 30, kDxIf = 31, kDxIEq = 32,
  kDxIGE = 33, kDxILT = 34, kDxIMAd = 35, kDxIMax = 36, kDxIMin = 37, kDxINE = 39, kDxIShL = 41, kDxIShR = 42,
  kDxIToF = 43, kDxLog = 47, kDxLoop = 48, kDxLT = 49, kDxMin = 51, kDxMax = 52, kDxCustomData = 53, kDxMov = 54,
  kDxMovC = 55, kDxMul = 56, kDxNE = 57, kDxNot = 59, kDxOr = 60, kDxResInfo = 61, kDxRet = 62, kDxRetC = 63,
  kDxRoundNE = 64, kDxRoundNI = 65, kDxRoundZ = 67, kDxSample = 69, kDxSampleL = 72, kDxSampleD = 73,
  kDxSampleB = 74, kDxSqRt = 75, kDxSwitch = 76, kDxSinCos = 77, kDxUDiv = 78, kDxULT = 79, kDxUGE = 80,
  kDxUMax = 83, kDxUMin = 84, kDxUShR = 85, kDxUToF = 86, kDxXOr = 87, kDxDclResource = 88,
  kDxDclConstantBuffer = 89, kDxDclSampler = 90, kDxDclInput = 95, kDxDclInputSGV = 96, kDxDclInputSIV = 97,
  kDxDclInputPS = 98, kDxDclInputPSSGV = 99, kDxDclInputPSSIV = 100, kDxDclOutput = 101, kDxDclOutputSIV = 103,
  kDxDclTemps = 104, kDxDclIndexableTemp = 105, kDxDclGlobalFlags = 106, kDxLOD = 108, kDxF32ToF16 = 130,
  kDxUBFE = 138, kDxIBFE = 139, kDxDclUavRaw = 157, kDxDclResourceRaw = 161, kDxLdRaw = 165, kDxStoreRaw = 166,
  kDxAtomicAnd = 169, kDxAtomicOr = 170,
};
enum : uint32_t {
  kOtTemp = 0, kOtInput = 1, kOtOutput = 2, kOtIndexableTemp = 3, kOtImm32 = 4, kOtSampler = 6, kOtResource = 7,
  kOtCb = 8, kOtOutputDepth = 12, kOtNull = 13, kOtUav = 30,
};
enum : uint32_t { kNamePosition = 1, kNameVertexId = 6, kNameFrontFace = 9 };

struct Operand {
  uint32_t type = 0;
  uint32_t ncomp = 0;  // 0, 1 or 4
  uint32_t mask = 0xF;
  uint32_t sw[4] = {0, 1, 2, 3};
  uint32_t dims = 0;
  struct Index {
    uint32_t imm = 0;
    int rel = -1;  // into Instr::rels
  } idx[3];
  uint32_t imm[4] = {};
  bool neg = false, abs = false;
};
struct Instr {
  uint32_t op = 0, token = 0;
  int32_t off[3] = {};  // sample offsets (u, v, w)
  std::vector<Operand> ops;
  std::vector<Operand> rels;  // registers used as relative indices
  std::vector<uint32_t> extra;  // dwords after the operands (decl counts, names, spaces)
};

class Parser {
 public:
  Parser(const uint32_t* p, size_t n) : p_(p), n_(n) {}
  bool ReadOperand(size_t& i, Instr& in, Operand& o, std::string& err) {
    if (i >= n_) { err = "operand past the end"; return false; }
    const uint32_t t = p_[i++];
    const uint32_t nc = t & 3;
    o.ncomp = nc == 0 ? 0 : nc == 1 ? 1 : nc == 2 ? 4 : 99;
    if (o.ncomp == 99) { err = "N-component operand"; return false; }
    if (o.ncomp == 4) {
      const uint32_t mode = (t >> 2) & 3;
      if (mode == 0) {
        o.mask = (t >> 4) & 0xF;
      } else if (mode == 1) {
        for (int c = 0; c < 4; ++c) o.sw[c] = (t >> (4 + 2 * c)) & 3;
      } else if (mode == 2) {
        const uint32_t s = (t >> 4) & 3;
        for (auto& c : o.sw) c = s;
      }
    } else if (o.ncomp == 1) {
      for (auto& c : o.sw) c = 0;
    }
    o.type = (t >> 12) & 0xFF;
    o.dims = (t >> 20) & 3;
    bool ext = t >> 31;
    while (ext) {
      if (i >= n_) { err = "extended operand past the end"; return false; }
      const uint32_t e = p_[i++];
      if ((e & 0x3F) == 1) {
        const uint32_t m = (e >> 6) & 0xFF;
        o.neg = m == 1 || m == 3;
        o.abs = m == 2 || m == 3;
      } else if ((e & 0x3F) != 0) {
        err = std::format("extended operand type {}", e & 0x3F);
        return false;
      }
      ext = e >> 31;
    }
    if (o.type == kOtImm32) {
      const uint32_t cnt = o.ncomp == 4 ? 4 : 1;
      if (i + cnt > n_) { err = "immediate past the end"; return false; }
      for (uint32_t c = 0; c < cnt; ++c) o.imm[c] = p_[i++];
      if (cnt == 1) o.imm[1] = o.imm[2] = o.imm[3] = o.imm[0];
      return true;
    }
    for (uint32_t d = 0; d < o.dims; ++d) {
      const uint32_t rep = (t >> (22 + 3 * d)) & 7;
      if (rep == 0 || rep == 3) {
        if (i >= n_) { err = "index past the end"; return false; }
        o.idx[d].imm = p_[i++];
      } else if (rep != 2) {
        err = std::format("index representation {}", rep);
        return false;
      }
      if (rep == 2 || rep == 3) {
        Operand r;
        if (!ReadOperand(i, in, r, err)) return false;
        in.rels.push_back(r);
        o.idx[d].rel = int(in.rels.size()) - 1;
      }
    }
    return true;
  }
  const uint32_t* p_;
  size_t n_;
};

// How many register operands each instruction has (before any trailing dwords). -1 = handled specially.
int OperandCount(uint32_t op) {
  switch (op) {
    case kDxBreak: case kDxContinue: case kDxDefault: case kDxElse: case kDxEndIf: case kDxEndLoop:
    case kDxEndSwitch: case kDxLoop: case kDxRet:
      return 0;
    case kDxBreakC: case kDxCase: case kDxDiscard: case kDxIf: case kDxRetC: case kDxSwitch:
      return 1;
    case kDxMov: case kDxExp: case kDxFrc: case kDxFToI: case kDxFToU: case kDxIToF: case kDxLog: case kDxNot:
    case kDxRoundNE: case kDxRoundNI: case kDxRoundZ: case kDxSqRt: case kDxUToF: case kDxF32ToF16:
      return 2;
    case kDxAdd: case kDxAnd: case kDxDiv: case kDxEq: case kDxGE: case kDxIAdd: case kDxIEq: case kDxIGE:
    case kDxILT: case kDxIMax: case kDxIMin: case kDxINE: case kDxIShL: case kDxIShR: case kDxLT: case kDxMin:
    case kDxMax: case kDxMul: case kDxNE: case kDxOr: case kDxULT: case kDxUGE: case kDxUMax: case kDxUMin:
    case kDxUShR: case kDxXOr: case kDxSinCos: case kDxResInfo: case kDxLdRaw: case kDxStoreRaw:
    case kDxAtomicAnd: case kDxAtomicOr:
      return 3;
    case kDxMovC: case kDxIMAd: case kDxUDiv: case kDxUBFE: case kDxIBFE: case kDxSample: case kDxLOD:
      return 4;
    case kDxSampleL: case kDxSampleB:
      return 5;
    case kDxSampleD:
      return 6;
    case kDxDclTemps: case kDxDclGlobalFlags: case kDxDclIndexableTemp:
      return 0;
    case kDxDclInput: case kDxDclInputSGV: case kDxDclInputSIV: case kDxDclInputPS: case kDxDclInputPSSGV:
    case kDxDclInputPSSIV: case kDxDclOutput: case kDxDclOutputSIV: case kDxDclConstantBuffer: case kDxDclSampler:
    case kDxDclResource: case kDxDclResourceRaw: case kDxDclUavRaw:
      return 1;
    default:
      return -1;
  }
}

// ---- The SPIR-V module ----
class Module {
 public:
  uint32_t Id() { return next_++; }
  static void Put(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
    v.push_back(op | (uint32_t(w.size() + 1) << 16));
    v.insert(v.end(), w.begin(), w.end());
  }
  static void PutV(std::vector<uint32_t>& v, uint32_t op, const std::vector<uint32_t>& w) {
    v.push_back(op | (uint32_t(w.size() + 1) << 16));
    v.insert(v.end(), w.begin(), w.end());
  }
  // A type or constant, shared by identical declarations.
  uint32_t Cached(uint32_t op, std::vector<uint32_t> args, bool result_type_first = false) {
    std::vector<uint32_t> key = args;
    key.insert(key.begin(), op);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const uint32_t id = Id();
    std::vector<uint32_t> w;
    if (result_type_first) {
      w.push_back(args[0]);
      w.push_back(id);
      w.insert(w.end(), args.begin() + 1, args.end());
    } else {
      w.push_back(id);
      w.insert(w.end(), args.begin(), args.end());
    }
    PutV(types, op, w);
    cache_[key] = id;
    return id;
  }
  std::vector<uint32_t> caps, ext, modes, annot, types, globals, fn, entry_ifaces;
  uint32_t next_ = 1;

 private:
  std::map<std::vector<uint32_t>, uint32_t> cache_;
};

struct Resource {
  uint32_t reg = 0;
  uint32_t dim = 0;  // DXBC resource dimension (3 = 2D, 5 = 3D, 6 = cube)
  uint32_t var = 0, image_type = 0;
};

class Converter {
 public:
  SpirvResult Run(const uint32_t* shex, size_t n);
  bool precise_ = true;  // patch 0088: false = no NoContraction (measurement)

 private:
  bool Fail(const std::string& why) {
    if (error_.empty()) error_ = why;
    return false;
  }
  // Types / constants.
  uint32_t TVoid() { return m_.Cached(kOpTypeVoid, {}); }
  uint32_t TBool() { return m_.Cached(kOpTypeBool, {}); }
  uint32_t TU() { return m_.Cached(kOpTypeInt, {32, 0}); }
  uint32_t TI() { return m_.Cached(kOpTypeInt, {32, 1}); }
  uint32_t TF() { return m_.Cached(kOpTypeFloat, {32}); }
  uint32_t TVec(uint32_t t, uint32_t n) { return m_.Cached(kOpTypeVector, {t, n}); }
  uint32_t TU4() { return TVec(TU(), 4); }
  uint32_t TF4() { return TVec(TF(), 4); }
  uint32_t TB4() { return TVec(TBool(), 4); }
  uint32_t TPtr(uint32_t sc, uint32_t t) { return m_.Cached(kOpTypePointer, {sc, t}); }
  uint32_t CU(uint32_t v) { return m_.Cached(kOpConstant, {TU(), v}, true); }
  uint32_t CI(int32_t v) { return m_.Cached(kOpConstant, {TI(), uint32_t(v)}, true); }
  uint32_t CF(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return m_.Cached(kOpConstant, {TF(), b}, true);
  }
  uint32_t CU4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return m_.Cached(kOpConstantComposite, {TU4(), CU(a), CU(b), CU(c), CU(d)}, true);
  }
  uint32_t CF4(float a, float b, float c, float d) {
    return m_.Cached(kOpConstantComposite, {TF4(), CF(a), CF(b), CF(c), CF(d)}, true);
  }
  uint32_t SplatU(uint32_t v) { return CU4(v, v, v, v); }
  uint32_t SplatF(float v) { return CF4(v, v, v, v); }
  void Decorate(uint32_t id, uint32_t dec, std::initializer_list<uint32_t> args = {}) {
    std::vector<uint32_t> w{id, dec};
    w.insert(w.end(), args.begin(), args.end());
    Module::PutV(m_.annot, kOpDecorate, w);
  }
  // Function code.
  uint32_t Op(uint32_t op, uint32_t type, std::initializer_list<uint32_t> args) {
    Open();
    const uint32_t id = m_.Id();
    std::vector<uint32_t> w{type, id};
    w.insert(w.end(), args.begin(), args.end());
    Module::PutV(m_.fn, op, w);
    return id;
  }
  uint32_t OpV(uint32_t op, uint32_t type, const std::vector<uint32_t>& args) {
    Open();
    const uint32_t id = m_.Id();
    std::vector<uint32_t> w{type, id};
    w.insert(w.end(), args.begin(), args.end());
    Module::PutV(m_.fn, op, w);
    return id;
  }
  void Void(uint32_t op, std::initializer_list<uint32_t> args) {
    Open();
    Module::Put(m_.fn, op, args);
  }
  uint32_t Precise(uint32_t id) {
    if (precise_) Decorate(id, kDecNoContraction);
    return id;
  }
  uint32_t Glsl(uint32_t type, uint32_t inst, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{glsl_, inst};
    w.insert(w.end(), args.begin(), args.end());
    return OpV(kOpExtInst, type, w);
  }
  uint32_t ToF(uint32_t u4) { return Op(kOpBitcast, TF4(), {u4}); }
  uint32_t ToU(uint32_t f4) { return Op(kOpBitcast, TU4(), {f4}); }
  uint32_t Shuffle(uint32_t type, uint32_t a, uint32_t b, std::initializer_list<uint32_t> lanes) {
    std::vector<uint32_t> w{a, b};
    w.insert(w.end(), lanes.begin(), lanes.end());
    return OpV(kOpVectorShuffle, type, w);
  }
  uint32_t X(uint32_t u4) { return Op(kOpCompositeExtract, TU(), {u4, 0}); }
  uint32_t BoolToMask(uint32_t b4) { return Op(kOpSelect, TU4(), {b4, SplatU(~0u), SplatU(0)}); }

  // Blocks.
  void Label(uint32_t id) {
    Module::Put(m_.fn, kOpLabel, {id});
    open_ = true;
  }
  void Open() {
    if (!open_) Label(m_.Id());  // code after a break / return: an unreachable block of its own
  }
  void Terminate(uint32_t op, std::initializer_list<uint32_t> args) {
    Open();
    Module::Put(m_.fn, op, args);
    open_ = false;
  }

  bool Declare(const Instr& in);
  bool Emit(size_t index, const Instr& in);
  // Operands.
  uint32_t IndexValue(const Instr& in, const Operand::Index& ix);
  uint32_t LoadRaw(const Instr& in, const Operand& o);
  uint32_t SrcU(const Instr& in, const Operand& o);
  uint32_t SrcF(const Instr& in, const Operand& o);
  uint32_t CondX(const Instr& in, const Operand& o);  // bool: .x != 0 (or == 0 per the instruction's test bit)
  bool Store(const Instr& in, const Operand& d, uint32_t u4, bool float_result);
  uint32_t Sampled(const Operand& t, const Operand& s, Resource*& res);
  uint32_t Coords(const Instr& in, const Operand& src, const Resource& r);
  uint32_t ApplySwizzle(uint32_t u4, const Operand& o) {
    if (o.sw[0] == 0 && o.sw[1] == 1 && o.sw[2] == 2 && o.sw[3] == 3) return u4;
    return Shuffle(TU4(), u4, u4, {o.sw[0], o.sw[1], o.sw[2], o.sw[3]});
  }
  uint32_t InputPtr(const Instr& in, const Operand& o, uint32_t& kind);

  Module m_;
  std::string error_;
  bool pixel_ = false;
  bool open_ = false;

  uint32_t glsl_ = 0;
  uint32_t main_ = 0;
  std::vector<uint32_t> temps_;                 // r#: uvec4 Function variables
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> xtemps_;  // x#: variable, size
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> cbs_;     // range id -> variable, vec4 count
  std::map<uint32_t, Resource> textures_;       // range id
  std::map<uint32_t, uint32_t> samplers_;       // range id -> variable
  std::map<uint32_t, uint32_t> raw_srvs_;       // range id -> variable (struct { uint[] })
  std::map<uint32_t, uint32_t> uavs_;           // range id -> variable
  // Inputs / outputs by register: kind 1 texcoord element, 2 FragCoord, 3 FrontFacing, 4 VertexIndex; outputs:
  // 1 texcoord element, 2 Position, 3 SV_Target.
  std::map<uint32_t, uint32_t> in_kind_, out_kind_;
  uint32_t interp_in_ = 0, interp_out_ = 0, frag_coord_ = 0, front_face_ = 0, vertex_index_ = 0, position_ = 0;
  uint32_t depth_out_ = 0;
  std::map<uint32_t, uint32_t> targets_;  // o# -> SV_Target variable
  uint32_t interp_mode_ = 0;
  bool interp_mode_set_ = false;

  struct Construct {
    enum Kind { kIf, kLoop, kSwitch } kind = kIf;
    uint32_t merge = 0, cont = 0, header = 0, else_label = 0;
    bool has_else = false;
    size_t switch_index = 0;
    std::vector<uint32_t> labels;  // switch: per label group
    std::vector<bool> started;
  };
  std::vector<Construct> cf_;
  // Switch pre-scan: per switch instruction index, the case groups.
  struct SwitchInfo {
    std::vector<std::pair<uint32_t, uint32_t>> cases;  // value, group
    int default_group = -1;
    uint32_t groups = 0;
  };
  std::map<size_t, SwitchInfo> switches_;
  std::map<size_t, uint32_t> case_group_;  // case / default instruction index -> group
};

uint32_t Converter::IndexValue(const Instr& in, const Operand::Index& ix) {
  if (ix.rel < 0) return CU(ix.imm);
  uint32_t v = X(SrcU(in, in.rels[size_t(ix.rel)]));
  if (ix.imm) v = Op(kOpIAdd, TU(), {v, CU(ix.imm)});
  return v;
}

uint32_t Converter::InputPtr(const Instr& in, const Operand& o, uint32_t& kind) {
  const uint32_t reg = o.idx[0].imm;
  auto it = in_kind_.find(reg);
  if (o.idx[0].rel >= 0 || it == in_kind_.end()) {
    // A relatively indexed input only exists for the texcoord array.
    if (o.idx[0].rel >= 0 && interp_in_) {
      kind = 1;
      return Op(kOpAccessChain, TPtr(kScInput, TF4()), {interp_in_, IndexValue(in, o.idx[0])});
    }
    Fail(std::format("input v{} not declared", reg));
    kind = 0;
    return 0;
  }
  kind = it->second;
  if (kind == 1) return Op(kOpAccessChain, TPtr(kScInput, TF4()), {interp_in_, CU(reg)});
  return 0;
}

uint32_t Converter::LoadRaw(const Instr& in, const Operand& o) {
  switch (o.type) {
    case kOtTemp: {
      const uint32_t r = o.idx[0].imm;
      if (r >= temps_.size()) { Fail(std::format("r{} past dcl_temps", r)); return SplatU(0); }
      return Op(kOpLoad, TU4(), {temps_[r]});
    }
    case kOtIndexableTemp: {
      auto it = xtemps_.find(o.idx[0].imm);
      if (it == xtemps_.end()) { Fail("x# not declared"); return SplatU(0); }
      const uint32_t p = Op(kOpAccessChain, TPtr(kScFunction, TU4()), {it->second.first, IndexValue(in, o.idx[1])});
      return Op(kOpLoad, TU4(), {p});
    }
    case kOtImm32:
      return CU4(o.imm[0], o.imm[1], o.imm[2], o.imm[3]);
    case kOtCb: {
      auto it = cbs_.find(o.idx[0].imm);
      if (it == cbs_.end()) { Fail("cb not declared"); return SplatU(0); }
      const uint32_t p =
          Op(kOpAccessChain, TPtr(kScUniform, TU4()), {it->second.first, CU(0), IndexValue(in, o.idx[2])});
      return Op(kOpLoad, TU4(), {p});
    }
    case kOtInput: {
      uint32_t kind = 0;
      const uint32_t p = InputPtr(in, o, kind);
      if (kind == 1) return ToU(Op(kOpLoad, TF4(), {p}));
      if (kind == 2) return ToU(Op(kOpLoad, TF4(), {frag_coord_}));
      if (kind == 3) {
        const uint32_t b = Op(kOpLoad, TBool(), {front_face_});
        const uint32_t x = Op(kOpSelect, TU(), {b, CU(~0u), CU(0)});
        return Op(kOpCompositeConstruct, TU4(), {x, CU(0), CU(0), CU(0)});
      }
      if (kind == 4) {
        const uint32_t x = Op(kOpLoad, TU(), {vertex_index_});
        return Op(kOpCompositeConstruct, TU4(), {x, CU(0), CU(0), CU(0)});
      }
      return SplatU(0);
    }
    default:
      Fail(std::format("source operand type {}", o.type));
      return SplatU(0);
  }
}

uint32_t Converter::SrcU(const Instr& in, const Operand& o) {
  if (o.abs) Fail("abs on an integer operand");
  uint32_t v = ApplySwizzle(LoadRaw(in, o), o);
  if (o.neg) v = Op(kOpSNegate, TU4(), {v});
  return v;
}

uint32_t Converter::SrcF(const Instr& in, const Operand& o) {
  uint32_t v = ToF(ApplySwizzle(LoadRaw(in, o), o));
  if (o.abs) v = Glsl(TF4(), kGlslFAbs, {v});
  if (o.neg) v = Op(kOpFNegate, TF4(), {v});
  return v;
}

uint32_t Converter::CondX(const Instr& in, const Operand& o) {
  const uint32_t x = X(SrcU(in, o));
  const bool nz = (in.token >> 18) & 1;
  return Op(nz ? kOpINotEqual : kOpIEqual, TBool(), {x, CU(0)});
}

bool Converter::Store(const Instr& in, const Operand& d, uint32_t u4, bool float_result) {
  if (d.type == kOtNull) return true;
  if (float_result && ((in.token >> 13) & 1)) {  // _sat (NaN -> 0)
    u4 = ToU(Glsl(TF4(), kGlslNClamp, {ToF(u4), SplatF(0.0f), SplatF(1.0f)}));
  }
  const uint32_t mask = d.ncomp == 1 ? 1 : d.mask;
  switch (d.type) {
    case kOtTemp:
    case kOtIndexableTemp: {
      uint32_t ptr = 0;
      if (d.type == kOtTemp) {
        if (d.idx[0].imm >= temps_.size()) return Fail("r# past dcl_temps");
        ptr = temps_[d.idx[0].imm];
      } else {
        auto it = xtemps_.find(d.idx[0].imm);
        if (it == xtemps_.end()) return Fail("x# not declared");
        ptr = Op(kOpAccessChain, TPtr(kScFunction, TU4()), {it->second.first, IndexValue(in, d.idx[1])});
      }
      if (mask != 0xF) {
        const uint32_t old = Op(kOpLoad, TU4(), {ptr});
        u4 = Shuffle(TU4(), old, u4,
                     {mask & 1 ? 4u : 0u, mask & 2 ? 5u : 1u, mask & 4 ? 6u : 2u, mask & 8 ? 7u : 3u});
      }
      Void(kOpStore, {ptr, u4});
      return true;
    }
    case kOtOutput: {
      const uint32_t reg = d.idx[0].imm;
      if (d.idx[0].rel >= 0) return Fail("relatively indexed output");
      auto it = out_kind_.find(reg);
      if (it == out_kind_.end()) return Fail(std::format("output o{} not declared", reg));
      uint32_t var = 0, base_ptr = 0;
      if (it->second == 1) base_ptr = Op(kOpAccessChain, TPtr(kScOutput, TF4()), {interp_out_, CU(reg)});
      else if (it->second == 2) var = position_;
      else var = targets_[reg];
      const uint32_t f = ToF(u4);
      if (mask == 0xF) {
        Void(kOpStore, {base_ptr ? base_ptr : var, f});
        return true;
      }
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c))) continue;
        const uint32_t p = Op(kOpAccessChain, TPtr(kScOutput, TF()), {base_ptr ? base_ptr : var, CU(c)});
        Void(kOpStore, {p, Op(kOpCompositeExtract, TF(), {f, c})});
      }
      return true;
    }
    case kOtOutputDepth: {
      if (!depth_out_) return Fail("oDepth not declared");
      Void(kOpStore, {depth_out_, Op(kOpCompositeExtract, TF(), {ToF(u4), 0})});
      return true;
    }
    default:
      return Fail(std::format("destination operand type {}", d.type));
  }
}

uint32_t Converter::Sampled(const Operand& t, const Operand& s, Resource*& res) {
  auto ti = textures_.find(t.idx[0].imm);
  auto si = samplers_.find(s.idx[0].imm);
  if (ti == textures_.end() || si == samplers_.end()) {
    Fail("texture or sampler not declared");
    res = nullptr;
    return 0;
  }
  res = &ti->second;
  const uint32_t img = Op(kOpLoad, res->image_type, {res->var});
  const uint32_t smp = Op(kOpLoad, m_.Cached(kOpTypeSampler, {}), {si->second});
  return Op(kOpSampledImage, m_.Cached(kOpTypeSampledImage, {res->image_type}), {img, smp});
}

uint32_t Converter::Coords(const Instr& in, const Operand& src, const Resource& r) {
  const uint32_t f = SrcF(in, src);
  if (r.dim == 3) return Shuffle(TVec(TF(), 2), f, f, {0, 1});
  return Shuffle(TVec(TF(), 3), f, f, {0, 1, 2});
}

bool Converter::Declare(const Instr& in) {
  switch (in.op) {
    case kDxDclGlobalFlags:
      // Patch 0095: "refactoring allowed" (our DXBC sets it only on pixel shaders that don't write depth, with
      // --gow2_renderer_ps_refactoring) = no NoContraction, as the D3D12 driver may then fuse / reorder too.
      if (in.token & (1u << 11)) precise_ = false;
      return true;
    case kDxDclTemps: {
      const uint32_t n = in.extra.empty() ? 0 : in.extra[0];
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = m_.Id();
        temps_.push_back(v);
      }
      return true;
    }
    case kDxDclIndexableTemp: {
      if (in.extra.size() < 3) return Fail("dcl_indexableTemp");
      const uint32_t arr = m_.Cached(kOpTypeArray, {TU4(), CU(in.extra[1])});
      xtemps_[in.extra[0]] = {m_.Id(), arr};
      return true;
    }
    case kDxDclConstantBuffer: {
      const Operand& o = in.ops[0];
      if (o.dims != 3 || in.extra.size() < 2) return Fail("dcl_constantbuffer (not SM 5.1)");
      const uint32_t id = o.idx[0].imm, reg = o.idx[1].imm, count = std::max(1u, in.extra[0]);
      if (in.extra[1] != 0) return Fail("constant buffer space");
      const uint32_t arr = m_.Id();
      Module::Put(m_.types, kOpTypeArray, {arr, TU4(), CU(count)});
      Decorate(arr, kDecArrayStride, {16});
      const uint32_t st = m_.Id();
      Module::Put(m_.types, kOpTypeStruct, {st, arr});
      Decorate(st, kDecBlock);
      Module::Put(m_.annot, kOpMemberDecorate, {st, 0, kDecOffset, 0});
      const uint32_t var = m_.Id();
      Module::Put(m_.globals, kOpVariable, {TPtr(kScUniform, st), var, kScUniform});
      Decorate(var, kDecDescriptorSet, {0});
      Decorate(var, kDecBinding, {reg});
      m_.entry_ifaces.push_back(var);
      cbs_[id] = {var, count};
      return true;
    }
    case kDxDclSampler: {
      const Operand& o = in.ops[0];
      if (o.dims != 3 || in.extra.empty() || in.extra[0] != 0) return Fail("dcl_sampler");
      const uint32_t var = m_.Id();
      Module::Put(m_.globals, kOpVariable,
                  {TPtr(kScUniformConstant, m_.Cached(kOpTypeSampler, {})), var, kScUniformConstant});
      Decorate(var, kDecDescriptorSet, {2});
      Decorate(var, kDecBinding, {o.idx[1].imm});
      m_.entry_ifaces.push_back(var);
      samplers_[o.idx[0].imm] = var;
      return true;
    }
    case kDxDclResource: {
      const Operand& o = in.ops[0];
      if (o.dims != 3 || in.extra.size() < 2 || in.extra[1] != 0) return Fail("dcl_resource");
      Resource r;
      r.reg = o.idx[1].imm;
      r.dim = (in.token >> 11) & 0x1F;
      uint32_t sdim = 0;
      if (r.dim == 3) sdim = 1;       // 2D
      else if (r.dim == 5) sdim = 2;  // 3D
      else if (r.dim == 6) sdim = 3;  // cube
      else return Fail(std::format("resource dimension {}", r.dim));
      if (in.extra[0] != 0x5555) return Fail(std::format("resource return type {:X}", in.extra[0]));  // float4
      r.image_type = m_.Cached(kOpTypeImage, {TF(), sdim, 2, 0, 0, 1, 0});
      r.var = m_.Id();
      Module::Put(m_.globals, kOpVariable, {TPtr(kScUniformConstant, r.image_type), r.var, kScUniformConstant});
      Decorate(r.var, kDecDescriptorSet, {1});
      Decorate(r.var, kDecBinding, {r.reg});
      m_.entry_ifaces.push_back(r.var);
      textures_[o.idx[0].imm] = r;
      return true;
    }
    case kDxDclResourceRaw:
    case kDxDclUavRaw: {
      const Operand& o = in.ops[0];
      if (o.dims != 3 || in.extra.empty() || in.extra[0] != 0) return Fail("raw buffer declaration");
      const bool uav = in.op == kDxDclUavRaw;
      const uint32_t rt = m_.Id();
      Module::Put(m_.types, kOpTypeRuntimeArray, {rt, TU()});
      Decorate(rt, kDecArrayStride, {4});
      const uint32_t st = m_.Id();
      Module::Put(m_.types, kOpTypeStruct, {st, rt});
      Decorate(st, kDecBlock);
      Module::Put(m_.annot, kOpMemberDecorate, {st, 0, kDecOffset, 0});
      if (!uav) Module::Put(m_.annot, kOpMemberDecorate, {st, 0, kDecNonWritable});
      const uint32_t var = m_.Id();
      Module::Put(m_.globals, kOpVariable, {TPtr(kScStorageBuffer, st), var, kScStorageBuffer});
      // HLSL path: t96 / t97 have [[vk::binding(9x, 1)]]; u0 has none, so DXC puts it at set 0, binding 300 + reg.
      Decorate(var, kDecDescriptorSet, {uav ? 0u : 1u});
      Decorate(var, kDecBinding, {uav ? 300 + o.idx[1].imm : o.idx[1].imm});
      m_.entry_ifaces.push_back(var);
      (uav ? uavs_ : raw_srvs_)[o.idx[0].imm] = var;
      return true;
    }
    case kDxDclInputSGV:
    case kDxDclInputSIV:
    case kDxDclInputPSSGV:
    case kDxDclInputPSSIV: {
      const uint32_t reg = in.ops[0].idx[0].imm;
      const uint32_t name = in.extra.empty() ? 0 : in.extra[0];
      if (name == kNamePosition && pixel_) {
        frag_coord_ = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScInput, TF4()), frag_coord_, kScInput});
        Decorate(frag_coord_, kDecBuiltIn, {kBiFragCoord});
        m_.entry_ifaces.push_back(frag_coord_);
        in_kind_[reg] = 2;
      } else if (name == kNameFrontFace && pixel_) {
        front_face_ = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScInput, TBool()), front_face_, kScInput});
        Decorate(front_face_, kDecBuiltIn, {kBiFrontFacing});
        Decorate(front_face_, kDecFlat);
        m_.entry_ifaces.push_back(front_face_);
        in_kind_[reg] = 3;
      } else if (name == kNameVertexId && !pixel_) {
        vertex_index_ = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScInput, TU()), vertex_index_, kScInput});
        Decorate(vertex_index_, kDecBuiltIn, {kBiVertexIndex});
        m_.entry_ifaces.push_back(vertex_index_);
        in_kind_[reg] = 4;
      } else {
        return Fail(std::format("input system value {}", name));
      }
      return true;
    }
    case kDxDclInputPS: {
      const uint32_t reg = in.ops[0].idx[0].imm;
      if (reg >= 16) return Fail(std::format("pixel input v{}", reg));
      const uint32_t mode = (in.token >> 11) & 0xF;
      if (interp_mode_set_ && mode != interp_mode_) return Fail("mixed interpolation modes");
      interp_mode_ = mode;
      interp_mode_set_ = true;
      in_kind_[reg] = 1;
      return true;
    }
    case kDxDclInput:
      return Fail("plain dcl_input");
    case kDxDclOutput: {
      const uint32_t reg = in.ops[0].idx[0].imm;
      if (in.ops[0].type == kOtOutputDepth) {
        depth_out_ = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScOutput, TF()), depth_out_, kScOutput});
        Decorate(depth_out_, kDecBuiltIn, {kBiFragDepth});
        m_.entry_ifaces.push_back(depth_out_);
        return true;
      }
      if (pixel_) {
        if (reg >= 4) return Fail(std::format("pixel output o{}", reg));
        const uint32_t v = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScOutput, TF4()), v, kScOutput});
        Decorate(v, kDecLocation, {reg});
        m_.entry_ifaces.push_back(v);
        targets_[reg] = v;
        out_kind_[reg] = 3;
      } else {
        if (reg >= 16) return Fail(std::format("vertex output o{}", reg));
        out_kind_[reg] = 1;
      }
      return true;
    }
    case kDxDclOutputSIV: {
      const uint32_t reg = in.ops[0].idx[0].imm;
      const uint32_t name = in.extra.empty() ? 0 : in.extra[0];
      if (name != kNamePosition || pixel_) return Fail(std::format("output system value {}", name));
      position_ = m_.Id();
      Module::Put(m_.globals, kOpVariable, {TPtr(kScOutput, TF4()), position_, kScOutput});
      Decorate(position_, kDecBuiltIn, {kBiPosition});
      m_.entry_ifaces.push_back(position_);
      out_kind_[reg] = 2;
      return true;
    }
    default:
      return Fail(std::format("declaration {}", in.op));
  }
}

bool Converter::Emit(size_t index, const Instr& in) {
  const auto& o = in.ops;
  auto F2 = [&](uint32_t op) {  // float binary
    const uint32_t r = Precise(Op(op, TF4(), {SrcF(in, o[1]), SrcF(in, o[2])}));
    return Store(in, o[0], ToU(r), true);
  };
  auto FCmp = [&](uint32_t op) {
    const uint32_t b = Op(op, TB4(), {SrcF(in, o[1]), SrcF(in, o[2])});
    return Store(in, o[0], BoolToMask(b), false);
  };
  auto I2 = [&](uint32_t op) { return Store(in, o[0], Op(op, TU4(), {SrcU(in, o[1]), SrcU(in, o[2])}), false); };
  auto ICmp = [&](uint32_t op) {
    return Store(in, o[0], BoolToMask(Op(op, TB4(), {SrcU(in, o[1]), SrcU(in, o[2])})), false);
  };
  auto G1 = [&](uint32_t inst) { return Store(in, o[0], ToU(Glsl(TF4(), inst, {SrcF(in, o[1])})), true); };
  auto Shift = [&](uint32_t op) {
    const uint32_t n = Op(kOpBitwiseAnd, TU4(), {SrcU(in, o[2]), SplatU(31)});
    return Store(in, o[0], Op(op, TU4(), {SrcU(in, o[1]), n}), false);
  };
  switch (in.op) {
    // ---- control flow ----
    case kDxIf: {
      const uint32_t c = CondX(in, o[0]);
      Construct k;
      k.kind = Construct::kIf;
      const uint32_t then_label = m_.Id();
      k.merge = m_.Id();
      k.else_label = m_.Id();
      Void(kOpSelectionMerge, {k.merge, 0});
      Terminate(kOpBranchConditional, {c, then_label, k.else_label});
      Label(then_label);
      cf_.push_back(k);
      return true;
    }
    case kDxElse: {
      if (cf_.empty() || cf_.back().kind != Construct::kIf || cf_.back().has_else) return Fail("else without if");
      if (open_) Terminate(kOpBranch, {cf_.back().merge});
      Label(cf_.back().else_label);
      cf_.back().has_else = true;
      return true;
    }
    case kDxEndIf: {
      if (cf_.empty() || cf_.back().kind != Construct::kIf) return Fail("endif without if");
      Construct k = cf_.back();
      cf_.pop_back();
      if (open_) Terminate(kOpBranch, {k.merge});
      if (!k.has_else) {
        Label(k.else_label);
        Terminate(kOpBranch, {k.merge});
      }
      Label(k.merge);
      return true;
    }
    case kDxLoop: {
      Construct k;
      k.kind = Construct::kLoop;
      k.header = m_.Id();
      k.merge = m_.Id();
      k.cont = m_.Id();
      const uint32_t body = m_.Id();
      Terminate(kOpBranch, {k.header});
      Label(k.header);
      Void(kOpLoopMerge, {k.merge, k.cont, 0});
      Terminate(kOpBranch, {body});
      Label(body);
      cf_.push_back(k);
      return true;
    }
    case kDxEndLoop: {
      if (cf_.empty() || cf_.back().kind != Construct::kLoop) return Fail("endloop without loop");
      Construct k = cf_.back();
      cf_.pop_back();
      if (open_) Terminate(kOpBranch, {k.cont});
      Label(k.cont);
      Terminate(kOpBranch, {k.header});
      Label(k.merge);
      return true;
    }
    case kDxBreak:
    case kDxBreakC:
    case kDxContinue: {
      uint32_t target = 0;
      for (auto it = cf_.rbegin(); it != cf_.rend(); ++it) {
        if (in.op == kDxContinue ? it->kind == Construct::kLoop : it->kind != Construct::kIf) {
          target = in.op == kDxContinue ? it->cont : it->merge;
          break;
        }
      }
      if (!target) return Fail("break / continue outside a loop or switch");
      if (in.op == kDxBreakC) {
        const uint32_t c = CondX(in, o[0]);
        const uint32_t t = m_.Id(), merge = m_.Id();
        Void(kOpSelectionMerge, {merge, 0});
        Terminate(kOpBranchConditional, {c, t, merge});
        Label(t);
        Terminate(kOpBranch, {target});
        Label(merge);
      } else {
        Terminate(kOpBranch, {target});
      }
      return true;
    }
    case kDxRet:
      Terminate(kOpReturn, {});
      return true;
    case kDxRetC: {
      const uint32_t c = CondX(in, o[0]);
      const uint32_t t = m_.Id(), merge = m_.Id();
      Void(kOpSelectionMerge, {merge, 0});
      Terminate(kOpBranchConditional, {c, t, merge});
      Label(t);
      Terminate(kOpReturn, {});
      Label(merge);
      return true;
    }
    case kDxDiscard: {
      if (!pixel_) return Fail("discard in a vertex shader");
      const uint32_t c = CondX(in, o[0]);
      const uint32_t t = m_.Id(), merge = m_.Id();
      Void(kOpSelectionMerge, {merge, 0});
      Terminate(kOpBranchConditional, {c, t, merge});
      Label(t);
      g_discards.fetch_add(1, std::memory_order_relaxed);  // patch 0102
      if (g_terminate.load(std::memory_order_relaxed)) {
        g_terminated.fetch_add(1, std::memory_order_relaxed);
        Terminate(kOpTerminateInvocation, {});
      } else {
        Void(kOpDemoteToHelperInvocation, {});
        Terminate(kOpBranch, {merge});
      }
      Label(merge);
      return true;
    }
    case kDxSwitch: {
      auto si = switches_.find(index);
      if (si == switches_.end()) return Fail("switch not pre-scanned");
      const uint32_t sel = X(SrcU(in, o[0]));
      Construct k;
      k.kind = Construct::kSwitch;
      k.merge = m_.Id();
      k.switch_index = index;
      for (uint32_t g = 0; g < si->second.groups; ++g) k.labels.push_back(m_.Id());
      k.started.assign(si->second.groups, false);
      Void(kOpSelectionMerge, {k.merge, 0});
      std::vector<uint32_t> w{sel, si->second.default_group >= 0 ? k.labels[size_t(si->second.default_group)] : k.merge};
      for (const auto& [value, group] : si->second.cases) {
        w.push_back(value);
        w.push_back(k.labels[group]);
      }
      Open();
      Module::PutV(m_.fn, kOpSwitch, w);
      open_ = false;
      cf_.push_back(k);
      return true;
    }
    case kDxCase:
    case kDxDefault: {
      if (cf_.empty() || cf_.back().kind != Construct::kSwitch) return Fail("case outside a switch");
      Construct& k = cf_.back();
      const uint32_t g = case_group_[index];
      if (k.started[g]) return true;  // a second label of the same group
      if (open_) Terminate(kOpBranch, {k.labels[g]});  // fall through from the previous case
      Label(k.labels[g]);
      k.started[g] = true;
      return true;
    }
    case kDxEndSwitch: {
      if (cf_.empty() || cf_.back().kind != Construct::kSwitch) return Fail("endswitch without switch");
      Construct k = cf_.back();
      cf_.pop_back();
      if (open_) Terminate(kOpBranch, {k.merge});
      Label(k.merge);
      return true;
    }

    // ---- moves and selects ----
    case kDxMov:
      if (o[1].abs || o[1].neg || ((in.token >> 13) & 1)) return Store(in, o[0], ToU(SrcF(in, o[1])), true);
      return Store(in, o[0], SrcU(in, o[1]), false);
    case kDxMovC: {
      const uint32_t c = Op(kOpINotEqual, TB4(), {SrcU(in, o[1]), SplatU(0)});
      const bool fl = o[2].abs || o[2].neg || o[3].abs || o[3].neg || ((in.token >> 13) & 1);
      const uint32_t a = fl ? ToU(SrcF(in, o[2])) : SrcU(in, o[2]);
      const uint32_t b = fl ? ToU(SrcF(in, o[3])) : SrcU(in, o[3]);
      return Store(in, o[0], Op(kOpSelect, TU4(), {c, a, b}), fl);
    }

    // ---- float ----
    case kDxAdd: return F2(kOpFAdd);
    case kDxMul: return F2(kOpFMul);
    case kDxDiv: return F2(kOpFDiv);
    case kDxMin:
      return Store(in, o[0], ToU(Glsl(TF4(), kGlslNMin, {SrcF(in, o[1]), SrcF(in, o[2])})), true);
    case kDxMax:
      return Store(in, o[0], ToU(Glsl(TF4(), kGlslNMax, {SrcF(in, o[1]), SrcF(in, o[2])})), true);
    case kDxEq: return FCmp(kOpFOrdEqual);
    case kDxNE: return FCmp(kOpFUnordNotEqual);
    case kDxLT: return FCmp(kOpFOrdLessThan);
    case kDxGE: return FCmp(kOpFOrdGreaterThanEqual);
    case kDxRoundNE: return G1(kGlslRoundEven);
    case kDxRoundNI: return G1(kGlslFloor);
    case kDxRoundZ: return G1(kGlslTrunc);
    case kDxFrc: return G1(kGlslFract);
    case kDxSqRt: return G1(kGlslSqrt);
    case kDxLog: return G1(kGlslLog2);
    case kDxExp: return G1(kGlslExp2);
    case kDxSinCos: {
      const uint32_t a = SrcF(in, o[2]);
      if (o[0].type != kOtNull && !Store(in, o[0], ToU(Glsl(TF4(), kGlslSin, {a})), true)) return false;
      if (o[1].type != kOtNull && !Store(in, o[1], ToU(Glsl(TF4(), kGlslCos, {a})), true)) return false;
      return true;
    }
    case kDxFToI: {  // D3D: NaN -> 0, saturating
      const uint32_t f = SrcF(in, o[1]);
      const uint32_t cl = Glsl(TF4(), kGlslNClamp, {f, SplatF(-2147483648.0f), SplatF(2147483520.0f)});
      uint32_t r = Op(kOpConvertFToS, TU4(), {cl});
      r = Op(kOpSelect, TU4(),
             {Op(kOpFOrdGreaterThanEqual, TB4(), {f, SplatF(2147483648.0f)}), SplatU(0x7FFFFFFFu), r});
      r = Op(kOpSelect, TU4(), {Op(kOpIsNan, TB4(), {f}), SplatU(0), r});
      return Store(in, o[0], r, false);
    }
    case kDxFToU: {  // D3D: NaN / negative -> 0, saturating
      const uint32_t f = SrcF(in, o[1]);
      const uint32_t cl = Glsl(TF4(), kGlslNClamp, {f, SplatF(0.0f), SplatF(4294967040.0f)});
      uint32_t r = Op(kOpConvertFToU, TU4(), {cl});
      r = Op(kOpSelect, TU4(),
             {Op(kOpFOrdGreaterThanEqual, TB4(), {f, SplatF(4294967296.0f)}), SplatU(0xFFFFFFFFu), r});
      return Store(in, o[0], r, false);
    }
    case kDxUToF: return Store(in, o[0], ToU(Op(kOpConvertUToF, TF4(), {SrcU(in, o[1])})), true);
    case kDxIToF: return Store(in, o[0], ToU(Op(kOpConvertSToF, TF4(), {SrcU(in, o[1])})), true);
    case kDxF32ToF16: {
      const uint32_t f = SrcF(in, o[1]);
      uint32_t lanes[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t x = Op(kOpCompositeExtract, TF(), {f, c});
        const uint32_t v2 = Op(kOpCompositeConstruct, TVec(TF(), 2), {x, CF(0.0f)});
        lanes[c] = Glsl(TU(), kGlslPackHalf2x16, {v2});
      }
      return Store(in, o[0], Op(kOpCompositeConstruct, TU4(), {lanes[0], lanes[1], lanes[2], lanes[3]}), false);
    }

    // ---- integer ----
    case kDxIAdd: return I2(kOpIAdd);
    case kDxAnd: return I2(kOpBitwiseAnd);
    case kDxOr: return I2(kOpBitwiseOr);
    case kDxXOr: return I2(kOpBitwiseXor);
    case kDxNot: return Store(in, o[0], Op(kOpNot, TU4(), {SrcU(in, o[1])}), false);
    case kDxIEq: return ICmp(kOpIEqual);
    case kDxINE: return ICmp(kOpINotEqual);
    case kDxILT: return ICmp(kOpSLessThan);
    case kDxIGE: return ICmp(kOpSGreaterThanEqual);
    case kDxULT: return ICmp(kOpULessThan);
    case kDxUGE: return ICmp(kOpUGreaterThanEqual);
    case kDxIMax: return Store(in, o[0], Glsl(TU4(), kGlslSMax, {SrcU(in, o[1]), SrcU(in, o[2])}), false);
    case kDxIMin: return Store(in, o[0], Glsl(TU4(), kGlslSMin, {SrcU(in, o[1]), SrcU(in, o[2])}), false);
    case kDxUMax: return Store(in, o[0], Glsl(TU4(), kGlslUMax, {SrcU(in, o[1]), SrcU(in, o[2])}), false);
    case kDxUMin: return Store(in, o[0], Glsl(TU4(), kGlslUMin, {SrcU(in, o[1]), SrcU(in, o[2])}), false);
    case kDxIShL: return Shift(kOpShiftLeftLogical);
    case kDxUShR: return Shift(kOpShiftRightLogical);
    case kDxIShR: return Shift(kOpShiftRightArithmetic);
    case kDxIMAd: {
      const uint32_t m = Op(kOpIMul, TU4(), {SrcU(in, o[1]), SrcU(in, o[2])});
      return Store(in, o[0], Op(kOpIAdd, TU4(), {m, SrcU(in, o[3])}), false);
    }
    case kDxUDiv: {  // D3D: x / 0 -> 0xFFFFFFFF for both results
      const uint32_t a = SrcU(in, o[2]), b = SrcU(in, o[3]);
      const uint32_t zero = Op(kOpIEqual, TB4(), {b, SplatU(0)});
      const uint32_t safe = Op(kOpSelect, TU4(), {zero, SplatU(1), b});
      if (o[0].type != kOtNull) {
        const uint32_t q = Op(kOpSelect, TU4(), {zero, SplatU(~0u), Op(kOpUDiv, TU4(), {a, safe})});
        if (!Store(in, o[0], q, false)) return false;
      }
      if (o[1].type != kOtNull) {
        const uint32_t r = Op(kOpSelect, TU4(), {zero, SplatU(~0u), Op(kOpUMod, TU4(), {a, safe})});
        if (!Store(in, o[1], r, false)) return false;
      }
      return true;
    }
    case kDxUBFE:
    case kDxIBFE: {  // D3D: width / offset & 31; width 0 -> 0; past bit 31 -> plain shift
      const uint32_t w = Op(kOpBitwiseAnd, TU4(), {SrcU(in, o[1]), SplatU(31)});
      const uint32_t off = Op(kOpBitwiseAnd, TU4(), {SrcU(in, o[2]), SplatU(31)});
      const uint32_t v = SrcU(in, o[3]);
      const uint32_t sr = in.op == kDxIBFE ? kOpShiftRightArithmetic : kOpShiftRightLogical;
      const uint32_t s = Op(kOpIAdd, TU4(), {w, off});
      const uint32_t fits = Op(kOpULessThan, TB4(), {s, SplatU(32)});
      const uint32_t l = Op(kOpBitwiseAnd, TU4(), {Op(kOpISub, TU4(), {SplatU(32), s}), SplatU(31)});
      const uint32_t rr = Op(kOpBitwiseAnd, TU4(), {Op(kOpISub, TU4(), {SplatU(32), w}), SplatU(31)});
      const uint32_t inside = Op(sr, TU4(), {Op(kOpShiftLeftLogical, TU4(), {v, l}), rr});
      const uint32_t past = Op(sr, TU4(), {v, off});
      uint32_t r = Op(kOpSelect, TU4(), {fits, inside, past});
      r = Op(kOpSelect, TU4(), {Op(kOpIEqual, TB4(), {w, SplatU(0)}), SplatU(0), r});
      return Store(in, o[0], r, false);
    }

    // ---- textures ----
    case kDxSample:
    case kDxSampleL:
    case kDxSampleB:
    case kDxSampleD: {
      Resource* r = nullptr;
      const uint32_t si = Sampled(o[2], o[3], r);
      if (!r) return false;
      if (in.op == kDxSample && !pixel_) return Fail("implicit-LOD sample in a vertex shader");
      const uint32_t c = Coords(in, o[1], *r);
      std::vector<uint32_t> w{si, c};
      uint32_t mask = 0;
      std::vector<uint32_t> extra;
      if (in.op == kDxSampleB) {
        mask |= 1;
        extra.push_back(Op(kOpCompositeExtract, TF(), {SrcF(in, o[4]), 0}));
      } else if (in.op == kDxSampleL) {
        mask |= 2;
        extra.push_back(Op(kOpCompositeExtract, TF(), {SrcF(in, o[4]), 0}));
      } else if (in.op == kDxSampleD) {
        mask |= 4;
        Resource rr = *r;
        extra.push_back(Coords(in, o[4], rr));
        extra.push_back(Coords(in, o[5], rr));
      }
      if (in.off[0] || in.off[1] || in.off[2]) {
        if (r->dim == 6) return Fail("offset on a cube texture");
        mask |= 8;
        if (r->dim == 3)
          extra.push_back(m_.Cached(kOpConstantComposite, {TVec(TI(), 2), CI(in.off[0]), CI(in.off[1])}, true));
        else
          extra.push_back(m_.Cached(kOpConstantComposite,
                                    {TVec(TI(), 3), CI(in.off[0]), CI(in.off[1]), CI(in.off[2])}, true));
      }
      if (mask) {
        w.push_back(mask);
        w.insert(w.end(), extra.begin(), extra.end());
      }
      const uint32_t res = OpV(in.op == kDxSample || in.op == kDxSampleB ? kOpImageSampleImplicitLod
                                                                         : kOpImageSampleExplicitLod,
                               TF4(), w);
      return Store(in, o[0], ApplySwizzle(ToU(res), o[2]), true);
    }
    case kDxLOD: {
      if (!pixel_) return Fail("lod in a vertex shader");
      Resource* r = nullptr;
      const uint32_t si = Sampled(o[2], o[3], r);
      if (!r) return false;
      const uint32_t q = Op(kOpImageQueryLod, TVec(TF(), 2), {si, Coords(in, o[1], *r)});
      // D3D: x clamped, y unclamped (z, w the same as y).
      const uint32_t v = Shuffle(TF4(), q, q, {0, 1, 1, 1});
      return Store(in, o[0], ApplySwizzle(ToU(v), o[2]), true);
    }
    case kDxResInfo: {
      auto ti = textures_.find(o[2].idx[0].imm);
      if (ti == textures_.end()) return Fail("resinfo resource");
      const Resource& r = ti->second;
      const uint32_t img = Op(kOpLoad, r.image_type, {r.var});
      const uint32_t mip = X(SrcU(in, o[1]));
      const uint32_t n = r.dim == 5 ? 3 : 2;
      const uint32_t sz = Op(kOpImageQuerySizeLod, TVec(TU(), n), {img, mip});
      const uint32_t levels = Op(kOpImageQueryLevels, TU(), {img});
      const uint32_t x = Op(kOpCompositeExtract, TU(), {sz, 0}), y = Op(kOpCompositeExtract, TU(), {sz, 1});
      const uint32_t z = n == 3 ? Op(kOpCompositeExtract, TU(), {sz, 2}) : CU(r.dim == 6 ? 6 : 0);
      uint32_t v = Op(kOpCompositeConstruct, TU4(), {x, y, z, levels});
      const uint32_t rt = (in.token >> 11) & 3;
      if (rt == 2) return Store(in, o[0], ApplySwizzle(v, o[2]), false);  // uint
      uint32_t f = Op(kOpConvertUToF, TF4(), {v});
      if (rt == 1) {  // rcpfloat: 1 / size, levels as float
        const uint32_t rcp = Precise(Op(kOpFDiv, TF4(), {SplatF(1.0f), f}));
        f = Shuffle(TF4(), rcp, f, {0, 1, 2, 7});
      }
      return Store(in, o[0], ApplySwizzle(ToU(f), o[2]), true);
    }

    // ---- raw buffers ----
    case kDxLdRaw: {
      auto bi = raw_srvs_.find(o[2].idx[0].imm);
      uint32_t var = 0;
      if (o[2].type == kOtResource && bi != raw_srvs_.end()) var = bi->second;
      else if (o[2].type == kOtUav && uavs_.count(o[2].idx[0].imm)) var = uavs_[o[2].idx[0].imm];
      if (!var) return Fail("ld_raw buffer");
      const uint32_t word = Op(kOpShiftRightLogical, TU(), {X(SrcU(in, o[1])), CU(2)});
      const uint32_t mask = o[0].ncomp == 1 ? 1 : o[0].mask;
      uint32_t lanes[4];
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c))) {
          lanes[c] = CU(0);
          continue;
        }
        const uint32_t idx = Op(kOpIAdd, TU(), {word, CU(o[2].sw[c])});
        const uint32_t p = Op(kOpAccessChain, TPtr(kScStorageBuffer, TU()), {var, CU(0), idx});
        lanes[c] = Op(kOpLoad, TU(), {p});
      }
      // The resource swizzle is already applied per lane; store without swizzling again.
      return Store(in, o[0], Op(kOpCompositeConstruct, TU4(), {lanes[0], lanes[1], lanes[2], lanes[3]}), false);
    }
    case kDxStoreRaw: {
      auto ui = uavs_.find(o[0].idx[0].imm);
      if (o[0].type != kOtUav || ui == uavs_.end()) return Fail("store_raw target");
      const uint32_t word = Op(kOpShiftRightLogical, TU(), {X(SrcU(in, o[1])), CU(2)});
      const uint32_t v = SrcU(in, o[2]);
      const uint32_t mask = o[0].ncomp == 1 ? 1 : o[0].mask;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c))) continue;
        const uint32_t idx = c ? Op(kOpIAdd, TU(), {word, CU(c)}) : word;
        const uint32_t p = Op(kOpAccessChain, TPtr(kScStorageBuffer, TU()), {ui->second, CU(0), idx});
        Void(kOpStore, {p, Op(kOpCompositeExtract, TU(), {v, c})});
      }
      return true;
    }
    case kDxAtomicAnd:
    case kDxAtomicOr: {
      auto ui = uavs_.find(o[0].idx[0].imm);
      if (o[0].type != kOtUav || ui == uavs_.end()) return Fail("atomic target");
      const uint32_t word = Op(kOpShiftRightLogical, TU(), {X(SrcU(in, o[1])), CU(2)});
      const uint32_t p = Op(kOpAccessChain, TPtr(kScStorageBuffer, TU()), {ui->second, CU(0), word});
      Op(in.op == kDxAtomicAnd ? kOpAtomicAnd : kOpAtomicOr, TU(), {p, CU(1), CU(0), X(SrcU(in, o[2]))});
      return true;
    }
    default:
      return Fail(std::format("instruction {}", in.op));
  }
}

SpirvResult Converter::Run(const uint32_t* shex, size_t n) {
  SpirvResult out;
  if (n < 2) { out.error = "no shader code"; return out; }
  const uint32_t type = shex[0] >> 16;
  if (type != 0 && type != 1) { out.error = std::format("program type {} (vertex / pixel only)", type); return out; }
  pixel_ = type == 0;
  out.pixel = pixel_;
  const size_t len = std::min<size_t>(shex[1], n);
  // ---- parse ----
  std::vector<Instr> code;
  Parser ps(shex, len);
  for (size_t i = 2; i < len;) {
    Instr in;
    in.token = shex[i];
    in.op = in.token & 0x7FF;
    uint32_t ilen = (in.token >> 24) & 0x7F;
    if (in.op == kDxCustomData) {
      if (i + 1 >= len) { out.error = "custom data"; return out; }
      ilen = shex[i + 1];
      i += ilen;
      continue;
    }
    if (!ilen || i + ilen > len) { out.error = std::format("instruction length at {}", i); return out; }
    const size_t end = i + ilen;
    size_t j = i + 1;
    bool ext = in.token >> 31;
    while (ext && j < end) {
      const uint32_t e = shex[j++];
      if ((e & 0x3F) == 1) {  // sample controls: immediate offsets
        auto s4 = [](uint32_t v) { return int32_t(v << 28) >> 28; };
        in.off[0] = s4((e >> 9) & 0xF);
        in.off[1] = s4((e >> 13) & 0xF);
        in.off[2] = s4((e >> 17) & 0xF);
      }
      ext = e >> 31;
    }
    const int cnt = OperandCount(in.op);
    if (cnt < 0) { out.error = std::format("instruction {}", in.op); return out; }
    std::string err;
    for (int k = 0; k < cnt; ++k) {
      Operand o;
      if (!ps.ReadOperand(j, in, o, err)) { out.error = err; return out; }
      in.ops.push_back(o);
    }
    while (j < end) in.extra.push_back(shex[j++]);
    code.push_back(std::move(in));
    i = end;
  }
  // ---- switch pre-scan ----
  {
    std::vector<size_t> stack;
    bool prev_label = false;
    for (size_t k = 0; k < code.size(); ++k) {
      const uint32_t op = code[k].op;
      if (op == kDxSwitch) {
        stack.push_back(k);
        switches_[k] = {};
        prev_label = false;
        continue;
      }
      if (op == kDxEndSwitch) {
        if (!stack.empty()) stack.pop_back();
        prev_label = false;
        continue;
      }
      if ((op == kDxCase || op == kDxDefault) && !stack.empty()) {
        SwitchInfo& s = switches_[stack.back()];
        if (!prev_label) ++s.groups;
        const uint32_t g = s.groups - 1;
        case_group_[k] = g;
        if (op == kDxDefault) s.default_group = int(g);
        else s.cases.push_back({code[k].ops[0].imm[0], g});
        prev_label = true;
        continue;
      }
      prev_label = false;
    }
  }
  // ---- module ----
  glsl_ = m_.Id();
  main_ = m_.Id();
  auto is_decl = [](uint32_t op) {
    return (op >= kDxDclResource && op <= kDxDclGlobalFlags) || op == kDxDclResourceRaw || op == kDxDclUavRaw;
  };
  for (const Instr& in : code)
    if (is_decl(in.op) && !Declare(in)) break;
  if (!error_.empty()) { out.error = error_; return out; }
  // Interface arrays (the HLSL path's float4 interp[16] : TEXCOORD0).
  const uint32_t arr16 = m_.Cached(kOpTypeArray, {TF4(), CU(16)});
  if (pixel_) {
    {
      bool any = false;
      for (auto& [r, k] : in_kind_) any = any || k == 1;
      if (any) {
        interp_in_ = m_.Id();
        Module::Put(m_.globals, kOpVariable, {TPtr(kScInput, arr16), interp_in_, kScInput});
        Decorate(interp_in_, kDecLocation, {0});
        // dcl_input_ps modes: 2 linear, 3 centroid, 4 noperspective, 5 noperspective centroid, 1 constant.
        if (interp_mode_ == 1) Decorate(interp_in_, kDecFlat);
        if (interp_mode_ == 3 || interp_mode_ == 5) Decorate(interp_in_, kDecCentroid);
        if (interp_mode_ == 4 || interp_mode_ == 5) Decorate(interp_in_, kDecNoPerspective);
        if (interp_mode_ > 5) { out.error = std::format("interpolation mode {}", interp_mode_); return out; }
        m_.entry_ifaces.push_back(interp_in_);
      }
    }
  } else {
    interp_out_ = m_.Id();
    Module::Put(m_.globals, kOpVariable, {TPtr(kScOutput, arr16), interp_out_, kScOutput});
    Decorate(interp_out_, kDecLocation, {0});
    m_.entry_ifaces.push_back(interp_out_);
  }
  // main
  const uint32_t fn_type = m_.Cached(kOpTypeFunction, {TVoid()});
  Module::Put(m_.fn, kOpFunction, {TVoid(), main_, 0, fn_type});
  Label(m_.Id());
  for (uint32_t v : temps_) Module::Put(m_.fn, kOpVariable, {TPtr(kScFunction, TU4()), v, kScFunction});
  for (auto& [x, vs] : xtemps_) Module::Put(m_.fn, kOpVariable, {TPtr(kScFunction, vs.second), vs.first, kScFunction});
  // D3D temps start at 0.
  for (uint32_t v : temps_) Void(kOpStore, {v, SplatU(0)});
  for (size_t k = 0; k < code.size(); ++k) {
    const Instr& in = code[k];
    if (is_decl(in.op)) continue;
    if (!Emit(k, in)) break;
  }
  if (!error_.empty()) { out.error = error_; return out; }
  if (!cf_.empty()) { out.error = "unclosed control flow"; return out; }
  if (open_) Terminate(kOpReturn, {});
  Module::Put(m_.fn, kOpFunctionEnd, {});
  // ---- assemble ----
  std::vector<uint32_t>& w = out.words;
  w = {0x07230203u, 0x00010600u, 0x00470000u /* generator: ours */, m_.next_, 0};
  auto cap = [&](uint32_t c) { Module::Put(w, kOpCapability, {c}); };
  cap(1);     // Shader
  cap(50);    // ImageQuery
  const bool fc = g_float_controls.load(std::memory_order_relaxed);  // patch 0103
  if (fc) cap(4466);  // SignedZeroInfNanPreserve (the HLSL path's -Gis)
  if (pixel_) cap(5379);  // DemoteToHelperInvocation
  {
    const char name[] = "GLSL.std.450";
    std::vector<uint32_t> s{glsl_};
    uint32_t words[5] = {};
    std::memcpy(words, name, sizeof(name));
    s.insert(s.end(), words, words + 4);
    Module::PutV(w, kOpExtInstImport, s);
  }
  Module::Put(w, kOpMemoryModel, {0, 1});
  {
    std::vector<uint32_t> e{pixel_ ? 4u : 0u, main_, 0x6E69616Du /* "main" */, 0};
    e.insert(e.end(), m_.entry_ifaces.begin(), m_.entry_ifaces.end());
    Module::PutV(w, kOpEntryPoint, e);
  }
  if (pixel_) Module::Put(w, kOpExecutionMode, {main_, 7});  // OriginUpperLeft
  if (depth_out_) Module::Put(w, kOpExecutionMode, {main_, 12});  // DepthReplacing
  if (pixel_) g_ps.fetch_add(1, std::memory_order_relaxed);  // patch 0102
  if (fc) Module::Put(w, kOpExecutionMode, {main_, 4461, 32});  // SignedZeroInfNanPreserve 32
  w.insert(w.end(), m_.annot.begin(), m_.annot.end());
  w.insert(w.end(), m_.types.begin(), m_.types.end());
  w.insert(w.end(), m_.globals.begin(), m_.globals.end());
  w.insert(w.end(), m_.fn.begin(), m_.fn.end());
  w[3] = m_.next_;
  out.ok = true;
  return out;
}

}  // namespace

SpirvResult ConvertDxbcToSpirv(const uint8_t* dxbc, size_t size, bool precise) {
  SpirvResult fail;
  if (size < 32 || std::memcmp(dxbc, "DXBC", 4) != 0) {
    fail.error = "not a DXBC container";
    return fail;
  }
  uint32_t chunks = 0;
  std::memcpy(&chunks, dxbc + 28, 4);
  for (uint32_t c = 0; c < chunks && 32 + 4 * c + 4 <= size; ++c) {
    uint32_t off = 0;
    std::memcpy(&off, dxbc + 32 + 4 * c, 4);
    if (off + 8 > size) break;
    if (std::memcmp(dxbc + off, "SHEX", 4) == 0 || std::memcmp(dxbc + off, "SHDR", 4) == 0) {
      uint32_t bytes = 0;
      std::memcpy(&bytes, dxbc + off + 4, 4);
      if (off + 8 + bytes > size) break;
      std::vector<uint32_t> code(bytes / 4);
      std::memcpy(code.data(), dxbc + off + 8, code.size() * 4);
      Converter conv;
      conv.precise_ = precise;
      return conv.Run(code.data(), code.size());
    }
  }
  fail.error = "no SHEX chunk";
  return fail;
}

}  // namespace xenos::dxbc_spirv
