// Unit tests for the translator's float-constant compaction (hlsl.cpp, HlslOptions::compact_float_constants; notes/52
// section 29). Hand-built one-instruction pixel shaders: oC0 = c[a] + c[b]. Game-free. Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "xenos/hlsl.h"
#include "xenos/ucode.h"

namespace uc = xenos::ucode;
namespace hl = xenos::hlsl;

namespace {
uc::Operand Constant(uint32_t index, uc::Addressing addressing = uc::Addressing::kAbsolute) {
  uc::Operand o;
  o.source = uc::Source::kConstantFloat;
  o.index = index;
  o.addressing = addressing;
  return o;
}

// A pixel shader with one exec_end holding `oC0 = a + b`.
uc::Program AddShader(const uc::Operand& a, const uc::Operand& b) {
  uc::Instruction ins;
  ins.kind = uc::Instruction::Kind::kAlu;
  ins.alu.vector_opcode = uc::AluVectorOpcode::kAdd;
  ins.alu.vector_result.target = uc::Target::kColor;
  ins.alu.vector_result.index = 0;
  ins.alu.vector_result.write_mask = 0xF;
  ins.alu.vector_operand_count = 2;
  ins.alu.vector_operands[0] = a;
  ins.alu.vector_operands[1] = b;
  ins.alu.scalar_opcode = uc::AluScalarOpcode::kRetainPrev;
  uc::ControlFlow cf;
  cf.opcode = uc::CfOpcode::kExecEnd;
  cf.exec_end = true;
  cf.exec_count = 1;
  cf.instructions.push_back(ins);
  uc::Program p;
  p.is_vertex_shader = false;
  p.control_flow.push_back(cf);
  return p;
}

bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
}  // namespace

TEST_CASE("compaction off: the layout is c[0, n) and the HLSL indexes the real registers", "[hlsl][constants]") {
  const hl::Translation t = hl::TranslateToHlsl(AddShader(Constant(2), Constant(253)));
  CHECK(t.float_constants == 254);
  CHECK(t.float_constants_low == 254);
  CHECK(t.float_constants_high == 254);
  CHECK(Has(t.hlsl, "xe_c[2]"));
  CHECK(Has(t.hlsl, "xe_c[253]"));
}

TEST_CASE("compaction: a low block plus the literal block reads one array of both", "[hlsl][constants]") {
  hl::HlslOptions o;
  o.compact_float_constants = true;
  const hl::Translation t = hl::TranslateToHlsl(AddShader(Constant(2), Constant(253)), o);
  CHECK(t.float_constants == 254);
  CHECK(t.float_constants_low == 3);    // c0-c2 kept where they are
  CHECK(t.float_constants_high == 253);  // c253 follows them, at 3
  CHECK(Has(t.hlsl, "xe_c[2]"));
  CHECK(Has(t.hlsl, "xe_c[3]"));
  CHECK(!Has(t.hlsl, "xe_c[253]"));
  // Uploaded registers: low block + high block.
  CHECK(t.float_constants_low + (t.float_constants - t.float_constants_high) == 4);
}

TEST_CASE("compaction: only the top block read (gap below the first constant)", "[hlsl][constants]") {
  hl::HlslOptions o;
  o.compact_float_constants = true;
  const hl::Translation t = hl::TranslateToHlsl(AddShader(Constant(252), Constant(255)), o);
  CHECK(t.float_constants == 256);
  CHECK(t.float_constants_low == 0);
  CHECK(t.float_constants_high == 252);
  CHECK(Has(t.hlsl, "xe_c[0]"));
  CHECK(Has(t.hlsl, "xe_c[3]"));
  CHECK(!Has(t.hlsl, "xe_c[252]"));
}

TEST_CASE("compaction: a gap under 16 registers is left alone, HLSL unchanged", "[hlsl][constants]") {
  hl::HlslOptions o;
  o.compact_float_constants = true;
  const uc::Program p = AddShader(Constant(1), Constant(16));  // unread c2-c15: 14 registers
  const hl::Translation on = hl::TranslateToHlsl(p, o);
  const hl::Translation off = hl::TranslateToHlsl(p);
  CHECK(on.float_constants_low == on.float_constants);
  CHECK(on.float_constants_high == on.float_constants);
  CHECK(on.hlsl == off.hlsl);  // same text -> same shader-cache file
}

TEST_CASE("compaction: any relative constant read keeps all 256", "[hlsl][constants]") {
  hl::HlslOptions o;
  o.compact_float_constants = true;
  const hl::Translation t =
      hl::TranslateToHlsl(AddShader(Constant(2, uc::Addressing::kAddressRegisterRelative), Constant(253)), o);
  CHECK(t.float_constants == 256);
  CHECK(t.float_constants_low == 256);
  CHECK(t.float_constants_high == 256);
  CHECK(Has(t.hlsl, "xe_c[253]"));
}
