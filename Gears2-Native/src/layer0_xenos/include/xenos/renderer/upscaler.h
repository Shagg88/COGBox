// Layer 0 (game-free): temporal upscalers (NVIDIA DLSS, AMD FSR, Intel XeSS) behind one small interface (notes/64,
// notes/65), shared by the
// Gears 2 and Gears 3 ports. The renderer supplies, per frame: the scene colour before post-processing, its depth, a
// per-pixel motion texture (camera reprojection from depth, with the game's own velocity on top where it has one) and
// the sub-pixel jitter it applied to the 3D passes' viewport; the upscaler writes the anti-aliased (and, later, upscaled)
// colour, which replaces the scene colour for the post passes.
//
// Conventions (every SDK is configured to match them):
//   - Jitter: in render-resolution pixels, +x right, +y down, each component in (-0.5, 0.5); the offset by which the
//     rendered image is shifted (the viewport moves by it), so a sample lands at pixel centre + jitter.
//   - Motion: render-resolution pixels, (previous position - current position) of the surface seen at the pixel, no
//     jitter in it; +x right, +y down.
//   - Matrices: row-major, clip_i = dot(row_i, (x, y, z, 1)). A game whose shaders compute x*c0 + y*c1 + z*c2 + c3
//     (UE3, job 252) stores the transpose in its constants - FromConstants / FromConstantsTransposed.
//   - Depth: guest convention (0 near .. 1 far, not inverted unless the game says so).
// The SDK wrappers (dlss_upscaler.cpp, fsr_upscaler.cpp, xess_upscaler.cpp) are built only when their SDK is in
// third_party (GN_HAVE_DLSS, GN_HAVE_FSR, GN_HAVE_XESS); without one, CreateUpscaler returns null and the launcher's
// choice falls back to SMAA / off.
// Our code (MIT). The SDKs keep their own licences (THIRD_PARTY_NOTICES.md).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenos/renderer/gpu.h"

namespace xenos::renderer::upscale {

enum class Kind : uint32_t { kNone, kDlss, kFsr, kXess };
// Render size / output size: 1 (native-resolution AA: DLAA / FSR "native AA"), 1.5, 1.7, 2.
enum class Quality : uint32_t { kNativeAA, kQuality, kBalanced, kPerformance };

Kind ParseKind(const std::string& name);  // "dlss", "fsr", "xess", anything else -> kNone
const char* KindName(Kind kind);
float QualityRatio(Quality quality);      // output / render per axis

// ---- Jitter (both SDKs' recommended sequence: Halton(2, 3)) ----
struct Jitter {
  float x = 0.0f, y = 0.0f;
};
// Radical inverse of `index` (1-based) in `base`: Halton(2) = 0.5, 0.25, 0.75, 0.125, ...
float Halton(uint32_t index, uint32_t base);
// Sequence length: ceil(8 * (output / render)^2) per FSR's ffxFsr3GetJitterPhaseCount (8 at native resolution).
uint32_t JitterPhaseCount(uint32_t render_width, uint32_t output_width);
// The frame's offset: Halton(2, 3) at (frame % phases) + 1, minus 0.5.
Jitter JitterOffset(uint64_t frame, uint32_t phase_count);

// ---- De-jittered depth resolves (Gears 3 job 058, --gow2_upscale_dejitter_depth) ----
// With jitter on, the scene is drawn shifted by j (host pixels, +y down, the viewport shift of the frame's scene
// draws), so depth texel q holds the surface at the unjittered position q - j. A game that rebuilds positions from a
// resolved depth texture and an unjittered screen position (UE3's shadow projection, fog, AO, DOF) is then j pixels
// off, which flips whole triangles into self-shadow on close-ups. The resolve instead writes, for texel q, the depth at
// q + j: a bilinear resample of the 4 texels around it, or the nearest of them across an edge (no in-between depths).
// The same arithmetic as kDepthResolveCs (builtin_shaders.h) - keep the two identical.
//   taps: depth (guest convention) at (i, j), (i+1, j), (i, j+1), (i+1, j+1); fx, fy: the position's fraction in that
//   quad. depth_a: the projection's depth at infinity (depth = a - b / w; DepthAtInfinity), so linear depth is
//   proportional to 1 / |a - depth|. edge: the largest spread of the 4 taps' linear depths, as a fraction of the
//   nearest one's, that is still one surface. *nearest_tap: -1 (bilinear) or the tap used (0..3).
//   (DepthAtInfinity is declared after Mat4 below.)
float DejitterDepth(const float taps[4], float fx, float fy, float depth_a, float edge, int* nearest_tap = nullptr);
// The edge fraction kDepthResolveCs uses (a literal there).
constexpr float kDejitterDepthEdge = 0.05f;

// ---- Matrices for camera motion ----
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // row-major
  float operator()(int r, int c) const { return m[r * 4 + c]; }
  float& operator()(int r, int c) { return m[r * 4 + c]; }
};
Mat4 Multiply(const Mat4& a, const Mat4& b);   // a x b (b applied first)
bool Invert(const Mat4& a, Mat4& out);         // false when singular
// Reads four float constants (c[i], i = first..first+3; 4 floats each, host order) as rows.
Mat4 FromConstants(const float* constants, uint32_t first);
// The same four constants as columns (clip = x*c[first] + y*c[first+1] + z*c[first+2] + w*c[first+3]).
Mat4 FromConstantsTransposed(const float* constants, uint32_t first);
// a in depth = a - b / w, from a camera matrix read as columns (FromConstantsTransposed's convention: clip z and w are
// the matrix's rows 2 and 3): the ratio of their x / y / z coefficients (the same for all three in a perspective
// matrix). 1 for a matrix that doesn't look like that.
float DepthAtInfinity(const Mat4& view_proj);
// Camera parameters FSR wants, read off a perspective matrix (our convention): vertical field of view (radians) from
// the y row's length, and the near plane assuming an infinite far plane (clip z = d - near, clip w = d). False when
// the matrix doesn't look like that.
bool PerspectiveParams(const Mat4& view_proj, float& vertical_fov, float& near_plane, bool& infinite_far);
// The matrix the motion pass uses: (u, v, depth, 1) of a pixel of this frame (u, v in [0, 1], depth guest
// convention times `depth_scale`, which undoes a host-side remap) -> the previous frame's clip position.
// = prev_view_proj x inverse(view_proj) x (uv / depth -> NDC). False when view_proj is singular.
bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, float depth_scale, Mat4& out);
// The same for a game that renders camera-relative (UE3, job 253: its view-projection has no translation; positions
// are given relative to the camera): `camera_move` = this frame's camera position minus last frame's, in world units.
// = prev_view_proj x translate(camera_move) x inverse(view_proj) x (uv / depth -> NDC).
bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, const float camera_move[3], float depth_scale,
                        Mat4& out);
// How far the camera moved, read off the objects drawn (camera-relative rendering): each sample is one draw's
// object-to-world translation (relative to the camera) under a key that names the object (its vertex data, shader
// and rotation rows). A static object's translation changes by minus the camera's move, so the move most matched
// objects agree on (within `tolerance` world units) wins; moving objects disagree and lose. Keys seen more than once
// in a frame are ignored. True when at least `min_votes` objects, and at least `min_share` of the matched ones,
// agree; `camera_move` is then their mean, `votes` / `matched` how many agreed / were matched.
struct ObjectSample {
  uint64_t key = 0;
  float t[3] = {};
};
bool CameraMoveFromObjects(std::vector<ObjectSample> prev, std::vector<ObjectSample> cur, float camera_move[3],
                           uint32_t* votes = nullptr, uint32_t* matched = nullptr, float tolerance = 0.05f,
                           uint32_t min_votes = 3, float min_share = 0.3f);
// What the shader computes for one pixel, for tests and diagnostics: motion in pixels of a width x height image.
// False (motion 0) when the previous position is behind the camera or not finite.
bool CameraMotion(const Mat4& reprojection, float u, float v, float depth, uint32_t width, uint32_t height,
                  float& motion_x, float& motion_y);

// ---- The upscaler ----
struct FrameInputs {
  gpu::TextureHandle color = 0;   // scene colour (render size), sampled
  gpu::TextureHandle depth = 0;   // depth (render size), sampled
  gpu::TextureHandle motion = 0;  // R16G16 float, render-size pixels (conventions above)
  gpu::TextureHandle output = 0;  // output size, storage (UAV)
  uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
  Jitter jitter;
  bool reset = false;             // camera cut / first frame: no history
  bool hdr = true;                // colour is linear HDR (before tone mapping)
  float pre_exposure = 1.0f;      // the factor the colour already carries (Xenos exp bias)
  float frame_ms = 16.7f;
  float camera_near = 1.0f, camera_far = 100000.0f, vertical_fov = 1.0f;  // radians; FSR uses them for depth
  bool depth_inverted = false;
  float sharpness = 0.0f;         // 0..1, the SDKs' own sharpening (0 = off)
};

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  virtual Kind kind() const = 0;
  // (Re)creates the SDK's feature for these sizes. Cheap when nothing changed. False: not available on this GPU /
  // driver (Status() says why) - the renderer then keeps its own path.
  // depth_infinite: the projection has no far plane (depth -> 1 at infinity); FSR needs to know.
  virtual bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                         uint32_t output_height, bool hdr, bool depth_infinite = false) = 0;
  // Records the upscale into the backend's command list (Backend::BeginNative / NativeTexture / EndNative).
  virtual bool Evaluate(gpu::Backend& backend, const FrameInputs& inputs) = 0;
  // A line for the log: SDK / runtime version, feature state, or the reason it is off.
  virtual std::string Status() const = 0;
};

// Null when that SDK was not built in (or kind == kNone).
std::unique_ptr<Upscaler> CreateUpscaler(Kind kind);
bool BuiltIn(Kind kind);

}  // namespace xenos::renderer::upscale
