// Gears2-Native - MIT. Stage 5C, display gamma ramps (notes/49 section 10).
// The display gamma ramps (256-entry table and 128-entry PWL), tracked from the DC_LUT_* register writes the PM4 ring
// carries. The write protocol is the SDK's CommandProcessor::WriteRegister (rexglue, BSD; from Xenia, Copyright 2022
// Ben Vanik, BSD) re-expressed: sequential R, G, B component writes with a blue-green-red write-enable mask, the
// read/write index auto-incrementing after each entry.
#pragma once

#include <cstdint>

namespace xenos::display_gamma {

constexpr uint32_t kRegLutRwMode = 0x1921, kRegLutRwIndex = 0x1922, kRegLutSeqColor = 0x1923,
                   kRegLutPwlData = 0x1924, kRegLut30Color = 0x1925, kRegLutWriteEnMask = 0x1927;

struct Ramps {
  // 256-entry table: blue bits 0:9, green 10:19, red 20:29 (DC_LUT_30_COLOR layout).
  uint32_t table[256];
  // PWL: [entry][component R, G, B]: base bits 0:15, delta bits 16:31 (DC_LUT_PWL_DATA layout), low 6 bits zero.
  uint32_t pwl[128][3];
  uint64_t table_version = 0, pwl_version = 0;  // bumped on every entry written

  Ramps() { Reset(); }
  void Reset();  // the SDK's initial (linear) ramps
  // A register write. Returns true if it was a DC_LUT register.
  bool OnRegisterWrite(uint32_t index, uint32_t value);

 private:
  uint32_t rw_mode_ = 0, rw_index_ = 0, write_en_mask_ = 0, component_ = 0;
};

}  // namespace xenos::display_gamma

// The ramps our PM4 processor tracks from the DC_LUT_* writes it runs (--gow2_pm4_execute; pm4_shadow.cpp); nullptr
// while it does not run the ring. Read on the GPU thread (observer callbacks).
const xenos::display_gamma::Ramps* XenosGammaRamps();
