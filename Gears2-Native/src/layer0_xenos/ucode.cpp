// Layer 0 (game-free): Xenos shader microcode decoder and disassembler (Stage 3 step S3.2, notes/36).
//
// Adapted from Xenia (Copyright Ben Vanik and Xenia project contributors) via rexglue, BSD-3-Clause: the bit layouts
// come from rexglue include/rex/graphics/format/ucode.h, the opcode tables from src/graphics/format/ucode.cpp, the
// parsing rules from src/graphics/pipeline/shader/translator.cpp (Shader::AnalyzeUcode, Parse*) and the text format from
// translator_disasm.cpp. Rewritten here with explicit shifts instead of bitfield structs so the layout is visible and
// does not depend on the compiler's bitfield order. Our code is MIT. The goal of the text is byte-for-byte equality
// with the SDK's Shader::ucode_disassembly(), which is how this decoder is verified against the game's whole corpus
// (tools/gn_disasm_check.cpp) - so even the SDK's quirks are reproduced and marked as such.
#include <xenos/ucode.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace xenos::ucode {

namespace {

constexpr uint32_t Bits(uint32_t v, unsigned shift, unsigned count) { return (v >> shift) & ((1u << count) - 1); }
constexpr int32_t SignedBits(uint32_t v, unsigned shift, unsigned count) {
  return int32_t(v << (32 - shift - count)) >> (32 - count);
}

struct ScalarInfo {
  const char* name;
  uint32_t operand_count;
  bool single_operand_is_two_component;
};
constexpr ScalarInfo kScalarInfos[64] = {
    {"adds", 1, true},       {"adds_prev", 1, false}, {"muls", 1, true},      {"muls_prev", 1, false},
    {"muls_prev2", 1, true}, {"maxs", 1, true},       {"mins", 1, true},      {"seqs", 1, false},
    {"sgts", 1, false},      {"sges", 1, false},      {"snes", 1, false},     {"frcs", 1, false},
    {"truncs", 1, false},    {"floors", 1, false},    {"exp", 1, false},      {"logc", 1, false},
    {"log", 1, false},       {"rcpc", 1, false},      {"rcpf", 1, false},     {"rcp", 1, false},
    {"rsqc", 1, false},      {"rsqf", 1, false},      {"rsq", 1, false},      {"maxas", 1, true},
    {"maxasf", 1, true},     {"subs", 1, true},       {"subs_prev", 1, false}, {"setp_eq", 1, false},
    {"setp_ne", 1, false},   {"setp_gt", 1, false},   {"setp_ge", 1, false},  {"setp_inv", 1, false},
    {"setp_pop", 1, false},  {"setp_clr", 0, false},  {"setp_rstr", 1, false}, {"kills_eq", 1, false},
    {"kills_gt", 1, false},  {"kills_ge", 1, false},  {"kills_ne", 1, false}, {"kills_one", 1, false},
    {"sqrt", 1, false},      {"opcode_41", 0, false}, {"mulsc", 2, false},    {"mulsc", 2, false},
    {"addsc", 2, false},     {"addsc", 2, false},     {"subsc", 2, false},    {"subsc", 2, false},
    {"sin", 1, false},       {"cos", 1, false},       {"retain_prev", 0, false}, {"opcode_51", 0, false},
    {"opcode_52", 0, false}, {"opcode_53", 0, false}, {"opcode_54", 0, false}, {"opcode_55", 0, false},
    {"opcode_56", 0, false}, {"opcode_57", 0, false}, {"opcode_58", 0, false}, {"opcode_59", 0, false},
    {"opcode_60", 0, false}, {"opcode_61", 0, false}, {"opcode_62", 0, false}, {"opcode_63", 0, false},
};

struct VectorInfo {
  const char* name;
  uint32_t operand_count;
};
constexpr VectorInfo kVectorInfos[32] = {
    {"add", 2},          {"mul", 2},          {"max", 2},          {"min", 2},          {"seq", 2},
    {"sgt", 2},          {"sge", 2},          {"sne", 2},          {"frc", 1},          {"trunc", 1},
    {"floor", 1},        {"mad", 3},          {"cndeq", 3},        {"cndge", 3},        {"cndgt", 3},
    {"dp4", 2},          {"dp3", 2},          {"dp2add", 3},       {"cube", 2},         {"max4", 1},
    {"setp_eq_push", 2}, {"setp_ne_push", 2}, {"setp_gt_push", 2}, {"setp_ge_push", 2}, {"kill_eq", 2},
    {"kill_gt", 2},      {"kill_ge", 2},      {"kill_ne", 2},      {"dst", 2},          {"maxa", 2},
    {"opcode_30", 0},    {"opcode_31", 0},
};

// Component i of a 2-bit-per-component ALU source swizzle (stored relative to the identity).
Swizzle AluSwizzle(uint32_t swizzle, uint32_t i) { return Swizzle(((swizzle >> (2 * i)) + i) & 3); }

bool IsExecOpcode(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }

// The two 48-bit control flow instructions packed in 3 dwords: returns {low dword, high 16 bits}.
void UnpackCfPair(const uint32_t* d, uint32_t out[2][2]) {
  out[0][0] = d[0];
  out[0][1] = d[1] & 0xFFFF;
  out[1][0] = (d[1] >> 16) | (d[2] << 16);
  out[1][1] = d[2] >> 16;
}

// Where decode problems are recorded.
struct Ctx {
  Program& program;
  const DecodeOptions& options;
  uint32_t address = 0;
  void Flag(Issue::Kind kind, std::string detail) { program.issues.push_back({kind, address, std::move(detail)}); }
};

// Fetch destination: 3 bits per component (x y z w 0 1 _ keep).
Result FetchResult(uint32_t dest, uint32_t swizzle, bool relative, Ctx& ctx) {
  Result r;
  r.target = Target::kRegister;
  r.index = dest;
  r.addressing = relative ? Addressing::kLoopRelative : Addressing::kAbsolute;
  r.write_mask = 0b1111;
  for (int i = 0; i < 4; ++i) {
    const uint32_t s = (swizzle >> (3 * i)) & 7;
    Swizzle c = Swizzle::k0;
    if (s <= 3) c = Swizzle(s);
    else if (s == 5) c = Swizzle::k1;
    else if (s == 7) r.write_mask &= ~(1u << i);  // keep: component not written
    else if (s == 6) ctx.Flag(Issue::Kind::kInvalidFetchSwizzle, "fetch destination swizzle 6");
    // 4 = constant 0; 6 is invalid and treated as 0 like the SDK.
    r.components[i] = c;
  }
  return r;
}

const char* VertexFormatName(uint32_t f);

struct VFetchWords {
  uint32_t w[3] = {};
};

VertexFetch DecodeVertexFetch(const uint32_t* w, const VFetchWords& previous_full, bool have_full, Ctx& ctx) {
  VertexFetch f;
  f.mini = Bits(w[1], 30, 1);
  f.predicated = Bits(w[1], 31, 1);
  f.predicate_condition = Bits(w[2], 31, 1);
  f.result = FetchResult(Bits(w[0], 12, 6), Bits(w[1], 0, 12), Bits(w[0], 18, 1), ctx);
  if (f.mini && !have_full) ctx.Flag(Issue::Kind::kMiniFetchWithoutFull, "vfetch_mini before any vfetch_full");
  // A mini fetch reuses the address register, fetch constant, stride and index rounding of the last full fetch (all
  // zero before the first one, as in the SDK).
  const uint32_t* full = f.mini ? previous_full.w : w;
  f.address.source = Source::kRegister;
  f.address.index = Bits(full[0], 5, 6);
  f.address.addressing = Bits(full[0], 11, 1) ? Addressing::kLoopRelative : Addressing::kAbsolute;
  f.address.component_count = 1;
  f.address.components[0] = Swizzle(Bits(full[0], 30, 2));
  f.fetch_constant = Bits(full[0], 20, 5) * 3 + Bits(full[0], 25, 2);
  f.stride = Bits(full[2], 0, 8);
  f.index_rounded = Bits(full[1], 15, 1);
  f.data_format = Bits(w[1], 16, 6);
  f.offset = SignedBits(w[2], 8, 23);
  f.exp_adjust = SignedBits(w[1], 24, 6);
  f.prefetch_count = Bits(w[0], 27, 3);
  f.is_signed = Bits(w[1], 12, 1);
  f.is_integer = Bits(w[1], 13, 1);
  f.signed_rf_mode = Bits(w[1], 14, 1);
  if (f.data_format && !VertexFormatName(f.data_format)) {
    ctx.Flag(Issue::Kind::kUnknownVertexFormat, std::format("vertex format {}", f.data_format));
  }
  return f;
}

TextureFetch DecodeTextureFetch(const uint32_t* w, Ctx& ctx) {
  TextureFetch f;
  f.opcode = FetchOpcode(Bits(w[0], 0, 5));
  f.dimension = FetchDimension(Bits(w[2], 14, 2));
  f.predicated = Bits(w[1], 31, 1);
  f.predicate_condition = Bits(w[2], 31, 1);
  uint32_t override_components = 0;
  switch (f.opcode) {
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureBorderColorFrac:
    case FetchOpcode::kGetTextureComputedLod:
    case FetchOpcode::kGetTextureWeights:
      f.has_result = f.has_fetch_constant = f.has_attributes = true;
      break;
    case FetchOpcode::kGetTextureGradients:
      f.has_result = f.has_fetch_constant = f.has_attributes = true;
      override_components = 2;
      break;
    case FetchOpcode::kSetTextureLod:
      override_components = 1;
      break;
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert:
      override_components = 3;
      break;
    default:
      ctx.Flag(Issue::Kind::kUnknownFetchOpcode, std::format("fetch opcode {}", uint32_t(f.opcode)));
      return f;  // left empty (the SDK asserts here)
  }
  if (f.has_result) f.result = FetchResult(Bits(w[0], 12, 6), Bits(w[1], 0, 12), Bits(w[0], 18, 1), ctx);
  f.coordinates.source = Source::kRegister;
  f.coordinates.index = Bits(w[0], 5, 6);
  f.coordinates.addressing = Bits(w[0], 11, 1) ? Addressing::kLoopRelative : Addressing::kAbsolute;
  f.coordinates.component_count = override_components ? override_components : FetchDimensionComponentCount(f.dimension);
  uint32_t swizzle = Bits(w[0], 26, 6);
  if (f.opcode == FetchOpcode::kTextureFetch && f.dimension == FetchDimension::k1D) {
    // Canary (545407D4's UI shader) treats a 1D fetch whose swizzle is not X-replicated as reading XY; rexglue reads X.
    const uint32_t x = swizzle & 3;
    if (((swizzle >> 2) & 3) != x || ((swizzle >> 4) & 3) != x) {
      ctx.Flag(Issue::Kind::kCanaryTfetch1DTwoComponents, "tfetch1D with a non-replicated swizzle");
      if (ctx.options.canary_tfetch1d_two_components) f.coordinates.component_count = 2;
    }
  }
  for (uint32_t j = 0; j < f.coordinates.component_count; ++j, swizzle >>= 2) f.coordinates.components[j] = Swizzle(swizzle & 3);
  if (f.has_fetch_constant) f.fetch_constant = Bits(w[0], 20, 5);
  if (f.has_attributes) {
    f.fetch_valid_only = Bits(w[0], 19, 1);
    f.unnormalized_coordinates = Bits(w[0], 25, 1);
    f.mag_filter = TextureFilter(Bits(w[1], 12, 2));
    f.min_filter = TextureFilter(Bits(w[1], 14, 2));
    f.mip_filter = TextureFilter(Bits(w[1], 16, 2));
    f.aniso_filter = AnisoFilter(Bits(w[1], 18, 3));
    f.vol_mag_filter = TextureFilter(Bits(w[1], 24, 2));
    f.vol_min_filter = TextureFilter(Bits(w[1], 26, 2));
    f.use_computed_lod = Bits(w[1], 28, 1);
    f.use_register_lod = Bits(w[1], 29, 1);
    f.use_register_gradients = Bits(w[2], 0, 1);
    f.lod_bias = float(SignedBits(w[2], 2, 7)) * (1.0f / 16.0f);
    f.offset_x = float(SignedBits(w[2], 16, 5)) * 0.5f;
    f.offset_y = float(SignedBits(w[2], 21, 5)) * 0.5f;
    f.offset_z = float(SignedBits(w[2], 26, 5)) * 0.5f;
  }
  return f;
}

// ALU word fields.
struct AluWords {
  uint32_t w0, w1, w2;
  uint32_t src_reg(uint32_t i) const { return Bits(w2, 24 - 8 * i, 8); }       // i = 1..3: src1 bits 16-23 ...
  bool src_is_temp(uint32_t i) const { return Bits(w2, 32 - i, 1); }           // src1_sel bit 31, src2 30, src3 29
  uint32_t src_swizzle(uint32_t i) const { return Bits(w1, 24 - 8 * i, 8); }   // src1 16-23, src2 8-15, src3 0-7
  bool src_negate(uint32_t i) const { return Bits(w1, 27 - i, 1); }            // src1 26, src2 25, src3 24
  bool abs_constants() const { return Bits(w0, 7, 1); }
  bool const_0_addressed() const { return Bits(w1, 31, 1); }
  bool const_1_addressed() const { return Bits(w1, 30, 1); }
  bool const_address_register_relative() const { return Bits(w1, 29, 1); }
  // Which constant slot (0 or 1) source i would use: constants are numbered in order among the non-temp sources.
  bool src_const_addressed(uint32_t i) const {
    if (i == 1) return const_0_addressed();
    if (i == 2) return src_is_temp(1) ? const_0_addressed() : const_1_addressed();
    return (src_is_temp(1) && src_is_temp(2)) ? const_0_addressed() : const_1_addressed();
  }
};

Operand AluOperand(const AluWords& a, uint32_t i, uint32_t component_count, uint32_t second_component = 0) {
  Operand op;
  op.negate = a.src_negate(i);
  const uint32_t reg = a.src_reg(i);
  if (a.src_is_temp(i)) {
    op.source = Source::kRegister;
    op.index = reg & 0x3F;
    op.absolute = (reg & 0x80) != 0;
    op.addressing = (reg & 0x40) ? Addressing::kLoopRelative : Addressing::kAbsolute;
  } else {
    op.source = Source::kConstantFloat;
    op.index = reg;
    if (a.src_const_addressed(i)) {
      op.addressing = a.const_address_register_relative() ? Addressing::kAddressRegisterRelative : Addressing::kLoopRelative;
    }
    op.absolute = a.abs_constants();
  }
  op.component_count = component_count;
  const uint32_t swizzle = a.src_swizzle(i);
  if (component_count == 1) {
    op.components[0] = AluSwizzle(swizzle, 3);  // scalar `a` reads W
  } else if (component_count == 2) {
    op.components[0] = AluSwizzle(swizzle, 3);  // scalar `a` (W) and `b` (X)
    op.components[1] = AluSwizzle(swizzle, second_component);
  } else {
    for (uint32_t j = 0; j < 4; ++j) op.components[j] = AluSwizzle(swizzle, j);
  }
  return op;
}

Alu DecodeAlu(const uint32_t* w, bool is_vertex_shader, Ctx& ctx) {
  const AluWords a{w[0], w[1], w[2]};
  Alu alu;
  alu.predicated = Bits(a.w1, 28, 1);
  alu.predicate_condition = Bits(a.w1, 27, 1);
  const bool is_export = Bits(a.w0, 15, 1);
  const uint32_t vector_dest = Bits(a.w0, 0, 6), scalar_dest = Bits(a.w0, 8, 6);
  const bool vector_dest_rel = Bits(a.w0, 6, 1), scalar_dest_rel = Bits(a.w0, 14, 1);
  const uint32_t vector_mask = Bits(a.w0, 16, 4), scalar_mask = Bits(a.w0, 20, 4);

  Target target = Target::kRegister;
  uint32_t export_index = 0;
  if (is_export) {
    // Both operations of an export write to the register named by vector_dest.
    target = Target::kNone;
    const uint32_t e = vector_dest;
    if (e == 32) {
      target = Target::kExportAddress;
    } else if (e >= 33 && e <= 37) {
      target = Target::kExportData;
      export_index = e - 33;
    } else if (is_vertex_shader) {
      if (e <= 15) { target = Target::kInterpolator; export_index = e; }
      else if (e == 62) target = Target::kPosition;
      else if (e == 63) target = Target::kPointSizeEdgeFlagKillVertex;
    } else {
      if (e <= 3) { target = Target::kColor; export_index = e; }
      else if (e == 61) target = Target::kDepth;
    }
    if (target == Target::kNone) ctx.Flag(Issue::Kind::kUnknownExportRegister, std::format("export register {}", e));
  }

  // Vector operation (plus the constant 0/1 writes an export can carry in the components neither op writes).
  alu.vector_opcode = AluVectorOpcode(Bits(a.w2, 24, 5));
  Result& vr = alu.vector_result;
  vr.target = target;
  if (is_export) {
    vr.index = export_index;
  } else {
    vr.index = vector_dest;
    if (vector_dest_rel) vr.addressing = Addressing::kLoopRelative;
  }
  vr.clamp = Bits(a.w0, 24, 1);
  const uint32_t const0 = (is_export && scalar_dest_rel) ? (0b1111 & ~(vector_mask | scalar_mask)) : 0;
  const uint32_t const1 = is_export ? (vector_mask & scalar_mask) : 0;
  const uint32_t vector_op_mask = is_export ? (vector_mask & ~scalar_mask) : vector_mask;
  vr.write_mask = vector_op_mask | const0 | const1;
  for (uint32_t i = 0; i < 4; ++i) {
    vr.components[i] = (const0 & (1u << i)) ? Swizzle::k0 : (const1 & (1u << i)) ? Swizzle::k1 : Swizzle(i);
  }
  alu.vector_operand_count = kVectorInfos[uint32_t(alu.vector_opcode)].operand_count;
  if (uint32_t(alu.vector_opcode) >= 30) {
    ctx.Flag(Issue::Kind::kUnknownVectorOpcode, std::format("vector opcode {}", uint32_t(alu.vector_opcode)));
  }
  for (uint32_t i = 0; i < alu.vector_operand_count; ++i) alu.vector_operands[i] = AluOperand(a, i + 1, 4);

  // Scalar operation.
  alu.scalar_opcode = AluScalarOpcode(Bits(a.w0, 26, 6));
  const ScalarInfo& si = kScalarInfos[uint32_t(alu.scalar_opcode)];
  if (uint32_t(alu.scalar_opcode) == 41 || uint32_t(alu.scalar_opcode) >= 51) {
    ctx.Flag(Issue::Kind::kUnknownScalarOpcode, std::format("scalar opcode {}", uint32_t(alu.scalar_opcode)));
  }
  Result& sr = alu.scalar_result;
  sr.target = target;
  if (is_export) {
    sr.index = export_index;
  } else {
    sr.index = scalar_dest;
    if (scalar_dest_rel) sr.addressing = Addressing::kLoopRelative;
  }
  sr.clamp = Bits(a.w0, 25, 1);
  sr.write_mask = is_export ? (scalar_mask & ~vector_mask) : scalar_mask;
  alu.scalar_operand_count = si.operand_count;
  if (si.operand_count == 1) {
    // The scalar op shares source 3 with the vector op. Canary takes the second component of a two-component operand
    // from Z when the vector op also uses source 3 (three operands); rexglue always takes X.
    uint32_t second = 0;
    if (si.single_operand_is_two_component && alu.vector_operand_count == 3) {
      ctx.Flag(Issue::Kind::kCanaryScalarOperandZ, std::format("{} with {}", si.name, kVectorInfos[uint32_t(alu.vector_opcode)].name));
      if (ctx.options.canary_scalar_operand_z) second = 2;
    }
    alu.scalar_operands[0] = AluOperand(a, 3, si.single_operand_is_two_component ? 2 : 1, second);
  } else if (si.operand_count == 2) {
    // mulsc/addsc/subsc: a constant (src3, W of its swizzle) and a temp whose register number is spread over spare bits.
    const bool negate = a.src_negate(3);
    const uint32_t swizzle = a.src_swizzle(3);
    Operand& c = alu.scalar_operands[0];
    c.negate = negate;
    c.absolute = a.abs_constants();
    c.source = Source::kConstantFloat;
    c.index = a.src_reg(3);
    if (a.src_const_addressed(3)) {
      c.addressing = a.const_address_register_relative() ? Addressing::kAddressRegisterRelative : Addressing::kLoopRelative;
    }
    c.component_count = 1;
    c.components[0] = AluSwizzle(swizzle, 3);
    Operand& t = alu.scalar_operands[1];
    t.negate = negate;
    t.absolute = a.abs_constants();
    t.source = Source::kRegister;
    t.index = (uint32_t(alu.scalar_opcode) & 1) | (uint32_t(a.src_is_temp(3)) << 1) | (swizzle & 0x3C);
    t.component_count = 1;
    t.components[0] = AluSwizzle(swizzle, 0);
  }
  return alu;
}

// ---- Text ------------------------------------------------------------------------------------------------------

char SwizzleChar(Swizzle s) { return "xyzw01"[uint32_t(s)]; }

void AppendResult(const Result& r, std::string& out) {
  bool indexed = false;
  switch (r.target) {
    case Target::kRegister: out += 'r'; indexed = true; break;
    case Target::kInterpolator: out += 'o'; indexed = true; break;
    case Target::kPosition: out += "oPos"; break;
    case Target::kPointSizeEdgeFlagKillVertex: out += "oPts"; break;
    case Target::kExportAddress: out += "eA"; break;
    case Target::kExportData: out += "eM"; indexed = true; break;
    case Target::kColor: out += "oC"; indexed = true; break;
    case Target::kDepth: out += "oDepth"; break;
    case Target::kNone: break;
  }
  if (indexed) {
    switch (r.addressing) {
      case Addressing::kAbsolute: out += std::to_string(r.index); break;
      case Addressing::kAddressRegisterRelative: out += std::format("[{}+a0]", r.index); break;
      case Addressing::kLoopRelative: out += std::format("[{}+aL]", r.index); break;
    }
  }
  // The raw microcode mask is printed, including components the target doesn't have.
  if (!r.write_mask) {
    out += "._";
  } else if (r.write_mask != 0b1111 || r.components[0] != Swizzle::kX || r.components[1] != Swizzle::kY ||
             r.components[2] != Swizzle::kZ || r.components[3] != Swizzle::kW) {
    out += '.';
    for (int i = 0; i < 4; ++i) out += (r.write_mask & (1u << i)) ? SwizzleChar(r.components[i]) : '_';
  }
}

void AppendOperand(const Operand& op, std::string& out) {
  if (op.negate) out += '-';
  out += op.source == Source::kConstantFloat ? 'c' : 'r';  // fetch-constant sources are printed by their callers
  if (op.absolute) out += "_abs";
  switch (op.addressing) {
    case Addressing::kAbsolute:
      out += op.absolute ? std::format("[{}]", op.index) : std::to_string(op.index);
      break;
    case Addressing::kAddressRegisterRelative: out += std::format("[{}+a0]", op.index); break;
    case Addressing::kLoopRelative: out += std::format("[{}+aL]", op.index); break;
  }
  if (!op.IsStandardSwizzle()) {
    out += '.';
    for (uint32_t j = 0; j < op.component_count; ++j) out += SwizzleChar(op.components[j]);
  }
}

// "   " plus either "      " or the predicate, as all instruction lines start.
void AppendInstructionPrefix(bool predicated, bool condition, std::string& out) {
  out += "   ";
  out += predicated ? (condition ? " (p0) " : "(!p0) ") : "      ";
}

const char* VertexFormatName(uint32_t f) {
  switch (f) {
    case 6: return "FMT_8_8_8_8";
    case 7: return "FMT_2_10_10_10";
    case 16: return "FMT_10_11_11";
    case 17: return "FMT_11_11_10";
    case 25: return "FMT_16_16";
    case 26: return "FMT_16_16_16_16";
    case 31: return "FMT_16_16_FLOAT";
    case 32: return "FMT_16_16_16_16_FLOAT";
    case 33: return "FMT_32";
    case 34: return "FMT_32_32";
    case 35: return "FMT_32_32_32_32";
    case 36: return "FMT_32_FLOAT";
    case 37: return "FMT_32_32_FLOAT";
    case 38: return "FMT_32_32_32_32_FLOAT";
    case 57: return "FMT_32_32_32_FLOAT";
    default: return nullptr;  // the SDK has no name either (fmt would print a null string); we print the number
  }
}

void AppendVertexFetch(const VertexFetch& f, std::string& out) {
  AppendInstructionPrefix(f.predicated, f.predicate_condition, out);
  out += f.mini ? "vfetch_mini " : "vfetch_full ";
  AppendResult(f.result, out);
  if (!f.mini) {
    out += ", ";
    AppendOperand(f.address, out);
    out += std::format(", vf{}", 95 - f.fetch_constant);  // SDK quirk: printed as 95 - index
    if (f.index_rounded) out += ", RoundIndex=true";
  }
  if (f.exp_adjust) out += std::format(", ExpAdjust={}", f.exp_adjust);
  if (f.offset) out += std::format(", Offset={}", f.offset);
  if (f.data_format) {
    const char* name = VertexFormatName(f.data_format);
    out += name ? std::format(", DataFormat={}", name) : std::format(", DataFormat={}", f.data_format);
  }
  if (!f.mini && f.stride) out += std::format(", Stride={}", f.stride);
  if (f.is_signed) out += ", Signed=true";
  if (f.is_integer) out += ", NumFormat=integer";
  if (f.prefetch_count) out += std::format(", PrefetchCount={}", f.prefetch_count + 1);
  out += '\n';
}

const char* TextureFetchName(const TextureFetch& f) {
  static const char* kFetch[] = {"tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"};
  static const char* kBcf[] = {"getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"};
  static const char* kLod[] = {"getCompTexLOD1D", "getCompTexLOD2D", "getCompTexLOD3D", "getCompTexLODCube"};
  static const char* kWeights[] = {"getWeights1D", "getWeights2D", "getWeights3D", "getWeightsCube"};
  const uint32_t d = uint32_t(f.dimension);
  switch (f.opcode) {
    case FetchOpcode::kTextureFetch: return kFetch[d];
    case FetchOpcode::kGetTextureBorderColorFrac: return kBcf[d];
    case FetchOpcode::kGetTextureComputedLod: return kLod[d];
    case FetchOpcode::kGetTextureGradients: return "getGradients";
    case FetchOpcode::kGetTextureWeights: return kWeights[d];
    case FetchOpcode::kSetTextureLod: return "setTexLOD";
    case FetchOpcode::kSetTextureGradientsHorz: return "setGradientH";
    case FetchOpcode::kSetTextureGradientsVert: return "setGradientV";
    default: return "";
  }
}

void AppendTextureFetch(const TextureFetch& f, std::string& out) {
  static const char* kFilters[] = {"point", "linear", "basemap", "keep"};
  static const char* kAniso[] = {"disabled", "max1to1", "max2to1", "max4to1", "max8to1", "max16to1", "keep"};
  AppendInstructionPrefix(f.predicated, f.predicate_condition, out);
  out += TextureFetchName(f);
  out += ' ';
  if (f.has_result) {
    AppendResult(f.result, out);
    out += ", ";
  }
  AppendOperand(f.coordinates, out);
  if (f.has_fetch_constant) out += std::format(", tf{}", f.fetch_constant);
  if (!f.fetch_valid_only) out += ", FetchValidOnly=false";
  if (f.unnormalized_coordinates) out += ", UnnormalizedTextureCoords=true";
  if (f.mag_filter != TextureFilter::kUseFetchConst) out += std::format(", MagFilter={}", kFilters[uint32_t(f.mag_filter)]);
  if (f.min_filter != TextureFilter::kUseFetchConst) out += std::format(", MinFilter={}", kFilters[uint32_t(f.min_filter)]);
  if (f.mip_filter != TextureFilter::kUseFetchConst) out += std::format(", MipFilter={}", kFilters[uint32_t(f.mip_filter)]);
  if (f.aniso_filter != AnisoFilter::kUseFetchConst) out += std::format(", AnisoFilter={}", kAniso[uint32_t(f.aniso_filter)]);
  if (f.vol_mag_filter != TextureFilter::kUseFetchConst) {
    out += std::format(", VolMagFilter={}", kFilters[uint32_t(f.vol_mag_filter)]);
  }
  if (f.vol_min_filter != TextureFilter::kUseFetchConst) {
    out += std::format(", VolMinFilter={}", kFilters[uint32_t(f.vol_min_filter)]);
  }
  if (!f.use_computed_lod) out += ", UseComputedLOD=false";
  if (f.use_register_lod) out += ", UseRegisterLOD=true";
  if (f.use_register_gradients) out += ", UseRegisterGradients=true";
  if (f.lod_bias != 0.0f) out += std::format(", LODBias={:g}", f.lod_bias);
  const uint32_t n = FetchDimensionComponentCount(f.dimension);
  if (f.offset_x != 0.0f) out += std::format(", OffsetX={:g}", f.offset_x);
  if (n > 1 && f.offset_y != 0.0f) out += std::format(", OffsetY={:g}", f.offset_y);
  if (n > 2 && f.offset_z != 0.0f) out += std::format(", OffsetZ={:g}", f.offset_z);
  out += '\n';
}

void AppendAlu(const Alu& alu, std::string& out) {
  const bool vector_nop = alu.IsVectorOpDefaultNop();
  const bool scalar_nop = alu.IsScalarOpDefaultNop();
  if (vector_nop && scalar_nop) {
    AppendInstructionPrefix(alu.predicated, alu.predicate_condition, out);
    out += "nop\n";
    return;
  }
  if (!vector_nop) {
    AppendInstructionPrefix(alu.predicated, alu.predicate_condition, out);
    out += kVectorInfos[uint32_t(alu.vector_opcode)].name;
    if (alu.vector_result.clamp) out += "_sat";
    out += ' ';
    AppendResult(alu.vector_result, out);
    for (uint32_t i = 0; i < alu.vector_operand_count; ++i) {
      out += ", ";
      AppendOperand(alu.vector_operands[i], out);
    }
    out += '\n';
  }
  if (!scalar_nop) {
    // A scalar op paired with a vector op is printed as a "+" continuation line.
    out += vector_nop ? "   " : "              + ";
    out += alu.predicated ? (alu.predicate_condition ? " (p0) " : "(!p0) ") : "      ";
    out += kScalarInfos[uint32_t(alu.scalar_opcode)].name;
    if (alu.scalar_result.clamp) out += "_sat";
    out += ' ';
    AppendResult(alu.scalar_result, out);
    for (uint32_t i = 0; i < alu.scalar_operand_count; ++i) {
      out += ", ";
      AppendOperand(alu.scalar_operands[i], out);
    }
    out += '\n';
  }
}

void AppendExec(const ControlFlow& cf, std::string& out) {
  const char* name;
  if (cf.exec_type == ControlFlow::ExecType::kConditional) name = cf.exec_end ? "cexece" : "cexec";
  else name = cf.exec_end ? "exece" : "exec";
  switch (cf.exec_type) {
    case ControlFlow::ExecType::kUnconditional: out += std::format("      {}", name); break;
    case ControlFlow::ExecType::kPredicated: out += cf.condition ? " (p0) " : "(!p0) "; out += name; break;
    case ControlFlow::ExecType::kConditional:
      out += std::format("      {} {}b{}", name, cf.condition ? "" : "!", cf.bool_constant);
      break;
  }
  if (cf.yield) {
    if (cf.exec_type == ControlFlow::ExecType::kConditional) out += ',';
    out += " Yield=true";
  }
  if (!cf.predicate_clean) out += "    // PredicateClean=false";
  out += '\n';
  for (const Instruction& in : cf.instructions) {
    out += std::format("/* {:4d}   */ ", in.address);
    if (in.serialize) out += "         serialize\n             ";
    switch (in.kind) {
      case Instruction::Kind::kVertexFetch: AppendVertexFetch(in.vfetch, out); break;
      case Instruction::Kind::kTextureFetch: AppendTextureFetch(in.tfetch, out); break;
      case Instruction::Kind::kAlu: AppendAlu(in.alu, out); break;
    }
  }
}

void AppendBranchHead(const ControlFlow& cf, const char* plain, const char* conditional, std::string& out) {
  switch (cf.branch_type) {
    case ControlFlow::ExecType::kUnconditional: out += std::format("      {} ", plain); break;
    case ControlFlow::ExecType::kPredicated: out += cf.condition ? " (p0) " : "(!p0) "; out += plain; out += ' '; break;
    case ControlFlow::ExecType::kConditional:
      out += std::format("      {} ", conditional);
      if (!cf.condition) out += '!';
      out += std::format("b{}, ", cf.bool_constant);
      break;
  }
  out += std::format("L{}\n", cf.target);
}

}  // namespace

const char* IssueKindName(Issue::Kind kind) {
  switch (kind) {
    case Issue::Kind::kUnknownFetchOpcode: return "unknown fetch opcode";
    case Issue::Kind::kUnknownVectorOpcode: return "unknown vector opcode";
    case Issue::Kind::kUnknownScalarOpcode: return "unknown scalar opcode";
    case Issue::Kind::kInvalidFetchSwizzle: return "invalid fetch swizzle";
    case Issue::Kind::kUnknownVertexFormat: return "unknown vertex format";
    case Issue::Kind::kUnknownExportRegister: return "unknown export register";
    case Issue::Kind::kInstructionPastEnd: return "instruction past end";
    case Issue::Kind::kMiniFetchWithoutFull: return "vfetch_mini without vfetch_full";
    case Issue::Kind::kMarkVsFetchDone: return "mark_vs_fetch_done";
    case Issue::Kind::kCanaryTfetch1DTwoComponents: return "canary/rexglue differ: tfetch1D XY";
    case Issue::Kind::kCanaryScalarOperandZ: return "canary/rexglue differ: scalar operand b = Z";
  }
  return "?";
}

const char* VectorOpcodeName(AluVectorOpcode op) { return kVectorInfos[uint32_t(op) & 31].name; }
const char* ScalarOpcodeName(AluScalarOpcode op) { return kScalarInfos[uint32_t(op) & 63].name; }

bool Alu::IsVectorOpDefaultNop() const {
  // "max r0._, r0, r0" is what the assembler emits for an empty vector slot.
  auto plain_r0 = [](const Operand& o) {
    return o.source == Source::kRegister && o.index == 0 && o.addressing == Addressing::kAbsolute && !o.negate &&
           !o.absolute && o.IsStandardSwizzle();
  };
  if (vector_opcode != AluVectorOpcode::kMax || vector_result.write_mask || vector_result.clamp ||
      !plain_r0(vector_operands[0]) || !plain_r0(vector_operands[1])) {
    return false;
  }
  if (vector_result.target == Target::kRegister) {
    return vector_result.index == 0 && vector_result.addressing == Addressing::kAbsolute;
  }
  // For an export with both slots empty the vector op is kept so the text still shows the export target.
  return !IsScalarOpDefaultNop();
}

bool Alu::IsScalarOpDefaultNop() const {
  if (scalar_opcode != AluScalarOpcode::kRetainPrev || scalar_result.write_mask || scalar_result.clamp) return false;
  if (scalar_result.target == Target::kRegister) {
    return scalar_result.index == 0 && scalar_result.addressing == Addressing::kAbsolute;
  }
  return true;
}

Program Decode(const uint32_t* dwords, size_t dword_count, bool is_vertex_shader, const DecodeOptions& options) {
  Program p;
  p.is_vertex_shader = is_vertex_shader;
  Ctx ctx{p, options};
  // Control flow comes first, two 48-bit instructions per 3 dwords. Its end is guessed as the smallest instruction
  // address any exec points at (the scan bound shrinks as execs are found, exactly as the SDK's loop does).
  uint32_t bound = uint32_t(dword_count / 3);
  for (uint32_t i = 0; i < bound; ++i) {
    uint32_t cf[2][2];
    UnpackCfPair(dwords + i * 3, cf);
    for (int j = 0; j < 2; ++j) {
      const uint32_t op = Bits(cf[j][1], 12, 4);
      if (IsExecOpcode(op)) bound = std::min(bound, Bits(cf[j][0], 0, 12));
      if (op == uint32_t(CfOpcode::kCondCall) || op == uint32_t(CfOpcode::kCondJmp) ||
          op == uint32_t(CfOpcode::kLoopStart) || op == uint32_t(CfOpcode::kLoopEnd)) {
        p.labels.insert(Bits(cf[j][0], 0, 13));
      }
    }
  }

  VFetchWords previous_full;
  bool have_full = false;
  for (uint32_t i = 0; i < bound; ++i) {
    uint32_t cfw[2][2];
    UnpackCfPair(dwords + i * 3, cfw);
    for (int j = 0; j < 2; ++j) {
      const uint32_t d0 = cfw[j][0], d1 = cfw[j][1];
      ControlFlow cf;
      cf.opcode = CfOpcode(Bits(d1, 12, 4));
      cf.index = i * 2 + j;
      ctx.address = cf.index;
      if (cf.opcode == CfOpcode::kMarkVsFetchDone) ctx.Flag(Issue::Kind::kMarkVsFetchDone, "mark_vs_fetch_done");
      switch (cf.opcode) {
        case CfOpcode::kExec:
        case CfOpcode::kExecEnd:
        case CfOpcode::kCondExec:
        case CfOpcode::kCondExecEnd:
        case CfOpcode::kCondExecPredClean:
        case CfOpcode::kCondExecPredCleanEnd:
        case CfOpcode::kCondExecPred:
        case CfOpcode::kCondExecPredEnd: {
          cf.exec_address = Bits(d0, 0, 12);
          cf.exec_count = Bits(d0, 12, 3);
          cf.yield = Bits(d0, 15, 1);
          cf.sequence = Bits(d0, 16, 12);
          switch (cf.opcode) {
            case CfOpcode::kExec:
            case CfOpcode::kExecEnd:
              cf.exec_type = ControlFlow::ExecType::kUnconditional;
              cf.exec_end = cf.opcode == CfOpcode::kExecEnd;
              cf.predicate_clean = Bits(d1, 9, 1);
              break;
            case CfOpcode::kCondExecPred:
            case CfOpcode::kCondExecPredEnd:
              cf.exec_type = ControlFlow::ExecType::kPredicated;
              cf.exec_end = cf.opcode == CfOpcode::kCondExecPredEnd;
              cf.predicate_clean = Bits(d1, 9, 1);
              cf.condition = Bits(d1, 10, 1);
              break;
            default:  // conditional on a bool constant; the "PredClean" variants always clean the predicate
              cf.exec_type = ControlFlow::ExecType::kConditional;
              cf.exec_end = cf.opcode == CfOpcode::kCondExecEnd || cf.opcode == CfOpcode::kCondExecPredCleanEnd;
              cf.bool_constant = Bits(d1, 2, 8);
              cf.condition = Bits(d1, 10, 1);
              cf.predicate_clean = !(cf.opcode == CfOpcode::kCondExec || cf.opcode == CfOpcode::kCondExecEnd);
              break;
          }
          uint32_t sequence = cf.sequence;
          for (uint32_t a = cf.exec_address; a < cf.exec_address + cf.exec_count; ++a, sequence >>= 2) {
            Instruction in;
            in.address = a;
            in.serialize = sequence & 0b10;
            // Instructions past the end of the microcode would be garbage; the SDK reads them anyway (it trusts the
            // count), so read zeros instead of overrunning our buffer.
            uint32_t w[3] = {};
            for (int k = 0; k < 3; ++k) if (size_t(a) * 3 + k < dword_count) w[k] = dwords[a * 3 + k];
            ctx.address = a;
            if (size_t(a) * 3 + 3 > dword_count) ctx.Flag(Issue::Kind::kInstructionPastEnd, std::format("slot {}", a));
            if (sequence & 0b01) {
              if (Bits(w[0], 0, 5) == uint32_t(FetchOpcode::kVertexFetch)) {
                in.kind = Instruction::Kind::kVertexFetch;
                in.vfetch = DecodeVertexFetch(w, previous_full, have_full, ctx);
                if (!in.vfetch.mini) { std::copy(w, w + 3, previous_full.w); have_full = true; }
              } else {
                in.kind = Instruction::Kind::kTextureFetch;
                in.tfetch = DecodeTextureFetch(w, ctx);
              }
            } else {
              in.kind = Instruction::Kind::kAlu;
              in.alu = DecodeAlu(w, is_vertex_shader, ctx);
            }
            cf.instructions.push_back(in);
          }
        } break;
        case CfOpcode::kLoopStart:
          cf.target = Bits(d0, 0, 13);
          cf.repeat = Bits(d0, 13, 1);
          cf.loop_constant = Bits(d0, 16, 5);
          break;
        case CfOpcode::kLoopEnd:
          cf.target = Bits(d0, 0, 13);
          cf.loop_constant = Bits(d0, 16, 5);
          cf.predicated_break = Bits(d0, 21, 1);
          cf.condition = Bits(d1, 10, 1);
          break;
        case CfOpcode::kCondCall:
        case CfOpcode::kCondJmp:
          cf.target = Bits(d0, 0, 13);
          if (Bits(d0, 13, 1)) {
            cf.branch_type = ControlFlow::ExecType::kUnconditional;
          } else if (Bits(d0, 14, 1)) {
            cf.branch_type = ControlFlow::ExecType::kPredicated;
            cf.condition = Bits(d1, 10, 1);
          } else {
            cf.branch_type = ControlFlow::ExecType::kConditional;
            cf.bool_constant = Bits(d1, 2, 8);
            cf.condition = Bits(d1, 10, 1);
          }
          break;
        case CfOpcode::kAlloc:
          cf.alloc_count = Bits(d0, 0, 3);
          cf.alloc_type = AllocType(Bits(d1, 9, 2));
          break;
        default:
          break;
      }
      p.control_flow.push_back(std::move(cf));
    }
  }
  return p;
}

std::string Disassemble(const Program& p) {
  std::string out;
  for (const ControlFlow& cf : p.control_flow) {
    if (p.labels.count(cf.index)) out += std::format("                label L{}\n", cf.index);
    out += std::format("/* {:4d}.{} */ ", cf.index / 2, cf.index % 2);
    switch (cf.opcode) {
      case CfOpcode::kNop: out += "      cnop\n"; break;
      case CfOpcode::kExec:
      case CfOpcode::kExecEnd:
      case CfOpcode::kCondExec:
      case CfOpcode::kCondExecEnd:
      case CfOpcode::kCondExecPredClean:
      case CfOpcode::kCondExecPredCleanEnd:
      case CfOpcode::kCondExecPred:
      case CfOpcode::kCondExecPredEnd: AppendExec(cf, out); break;
      case CfOpcode::kLoopStart:
        out += std::format("      loop i{}, L{}", cf.loop_constant, cf.target);
        if (cf.repeat) out += ", Repeat=true";
        out += '\n';
        break;
      case CfOpcode::kLoopEnd:
        out += cf.predicated_break ? (cf.condition ? " (p0) " : "(!p0) ") : "      ";
        out += std::format("endloop i{}, L{}\n", cf.loop_constant, cf.target);
        break;
      case CfOpcode::kCondCall: AppendBranchHead(cf, "call", "ccall", out); break;
      case CfOpcode::kReturn: out += "      ret\n"; break;
      case CfOpcode::kCondJmp: AppendBranchHead(cf, "jmp", "cjmp", out); break;
      case CfOpcode::kAlloc:
        out += "      alloc ";
        switch (cf.alloc_type) {
          case AllocType::kNone: break;
          case AllocType::kVsPosition: out += "position"; break;
          case AllocType::kVsInterpolatorsOrPsColors: out += p.is_vertex_shader ? "interpolators" : "colors"; break;
          case AllocType::kMemory: out += std::format("export = {}", cf.alloc_count); break;
        }
        out += '\n';
        break;
      case CfOpcode::kMarkVsFetchDone:
        // SDK quirk: only the "/* n.m */ " prefix is written, with no text and no newline.
        break;
    }
  }
  return out;
}

}  // namespace xenos::ucode
