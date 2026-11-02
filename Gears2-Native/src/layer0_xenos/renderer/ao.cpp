// Layer 0 (game-free): the camera maths for screen-space AO (ao.h, notes/80). Our code (MIT).
// XeGTAO ambient occlusion, upscaler motion-vector / depth fixes and SMAA T2x: PR #139 by gmihaic (https://github.com/gmihaic).
#include "xenos/renderer/ao.h"

#include <cmath>

namespace xenos::renderer::ao {

Mode ParseMode(const std::string& name) {
  if (name == "gtao") return Mode::kGtao;
  if (name == "off") return Mode::kOff;
  return Mode::kGame;
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kGtao: return "XeGTAO";
    case Mode::kOff: return "off";
    default: return "game";
  }
}

DepthMap ViewportDepthMap(const ViewportState& v) {
  // TranslateViewport: the depth written is z_min + (z_max - z_min) * (ndc_scale[2] * z / w + ndc_offset[2]).
  const float range = v.z_max - v.z_min;
  return {range * v.ndc_scale[2], v.z_min + range * v.ndc_offset[2]};
}

bool MakeGtaoConstants(const upscale::Mat4& m, const DepthMap& depth, uint32_t width, uint32_t height,
                       GtaoConstants& out) {
  out = {};
  if (!width || !height) return false;
  auto len3 = [&](int r) {
    return std::sqrt(double(m(r, 0)) * m(r, 0) + double(m(r, 1)) * m(r, 1) + double(m(r, 2)) * m(r, 2));
  };
  const double xs = len3(0), ys = len3(1), ws = len3(3);
  if (xs < 1e-6 || ys < 1e-6 || ws < 1e-6) return false;  // no perspective divide (UI, full-screen passes)
  // clip z = k * clip w + b: the z row is the w row scaled (a perspective matrix), plus a constant.
  const double k = upscale::DepthAtInfinity(m);
  double residual = 0.0;
  for (int c = 0; c < 3; ++c) residual = std::fmax(residual, std::fabs(m(2, c) - k * m(3, c)));
  if (residual > 1e-3 * std::fabs(k) * ws + 1e-7) return false;
  const double b = double(m(2, 3)) - k * double(m(3, 3));
  // guest depth d = A * (k + b / clip w) + B, clip w = ws * view z
  //   => view z = (-A * b / ws) / ((A * k + B) - d)   (XeGTAO: mul / (add - d))
  const double a_scale = depth.scale, b_offset = depth.offset;
  const double mul = -a_scale * b / ws, add = a_scale * k + b_offset;
  const double near_z = -b / (k * ws);  // clip z = 0 (D3D clip space)
  if (!std::isfinite(mul) || !std::isfinite(add) || std::fabs(mul) < 1e-9 || !(near_z > 0.0)) return false;
  out.width = width;
  out.height = height;
  out.pixel_size[0] = 1.0f / float(width);
  out.pixel_size[1] = 1.0f / float(height);
  out.depth_unpack[0] = float(mul);
  out.depth_unpack[1] = float(add);
  out.tan_half_fov[0] = float(ws / xs);
  out.tan_half_fov[1] = float(ws / ys);
  // XeGTAO.h GTAOUpdateConstants.
  out.ndc_to_view_mul[0] = out.tan_half_fov[0] * 2.0f;
  out.ndc_to_view_mul[1] = out.tan_half_fov[1] * -2.0f;
  out.ndc_to_view_add[0] = -out.tan_half_fov[0];
  out.ndc_to_view_add[1] = out.tan_half_fov[1];
  out.ndc_to_view_mul_x_pixel_size[0] = out.ndc_to_view_mul[0] * out.pixel_size[0];
  out.ndc_to_view_mul_x_pixel_size[1] = out.ndc_to_view_mul[1] * out.pixel_size[1];
  out.near_plane = float(near_z);
  return true;
}

float ViewDepth(const GtaoConstants& c, float guest_depth) {
  // XeGTAO.hlsli XeGTAO_ScreenSpaceToViewSpaceDepth.
  return c.depth_unpack[0] / (c.depth_unpack[1] - guest_depth);
}

void ViewPosition(const GtaoConstants& c, float u, float v, float guest_depth, float out[3]) {
  // XeGTAO.hlsli XeGTAO_ComputeViewspacePosition.
  const float z = ViewDepth(c, guest_depth);
  out[0] = (c.ndc_to_view_mul[0] * u + c.ndc_to_view_add[0]) * z;
  out[1] = (c.ndc_to_view_mul[1] * v + c.ndc_to_view_add[1]) * z;
  out[2] = z;
}

}  // namespace xenos::renderer::ao
