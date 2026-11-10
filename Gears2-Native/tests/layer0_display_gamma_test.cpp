// Gears2-Native - MIT. Stage 5C: gamma ramp tracking from DC_LUT_* register writes (xenos/display_gamma.h).
#include <catch2/catch_test_macros.hpp>

#include "xenos/display_gamma.h"

namespace g = xenos::display_gamma;

TEST_CASE("gamma: initial ramps are the SDK's linear ones", "[gamma]") {
  g::Ramps r;
  CHECK(r.table[0] == 0);
  CHECK(r.table[255] == 0x3FFFFFFFu);
  CHECK(r.table[1] == ((4u << 20) | (4u << 10) | 4u));  // 1 * 0x3FF / 0xFF = 4
  CHECK(r.pwl[0][0] == (0x200u << 16));
  CHECK(r.pwl[127][2] == 0xFFC0u);  // base 0xFFFF & ~0x3F, delta 0 for the last entry
}

TEST_CASE("gamma: D3D's 256-entry table writes (DC_LUT_30_COLOR, register_table.inc example)", "[gamma]") {
  g::Ramps r;
  r.OnRegisterWrite(g::kRegLutRwMode, 0);
  r.OnRegisterWrite(g::kRegLutRwIndex, 0);
  r.OnRegisterWrite(g::kRegLutWriteEnMask, 7);
  r.OnRegisterWrite(g::kRegLut30Color, 0x00000000);
  r.OnRegisterWrite(g::kRegLutRwIndex, 1);
  r.OnRegisterWrite(g::kRegLut30Color, 0x04812048);
  r.OnRegisterWrite(g::kRegLut30Color, 0x05916459);  // index auto-incremented to 2
  CHECK(r.table[1] == 0x04812048u);
  CHECK(r.table[2] == 0x05916459u);
  r.OnRegisterWrite(g::kRegLutWriteEnMask, 1);  // blue only
  r.OnRegisterWrite(g::kRegLutRwIndex, 1);
  r.OnRegisterWrite(g::kRegLut30Color, 0x3FFFFFFF);
  CHECK(r.table[1] == ((0x04812048u & ~0x3FFu) | 0x3FFu));
}

TEST_CASE("gamma: sequential colour and PWL writes (R, G, B order; B, G, R enable mask)", "[gamma]") {
  g::Ramps r;
  r.OnRegisterWrite(g::kRegLutWriteEnMask, 7);
  r.OnRegisterWrite(g::kRegLutRwIndex, 5);
  r.OnRegisterWrite(g::kRegLutSeqColor, 0x1000 << 6);  // R, low 6 bits of seq_color dropped
  r.OnRegisterWrite(g::kRegLutSeqColor, 0x0020 << 6);  // G
  r.OnRegisterWrite(g::kRegLutSeqColor, 0x0003 << 6 | 0x3F);  // B
  CHECK(r.table[5] == ((0x000u << 20) | (0x020u << 10) | 0x003u));  // 0x1000 & 0x3FF = 0 (10 bits)
  r.OnRegisterWrite(g::kRegLutSeqColor, 0x3FF << 6);  // entry 6 R
  CHECK(((r.table[6] >> 20) & 0x3FF) == 0x3FFu);
  // PWL: D3D's example, three writes per entry, index bit 7 ignored, auto-increment
  r.OnRegisterWrite(g::kRegLutRwMode, 1);
  r.OnRegisterWrite(g::kRegLutRwIndex, 0x80 | 0x7D);
  for (int i = 0; i < 3; ++i) r.OnRegisterWrite(g::kRegLutPwlData, 0x0200FBC0);
  for (int i = 0; i < 3; ++i) r.OnRegisterWrite(g::kRegLutPwlData, 0x0200FDFF);  // entry 0x7E, low 6 bits dropped
  CHECK(r.pwl[0x7D][1] == 0x0200FBC0u);
  CHECK(r.pwl[0x7E][2] == 0x0200FDC0u);
  r.OnRegisterWrite(g::kRegLutWriteEnMask, 4);  // red only
  r.OnRegisterWrite(g::kRegLutRwIndex, 0x10);
  for (int i = 0; i < 3; ++i) r.OnRegisterWrite(g::kRegLutPwlData, 0x01000040);
  CHECK(r.pwl[0x10][0] == 0x01000040u);
  CHECK(r.pwl[0x10][1] != 0x01000040u);
  CHECK_FALSE(r.OnRegisterWrite(0x2000, 1));
}
