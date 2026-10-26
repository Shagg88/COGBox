// Gears2-Native - MIT. See xenos/display_gamma.h.
#include "xenos/display_gamma.h"

namespace xenos::display_gamma {

void Ramps::Reset() {
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = i * 0x3FF / 0xFF;
    table[i] = v | (v << 10) | (v << 20);
  }
  for (uint32_t i = 0; i < 128; ++i) {
    const uint32_t base = (i * 0xFFFF / 0x7F) & ~0x3Fu, delta = i < 0x7F ? 0x200u : 0u;
    for (uint32_t c = 0; c < 3; ++c) pwl[i][c] = base | (delta << 16);
  }
  rw_mode_ = rw_index_ = write_en_mask_ = component_ = 0;
  ++table_version;
  ++pwl_version;
}

bool Ramps::OnRegisterWrite(uint32_t index, uint32_t value) {
  switch (index) {
    case kRegLutRwMode:
      rw_mode_ = value;
      return true;
    case kRegLutWriteEnMask:
      write_en_mask_ = value;
      return true;
    case kRegLutRwIndex:  // resets the sequential component index
      rw_index_ = value;
      component_ = 0;
      return true;
    case kRegLutSeqColor: {  // 256-entry table, components in R, G, B order; the enable mask is B, G, R
      const bool write = (write_en_mask_ & (1u << (2 - component_))) != 0;
      if (write) {
        uint32_t& e = table[rw_index_ & 0xFF];
        const uint32_t c = (value & 0xFFFF) >> 6;  // DC_LUT_SEQ_COLOR.seq_color (16 bits), bits 0:5 hardwired zero
        const uint32_t shift = component_ == 0 ? 20u : component_ == 1 ? 10u : 0u;
        e = (e & ~(0x3FFu << shift)) | ((c & 0x3FF) << shift);
        ++table_version;
      }
      if (++component_ >= 3) {
        component_ = 0;
        rw_index_ = (rw_index_ & ~0xFFu) | ((rw_index_ + 1) & 0xFF);
      }
      return true;
    }
    case kRegLutPwlData: {  // PWL, index bit 7 ignored
      const uint32_t entry = rw_index_ & 0x7F;
      const bool write = (write_en_mask_ & (1u << (2 - component_))) != 0;
      if (write) {
        pwl[entry][component_] = value & ~0x003F003Fu;  // base and delta: bits 0:5 hardwired zero
        ++pwl_version;
      }
      if (++component_ >= 3) {
        component_ = 0;
        rw_index_ = (rw_index_ & ~0x7Fu) | ((entry + 1) & 0x7F);
      }
      return true;
    }
    case kRegLut30Color: {  // 256-entry table, whole entry, per-component enable (B, G, R bits)
      const uint32_t mask = write_en_mask_ & 7;
      if (mask) {
        uint32_t& e = table[rw_index_ & 0xFF];
        if (mask & 1) e = (e & ~0x3FFu) | (value & 0x3FF);
        if (mask & 2) e = (e & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
        if (mask & 4) e = (e & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
        ++table_version;
      }
      component_ = 0;
      rw_index_ = (rw_index_ & ~0xFFu) | ((rw_index_ + 1) & 0xFF);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace xenos::display_gamma
