// Layer 0 (game-free): temporal upscalers (NVIDIA DLSS, AMD FSR) behind one small interface (notes/64,
// XeGTAO ambient occlusion, upscaler motion-vector / depth fixes and SMAA T2x: PR #139 by gmihaic (https://github.com/gmihaic).
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
// The SDK wrappers (dlss_upscaler.cpp, fsr_upscaler.cpp) are built only when their SDK is in
// third_party (GN_HAVE_DLSS, GN_HAVE_FSR); without one, CreateUpscaler returns null and the launcher's
// choice falls back to SMAA / off.
// Our code (MIT). The SDKs keep their own licences (THIRD_PARTY_NOTICES.md).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenos/renderer/gpu.h"

namespace xenos::renderer::upscale {

// kSmaaT2x (notes/80 s.7): ours, no SDK - SMAA 1x with two alternating jitters and a temporal resolve, at the
// render size (no upscaling); made by the renderer (it needs the shader cache), not by CreateUpscaler.
enum class Kind : uint32_t { kNone, kDlss, kFsr, kSmaaT2x };
// Render size / output size: 1 (native-resolution AA: DLAA / FSR "native AA"), 1.5, 1.7, 2.
enum class Quality : uint32_t { kNativeAA, kQuality, kBalanced, kPerformance };

Kind ParseKind(const std::string& name);  // "dlss", "fsr", "smaa_t2x", anything else -> kNone
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
// depth_map_scale / depth_map_offset (notes/80 s.6): the viewport's Z transform, depth = scale * (clip z / w) + offset
// (ao::ViewportDepthMap); clip z / w = (depth - offset) / scale. 1 / 0 for a plain 0..1 range; Gears 2 draws reversed-Z
// (-1 / 1), which the matrix must undo or every pixel looks a few units from the camera and camera moves blow up.
bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, float depth_scale, Mat4& out,
                        float depth_map_scale = 1.0f, float depth_map_offset = 0.0f);
// The same for a game that renders camera-relative (UE3, job 253: its view-projection has no translation; positions
// are given relative to the camera): `camera_move` = this frame's camera position minus last frame's, in world units.
// = prev_view_proj x translate(camera_move) x inverse(view_proj) x (uv / depth -> NDC).
bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, const float camera_move[3], float depth_scale,
                        Mat4& out, float depth_map_scale = 1.0f, float depth_map_offset = 0.0f);
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
// Notes/80 s.11: which of a frame's full-screen scene copies the upscaler takes. Off: the first, each time asked (the
// caller stops after one). last_only: the one at the position of the last frame's last copy - the frame's end can't be
// seen in advance, but the count barely changes. Before any count is known the first is taken; a frame with fewer copies
// takes none, and the next has learned. Frames without copies (movies) keep the count.
struct SceneCopyPicker {
  bool last_only = false;
  uint32_t count = 0, last_count = 0;
  // With last_only, the scene's format - the format of a frame's last copy: copies of another format are not counted,
  // so a frame that adds or drops a mid-frame copy of another format (Judgment, Blood Drive: a 16-bit float copy before
  // the 10:10:10:2 scene, every other frame) still takes the same copy. Taking the other one flipped the upscaler's
  // HDR flag and re-created it (~33 ms a time). After kFormatPatience frames in a row whose last copy (taken or
  // skipped) has another format, it learns again: also when every frame still has a copy of the learned format, as
  // when it learned on a frame that only had the mid-frame copy (a menu, a transition, match start).
  static constexpr uint32_t kFormatPatience = 60;
  uint32_t format = 0, other_frames = 0;
  // anchored (Judgment): the scene copy comes after an anchor pass (the fog pass). Judgment adds a mid-frame scene copy
  // before its fog pass on some frames only (what is visible decides); counted, it moved the taken copy to before the
  // fog - the AO that goes in at the fog pass was then left out of that frame (a one-frame flash) and the upscalers got
  // the scene without its last passes. Once a frame has drawn the anchor with a copy after it, copies before the anchor
  // are neither taken nor counted. A frame without the anchor takes no copy (whether one comes later can't be known at
  // the copy); after kNoAnchorFrames such frames in a row (a pause or menu over the world) it lets go, as it does after
  // kAnchorPatience frames that draw the anchor with no copy after it, and every copy counts again. The count starts
  // over at each switch, so the two ways of counting never mix.
  bool anchored = false;
  static constexpr uint32_t kAnchorPatience = 60, kNoAnchorFrames = 2;
  bool Take(uint32_t copy_format = 0, bool after_anchor = true) {
    if (anchored && after_anchor) ++after_anchor_copies_;
    if (anchored && anchor_active_ && !after_anchor) return false;
    if (last_only && copy_format) frame_last_ = copy_format;   // the frame's last copy of any format
    if (last_only && format && copy_format && copy_format != format) return false;
    const uint32_t index = count++;
    if (last_only && !format && copy_format) {   // learning: the frame's copies counted by format, the last one decides
      uint32_t i = 0;
      while (i < n_formats_ && formats_[i] != copy_format) ++i;
      if (i == n_formats_ && n_formats_ < kMaxFormats) formats_[n_formats_++] = copy_format;
      if (i < n_formats_) ++format_counts_[i];
    }
    return !last_only || last_count == 0 || index + 1 >= last_count;
  }
  void EndFrame(bool anchor_seen = true) {
    bool relearn = false;
    if (last_only && format && frame_last_) {   // a frame with no copies at all says nothing
      if (frame_last_ != format) relearn = ++other_frames >= kFormatPatience;
      else other_frames = 0;
    }
    if (relearn) {   // the scene now has another format: learn it from the next frame
      format = 0;
      last_count = 0;
      other_frames = 0;
    } else if (count) {
      last_count = count;
      if (last_only && !format && frame_last_) {   // learnt: count only that format's copies from now on
        format = frame_last_;
        last_count = 0;
        for (uint32_t i = 0; i < n_formats_; ++i)
          if (formats_[i] == format) last_count = format_counts_[i];
      }
    }
    if (anchored) {
      const bool good = anchor_seen && after_anchor_copies_;
      frames_without_anchor_ = good ? 0 : frames_without_anchor_ + 1;   // no copy after the anchor
      frames_no_anchor_ = anchor_seen ? 0 : frames_no_anchor_ + 1;      // no anchor at all
      const bool active = frames_without_anchor_ < kAnchorPatience && frames_no_anchor_ < kNoAnchorFrames;
      if (active != anchor_active_) {
        anchor_active_ = active;
        last_count = 0;   // learnt again in the new way of counting
      }
    }
    after_anchor_copies_ = 0;
    count = 0;
    frame_last_ = 0;
    n_formats_ = 0;
    for (uint32_t& c : format_counts_) c = 0;
  }

 private:
  static constexpr uint32_t kMaxFormats = 16;   // distinct formats in one frame (a frame has two in practice)
  uint32_t frame_last_ = 0, n_formats_ = 0;
  bool anchor_active_ = false;
  uint32_t frames_without_anchor_ = kAnchorPatience, frames_no_anchor_ = 0, after_anchor_copies_ = 0;
  uint32_t formats_[kMaxFormats] = {}, format_counts_[kMaxFormats] = {};
};

// Notes/80 s.6 (Gears 2): one character of the game's velocity pass. UE3 gives its VS this frame's object-to-world
// relative to the camera (`now`, c5-c8, as columns: world = x*c5 + y*c6 + z*c7 + c8) and last frame's in absolute world
// coordinates (`then`, c9-c12). `key` names the draw across frames (vertex data, shader, index count).
struct MovingObject {
  uint64_t key = 0;
  Mat4 now, then;
};
// Last frame's camera position (world), from objects drawn in both frames: an object's absolute position last frame
// (this frame's `then`) minus its camera-relative position last frame (last frame's `now`). The median of all matched
// objects; true when at least `min_agree` of them lie within `tolerance` of it (an animated part's origin may move).
bool PreviousCameraFromObjects(const std::vector<MovingObject>& prev, const std::vector<MovingObject>& cur,
                               float camera[3], uint32_t min_agree = 1, float tolerance = 0.5f);
// The reprojection matrix (as ReprojectionMatrix) for points on one object: a point's camera-relative position now ->
// the object's local position -> its world position last frame -> relative to last frame's camera -> last frame's clip.
// = prev_view_proj x translate(-prev_camera) x then x inverse(now) x inverse(view_proj) x (uv / depth -> NDC).
bool ObjectReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, const MovingObject& object,
                              const float prev_camera[3], float depth_scale, Mat4& out, float depth_map_scale = 1.0f,
                              float depth_map_offset = 0.0f);
// What the shader computes for one pixel, for tests and diagnostics: motion in pixels of a width x height image.
// False (motion 0) when the previous position is behind the camera or not finite.
bool CameraMotion(const Mat4& reprojection, float u, float v, float depth, uint32_t width, uint32_t height,
                  float& motion_x, float& motion_y);

// ---- GitHub #323: a scene whose size changes from frame to frame ----
// Gears 2 narrows its scene by itself in heavy views (1024..1280 x 720 guest pixels, 64-pixel steps, many times a
// second). Re-creating an SDK feature for every size cost ~21 ms each (DLSS), so the feature is created once for a
// size the frames fit in, and each frame is handed over within it (ResizeMode).
// The guest size to create for: keeps cw x ch (0 x 0: none yet - then the frame's own size, so a game whose scene is
// always smaller than nominal is never padded) while w x h fits in it; an axis the frame exceeds grows to at least the
// frame and at least the nominal (the game's full scene, 1280 x 720), so a game that started narrower re-creates once
// more, not at every step. Never shrinks by itself (UpdateCreationSize below does, slowly). True when it changed, i.e.
// the feature must be re-created.
bool FitCreationSize(uint32_t w, uint32_t h, uint32_t nominal_w, uint32_t nominal_h, uint32_t& cw, uint32_t& ch);
// FitCreationSize plus a slow shrink (r83 L2): one taller or wider frame (a 720-high scene in an ultrawide 1280 x 550
// session, say) must not pin a larger feature, and the padding it needs, for the rest of the session. Hysteresis both
// ways, so the size cannot oscillate:
// - a shrink needs `shrink_after` frames in a row that all fit a box at least 1/8 smaller in area (Gears 2's 1216-wide
//   steps never qualify, and one full-size frame starts the count again); the box is the largest of those frames;
// - a grow that undoes a shrink doubles the frames the next shrink needs (up to 2^24), so a scene that keeps coming
//   back to the larger size re-creates a few times at most, ever further apart.
struct CreationSize {
  uint32_t w = 0, h = 0;          // the guest size created for (0 x 0: none yet)
  uint32_t run_w = 0, run_h = 0;  // the box the frames of the current run fit in (what a shrink would make)
  uint32_t run = 0;               // frames in the run
  uint32_t wait = 0;              // frames a shrink needs (0: shrink_after)
  bool shrunk = false;            // the last change was a shrink
};
enum class CreationChange : uint32_t { kNone, kGrow, kShrink };  // kGrow includes the first frame's
// shrink_after = 0: never shrinks (FitCreationSize alone).
CreationChange UpdateCreationSize(CreationSize& s, uint32_t w, uint32_t h, uint32_t nominal_w, uint32_t nominal_h,
                                  uint32_t shrink_after);
// The motion (render pixels, previous - current, one axis) of the pixel whose centre is at `pos` in a frame `size`
// pixels across, for an upscaler whose history is the last frame drawn `prev_size` across in the same pixel grid
// (left / top aligned: the game draws the same view narrower, it does not crop it): the previous position
// u_prev * size becomes u_prev * prev_size, i.e. motion * k + pos * (k - 1) with k = prev_size / size.
// kUpscalePadCs (builtin_shaders.h) does the same on the GPU - keep the two identical.
float ResizedMotion(float motion, float pos, uint32_t size, uint32_t prev_size);

enum class ResizeMode : uint32_t {
  kRecreate,  // Configure with every frame's size (ours: SMAA T2x - its textures are cheap to remake)
  kPerFrame,  // FSR without the padding shader: created for the largest size; renderSize / upscaleSize per dispatch
              // below it (history reset on a size change)
  kPadded,    // DLSS, FSR: created once, always given the creation size (NGX takes a render subrect, but its
              // output always fills the target; FSR 4 / older FSR 3.1 providers are not known to honour
              // an upscaleSize below the maximum - r83 M1), so the renderer pads the frame's colour / depth / motion to
              // the creation size (edges repeated) and reads its own part of the output - the same pixels at the same
              // place as before
};

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
  // depth_inverted (notes/80 s.6): the depth input is reversed - 1 at the near plane, toward 0 far away (Gears 2's
  // viewport draws reversed-Z); every SDK has a creation flag for it.
  virtual bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                         uint32_t output_height, bool hdr, bool depth_infinite = false, bool depth_inverted = false) = 0;
  // Records the upscale into the backend's command list (Backend::BeginNative / NativeTexture / EndNative).
  virtual bool Evaluate(gpu::Backend& backend, const FrameInputs& inputs) = 0;
  // A line for the log: SDK / runtime version, feature state, or the reason it is off.
  virtual std::string Status() const = 0;
  // GitHub #323: how a frame smaller than the Configure size is handed over (ResizeMode above).
  virtual ResizeMode Resize() const { return ResizeMode::kRecreate; }
  // What kPadded falls back to when the padding shader or its textures are not available: kRecreate, or kPerFrame for
  // an SDK that takes a smaller size per dispatch (FSR).
  virtual ResizeMode ResizeFallback() const { return ResizeMode::kRecreate; }
};

// Null when that SDK was not built in (or kind == kNone).
std::unique_ptr<Upscaler> CreateUpscaler(Kind kind);
bool BuiltIn(Kind kind);

}  // namespace xenos::renderer::upscale
