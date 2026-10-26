// Layer 0 (game-free): the SDK-independent part of the upscaler interface (upscaler.h, notes/64) - names, the
// jitter sequence, the camera-motion matrices - and the factory. Our code (MIT).
#include "xenos/renderer/upscaler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace xenos::renderer::upscale {

#if defined(GN_HAVE_DLSS)
std::unique_ptr<Upscaler> CreateDlssUpscaler();  // dlss_upscaler.cpp
#endif
#if defined(GN_HAVE_FSR)
std::unique_ptr<Upscaler> CreateFsrUpscaler();   // fsr_upscaler.cpp
#endif
#if defined(GN_HAVE_XESS)
std::unique_ptr<Upscaler> CreateXessUpscaler();  // xess_upscaler.cpp
#endif

Kind ParseKind(const std::string& name) {
  if (name == "dlss") return Kind::kDlss;
  if (name == "fsr") return Kind::kFsr;
  if (name == "xess") return Kind::kXess;
  return Kind::kNone;
}

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kDlss: return "DLSS";
    case Kind::kFsr: return "FSR";
    case Kind::kXess: return "XeSS";
    default: return "none";
  }
}

float QualityRatio(Quality quality) {
  switch (quality) {
    case Quality::kQuality: return 1.5f;
    case Quality::kBalanced: return 1.7f;
    case Quality::kPerformance: return 2.0f;
    default: return 1.0f;
  }
}

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

uint32_t JitterPhaseCount(uint32_t render_width, uint32_t output_width) {
  if (!render_width || !output_width) return 8;
  const double ratio = double(output_width) / double(render_width);
  return uint32_t(std::ceil(8.0 * ratio * ratio));
}

Jitter JitterOffset(uint64_t frame, uint32_t phase_count) {
  if (!phase_count) phase_count = 8;
  const uint32_t i = uint32_t(frame % phase_count) + 1;
  return {Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f};
}

float DejitterDepth(const float taps[4], float fx, float fy, float depth_a, float edge, int* nearest_tap) {
  float lin_min = 0.0f, lin_max = 0.0f;
  for (int i = 0; i < 4; ++i) {
    const float l = 1.0f / std::max(std::fabs(depth_a - taps[i]), 1e-7f);
    lin_min = i ? std::min(lin_min, l) : l;
    lin_max = i ? std::max(lin_max, l) : l;
  }
  if (lin_max - lin_min <= edge * lin_min) {
    if (nearest_tap) *nearest_tap = -1;
    const float top = taps[0] + (taps[1] - taps[0]) * fx, bottom = taps[2] + (taps[3] - taps[2]) * fx;
    return top + (bottom - top) * fy;
  }
  const int t = (fx >= 0.5f ? 1 : 0) + (fy >= 0.5f ? 2 : 0);
  if (nearest_tap) *nearest_tap = t;
  return taps[t];
}

float DepthAtInfinity(const Mat4& m) {
  int best = -1;
  float best_w = 0.0f;
  for (int c = 0; c < 3; ++c)
    if (std::fabs(m(3, c)) > best_w) best_w = std::fabs(m(3, c)), best = c;
  if (best < 0 || best_w < 1e-12f) return 1.0f;
  const float a = m(2, best) / m(3, best);
  return std::isfinite(a) ? a : 1.0f;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 o;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      for (int k = 0; k < 4; ++k) s += double(a(r, k)) * double(b(k, c));
      o(r, c) = float(s);
    }
  return o;
}

bool Invert(const Mat4& a, Mat4& out) {
  // Gauss-Jordan with partial pivoting, in double.
  double m[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? double(a(r, c)) : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
    if (std::fabs(m[p][c]) < 1e-30) return false;
    if (p != c)
      for (int k = 0; k < 8; ++k) std::swap(m[p][k], m[c][k]);
    const double inv = 1.0 / m[c][c];
    for (int k = 0; k < 8; ++k) m[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c || m[r][c] == 0.0) continue;
      const double f = m[r][c];
      for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out(r, c) = float(m[r][c + 4]);
  return true;
}

Mat4 FromConstants(const float* constants, uint32_t first) {
  Mat4 o;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) o(r, c) = constants[(first + uint32_t(r)) * 4 + uint32_t(c)];
  return o;
}

Mat4 FromConstantsTransposed(const float* constants, uint32_t first) {
  Mat4 o;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) o(r, c) = constants[(first + uint32_t(c)) * 4 + uint32_t(r)];
  return o;
}

bool PerspectiveParams(const Mat4& m, float& vertical_fov, float& near_plane, bool& infinite_far) {
  auto len3 = [&](int r) { return std::sqrt(double(m(r, 0)) * m(r, 0) + double(m(r, 1)) * m(r, 1) + double(m(r, 2)) * m(r, 2)); };
  const double ys = len3(1), zs = len3(2), ws = len3(3);
  if (ys < 1e-6 || ws < 1e-6) return false;
  vertical_fov = float(2.0 * std::atan(ws / ys));
  // clip z = a * d + b, clip w = d (both per unit of the w row): depth = a + b / d. Infinite far: a == 1 (depth -> 1).
  const double a = zs / ws, b = m(2, 3) - m(3, 3) * a;
  infinite_far = std::fabs(a - 1.0) < 1e-2;  // UE3 (job 252): a = 0.999 - depth tends to 0.999, never 1
  near_plane = float(std::fabs(b) / ws);
  return near_plane > 0.0f;
}

bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, float depth_scale, Mat4& out) {
  Mat4 inv;
  if (!Invert(view_proj, inv)) return false;
  // (u, v, depth, 1) -> NDC (D3D: x right, y up, z 0..1): x = 2u - 1, y = 1 - 2v, z = depth * scale.
  Mat4 ndc;
  ndc(0, 0) = 2.0f;  ndc(0, 3) = -1.0f;
  ndc(1, 1) = -2.0f; ndc(1, 3) = 1.0f;
  ndc(2, 2) = depth_scale;
  out = Multiply(prev_view_proj, Multiply(inv, ndc));
  return true;
}

bool ReprojectionMatrix(const Mat4& view_proj, const Mat4& prev_view_proj, const float camera_move[3], float depth_scale,
                        Mat4& out) {
  Mat4 t;  // this frame's camera-relative position -> last frame's: + (camera now - camera then)
  t(0, 3) = camera_move[0];
  t(1, 3) = camera_move[1];
  t(2, 3) = camera_move[2];
  return ReprojectionMatrix(view_proj, Multiply(prev_view_proj, t), depth_scale, out);
}

namespace {
// Sorted by key, keys that occur more than once removed.
void UniqueByKey(std::vector<ObjectSample>& v) {
  std::sort(v.begin(), v.end(), [](const ObjectSample& a, const ObjectSample& b) { return a.key < b.key; });
  std::vector<ObjectSample> o;
  o.reserve(v.size());
  for (size_t i = 0; i < v.size();) {
    size_t j = i + 1;
    while (j < v.size() && v[j].key == v[i].key) ++j;
    if (j == i + 1) o.push_back(v[i]);
    i = j;
  }
  v.swap(o);
}
}  // namespace

bool CameraMoveFromObjects(std::vector<ObjectSample> prev, std::vector<ObjectSample> cur, float camera_move[3],
                           uint32_t* votes, uint32_t* matched, float tolerance, uint32_t min_votes, float min_share) {
  camera_move[0] = camera_move[1] = camera_move[2] = 0.0f;
  if (votes) *votes = 0;
  if (matched) *matched = 0;
  UniqueByKey(prev);
  UniqueByKey(cur);
  struct D { float d[3]; };
  std::vector<D> moves;  // camera move each matched object implies: -(translation now - translation then)
  for (size_t i = 0, j = 0; i < prev.size() && j < cur.size();) {
    if (prev[i].key < cur[j].key) { ++i; continue; }
    if (cur[j].key < prev[i].key) { ++j; continue; }
    D d;
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      d.d[k] = -(cur[j].t[k] - prev[i].t[k]);
      ok = ok && std::isfinite(d.d[k]);
    }
    if (ok) moves.push_back(d);
    ++i, ++j;
  }
  if (matched) *matched = uint32_t(moves.size());
  if (moves.empty()) return false;
  auto close = [&](const D& a, const D& b) {
    return std::fabs(a.d[0] - b.d[0]) <= tolerance && std::fabs(a.d[1] - b.d[1]) <= tolerance &&
           std::fabs(a.d[2] - b.d[2]) <= tolerance;
  };
  // The candidate most others agree with (up to 256 candidates spread over the list; ties: the smaller move).
  const size_t step = std::max<size_t>(1, moves.size() / 256);
  size_t best = 0, best_count = 0;
  float best_len = 0.0f;
  for (size_t i = 0; i < moves.size(); i += step) {
    size_t n = 0;
    for (const D& m : moves) n += close(moves[i], m) ? 1 : 0;
    const float len = std::fabs(moves[i].d[0]) + std::fabs(moves[i].d[1]) + std::fabs(moves[i].d[2]);
    if (n > best_count || (n == best_count && len < best_len)) best = i, best_count = n, best_len = len;
  }
  if (votes) *votes = uint32_t(best_count);
  if (best_count < min_votes || float(best_count) < min_share * float(moves.size())) return false;
  double sum[3] = {};
  for (const D& m : moves)
    if (close(moves[best], m))
      for (int k = 0; k < 3; ++k) sum[k] += m.d[k];
  for (int k = 0; k < 3; ++k) camera_move[k] = float(sum[k] / double(best_count));
  return true;
}

bool CameraMotion(const Mat4& reprojection, float u, float v, float depth, uint32_t width, uint32_t height,
                  float& motion_x, float& motion_y) {
  motion_x = motion_y = 0.0f;
  const float in[4] = {u, v, depth, 1.0f};
  float p[4];
  for (int r = 0; r < 4; ++r) p[r] = reprojection(r, 0) * in[0] + reprojection(r, 1) * in[1] +
                                     reprojection(r, 2) * in[2] + reprojection(r, 3) * in[3];
  if (!(p[3] > 1e-6f)) return false;  // behind the previous camera (or NaN)
  const float pu = (p[0] / p[3] + 1.0f) * 0.5f, pv = (1.0f - p[1] / p[3]) * 0.5f;
  if (!std::isfinite(pu) || !std::isfinite(pv)) return false;
  motion_x = (pu - u) * float(width);
  motion_y = (pv - v) * float(height);
  return true;
}

std::unique_ptr<Upscaler> CreateUpscaler(Kind kind) {
#if defined(GN_HAVE_DLSS)
  if (kind == Kind::kDlss) return CreateDlssUpscaler();
#endif
#if defined(GN_HAVE_FSR)
  if (kind == Kind::kFsr) return CreateFsrUpscaler();
#endif
#if defined(GN_HAVE_XESS)
  if (kind == Kind::kXess) return CreateXessUpscaler();
#endif
  (void)kind;
  return nullptr;
}

bool BuiltIn(Kind kind) {
#if defined(GN_HAVE_DLSS)
  if (kind == Kind::kDlss) return true;
#endif
#if defined(GN_HAVE_FSR)
  if (kind == Kind::kFsr) return true;
#endif
#if defined(GN_HAVE_XESS)
  if (kind == Kind::kXess) return true;
#endif
  (void)kind;
  return false;
}

}  // namespace xenos::renderer::upscale
