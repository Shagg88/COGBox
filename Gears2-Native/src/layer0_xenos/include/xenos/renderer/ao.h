// Layer 0 (game-free): screen-space ambient occlusion (notes/79) - the camera maths XeGTAO (Intel, MIT) needs, read
// off the game's own view-projection and viewport, shared by the Gears ports.
//
// Conventions:
//   - The camera matrix is upscaler.h's Mat4 (row-major, clip_i = dot(row_i, (x, y, z, 1))): FromConstantsTransposed
//     of the vertex constants the game keeps it in (UE3: c0-c3, camera-relative, no translation - only the projection
//     is needed here).
//   - Guest depth = the value the game's depth buffer holds (0..1, before our host-side x0.5 float24 remap):
//     depth = DepthMap.scale * (clip.z / clip.w) + DepthMap.offset, the viewport's Z transform. Gears 2 / UE3 on the
//     360 draws reversed-Z through it (scale -1, offset 1: 1 at the near plane, ~0.001 at infinity; notes/79 s.8).
//   - View space as XeGTAO's shaders rebuild it: z = distance along the camera's forward axis (clip.w per unit length
//     of the w row), x / y = ndc x / y * z * tan(half field of view) per axis. A mirrored axis (the guest's viewport
//     y direction) mirrors the positions and their normals alike, so AO doesn't depend on it.
// Our code (MIT). XeGTAO's own licence is in THIRD_PARTY_NOTICES.md once its shaders are in.
#pragma once

#include <cstdint>
#include <string>

#include "xenos/renderer/pipeline_state.h"
#include "xenos/renderer/upscaler.h"

namespace xenos::renderer::ao {

// --gow2_ao: game = the game's own AO (as before), gtao = XeGTAO in its place, off = no AO.
enum class Mode : uint32_t { kGame, kGtao, kOff };
Mode ParseMode(const std::string& name);  // anything unknown -> kGame
const char* ModeName(Mode mode);

// guest depth = scale * (clip.z / clip.w) + offset.
struct DepthMap {
  float scale = 1.0f, offset = 0.0f;
};
// From a draw's translated viewport (pipeline_state.h TranslateViewport, before any host-side remap).
DepthMap ViewportDepthMap(const ViewportState& viewport);

// XeGTAO's GTAOConstants camera part (XeGTAO.h GTAOUpdateConstants, our derivation from a Mat4 + DepthMap).
struct GtaoConstants {
  uint32_t width = 0, height = 0;          // the depth texture's size, host pixels
  float pixel_size[2] = {};                // 1 / size
  float depth_unpack[2] = {};              // view z = depth_unpack[0] / (depth_unpack[1] - guest depth)
  float tan_half_fov[2] = {};              // x, y
  float ndc_to_view_mul[2] = {};           // view xy = (uv * mul + add) * view z  (uv: 0..1, v down)
  float ndc_to_view_add[2] = {};
  float ndc_to_view_mul_x_pixel_size[2] = {};
  float near_plane = 0.0f;                 // view z at guest depth of the near plane (diagnostics)
};
// False when the matrix isn't a perspective projection the shader can use (an orthographic UI pass, a degenerate
// matrix, a depth map that leaves no depth range).
bool MakeGtaoConstants(const upscale::Mat4& view_proj, const DepthMap& depth, uint32_t width, uint32_t height,
                       GtaoConstants& out);

// What the shaders compute, for tests and diagnostics.
float ViewDepth(const GtaoConstants& c, float guest_depth);
void ViewPosition(const GtaoConstants& c, float u, float v, float guest_depth, float out[3]);

}  // namespace xenos::renderer::ao
