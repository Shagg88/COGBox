// UE3 scene-grade bridge, informed by RenoDX's Batman AK, Dishonored and BL2.
// Oklab matrices adapted from RenoDX: Copyright (c) 2025 Carlos Lopez Jr.
// SPDX-License-Identifier: MIT. See cogbox/licenses/RenoDX-Grading-LICENSE.txt.
#pragma once

namespace xenos::renderer::native_hdr {
inline constexpr const char* kUe3GradeHlsl = R"COG_UE3(
// Oklab conversions adapted from RenoDX, Copyright (c) 2025 Carlos Lopez Jr.
// SPDX-License-Identifier: MIT; distributed with RenoDX-Grading-LICENSE.txt.
float3 NativeNormalizeScene(float3 weightedColor, float normalizationReciprocal, float3 blackOffset,
                            float3 fallbackScene = 0.0) {
  // Missing motion/blur coverage can leave a zero normalization weight at an
  // image edge. Its reciprocal is infinity. This is missing scene data, not
  // HDR lighting: use the already sampled, unblurred scene at that pixel.
  float3 fallback = max(fallbackScene - blackOffset, 0.0);
  if (!(normalizationReciprocal > 0.0) || !isfinite(normalizationReciprocal)) return fallback;
  float3 normalized = weightedColor * normalizationReciprocal - blackOffset;
  return all(isfinite(normalized)) ? max(normalized, 0.0) : fallback;
}

float3 NativeOklab(float3 color) {
  float3 lms = mul(float3x3(
      0.4122214708, 0.5363325363, 0.0514459929,
      0.2119034982, 0.6806995451, 0.1073969566,
      0.0883024619, 0.2817188376, 0.6299787005), max(color, 0.0));
  lms = pow(max(lms, 0.0), 1.0 / 3.0);
  return mul(float3x3(
      0.2104542553, 0.7936177850, -0.0040720468,
      1.9779984951, -2.4285922050, 0.4505937099,
      0.0259040371, 0.7827717662, -0.8086757660), lms);
}

float3 NativeFromOklab(float3 color) {
  float3 lms = mul(float3x3(
      1.0, 0.3963377774, 0.2158037573,
      1.0, -0.1055613458, -0.0638541728,
      1.0, -0.0894841775, -1.2914855480), color);
  return mul(float3x3(
      4.0767416621, -3.3077115913, 0.2309699292,
      -1.2684380046, 2.6097574011, -0.3413193965,
      -0.0041960863, -0.7034186147, 1.7076147010), lms * lms * lms);
}

float3 NativeFitHdr(float3 color, float peak) {
  // Contract chroma toward a grey of equal luminance before the peak fit.
  // Avoid per-channel negative clipping, which changes saturated hues.
  float y = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0.0);
  float minimum = min(color.r, min(color.g, color.b));
  if (minimum < 0.0) color = y + (color - y) * (y / max(y - minimum, 1e-8));
  color = max(color, 0.0);
  return color * (peak / max(peak, max(color.r, max(color.g, color.b))));
}

float3 NativeUe3Hue(float3 color, float3 vanilla, float gradeChroma = 0.0) {
  float3 reference = NativeOklab(vanilla);
  float radius = length(reference.yz);
  // An achromatic/clipped reference carries no reliable hue information.
  if (radius <= 1e-4 * max(reference.x, 1e-6)) return color;
  float3 corrected = NativeOklab(color);
  // Relative Oklab chroma C/L follows the SDR grade at the HDR lightness.
  // Restoring hue alone leaves the mapper's desaturated highlights too white.
  float chroma = length(corrected.yz);
  if (gradeChroma > 0.0) {
    float referenceChroma = radius * corrected.x / max(reference.x, 1e-6);
    chroma = lerp(chroma, referenceChroma, gradeChroma);
  }
  corrected.yz = reference.yz * (chroma / radius);
  float3 result = NativeFromOklab(corrected);
  if (any(result < 0.0)) {
    // A hue rotation can leave BT.709. Reduce Oklab chroma along that same
    // hue, rather than clipping RGB or changing the artist's yellow/orange.
    float lower = 0.0, upper = 1.0;
    [unroll] for (int i = 0; i < 5; ++i) {
      float middle = (lower + upper) * 0.5;
      float3 trial = NativeFromOklab(float3(corrected.x, corrected.yz * middle));
      if (all(trial >= 0.0)) lower = middle;
      else upper = middle;
    }
    result = NativeFromOklab(float3(corrected.x, corrected.yz * lower));
  }
  result = max(result, 0.0);
  if (gradeChroma > 0.0) {
    // Chroma changes at constant Oklab L can change linear luminance. Retain
    // the mapped light level here; the final RGB peak fit still takes priority.
    float originalY = dot(color, float3(0.2126, 0.7152, 0.0722));
    float correctedY = dot(result, float3(0.2126, 0.7152, 0.0722));
    result *= originalY / max(correctedY, 1e-8);
  }
  return result;
}

float3 NativeUe3DisplayMap(float3 color, float peak, float inputWhite = 0.0) {
  float anchor = min(0.5, peak * 0.95);
  // The finite input white must exceed the display peak. Mapping a modest
  // input of 3 to a display range of 19 stretched highlights excessively.
  // PsychoV35 remains the only scene display mapper; its source is unchanged.
  float clip = max(100.0, peak * 4.0);
  if (inputWhite > 0.0) clip = max(inputWhite, anchor + 1e-4);
  return renodx::tonemap::psychov::custom_psychotm_test35(
      max(color, 0.0), peak, 1.0, 1.0, 1.0, 1.0, 0.0,
      1.0, 1.0, 1.0, 1.0, 0.0, anchor.xxx, anchor.xxx,
      0.0, 1.0, 0, 1.5, clip);
}

float3 NativeUe3TangentPower(float3 value, float3 exponent, float3 pivot) {
  value = max(value, 0.0);
  pivot = max(pivot, 1e-5);
  float3 atPivot = pow(pivot, exponent);
  float3 slope = atPivot * exponent / pivot;
  // Evaluate pow only on the bounded part, even when the input is FP16-max.
  return pow(min(value, pivot), exponent) + slope * max(value - pivot, 0.0);
}

float3 NativeUe3ContinueLut(float3 boundary, float3 edgeSlope, float3 interiorSlope,
                           float distance, float stepSize) {
  float t = saturate(distance / stepSize);
  // Integral of smoothstep: joins the actual boundary derivative continuously
  // to an earlier useful derivative when the final LUT cell is clipped flat.
  float transition = distance < stepSize ? stepSize * (t*t*t - 0.5*t*t*t*t)
                                        : distance - 0.5 * stepSize;
  return max(boundary + edgeSlope * distance + (interiorSlope - edgeSlope) * transition, 0.0);
}

float3 NativeUe3MapExtendedGrade(float3 scene, float3 extendedGrade,
                                float3 vanillaGrade, float peak, float fade,
                                float gradeChroma = 0.0, float inputWhite = 0.0) {
  vanillaGrade = max(vanillaGrade, 0.0);
  if (all(vanillaGrade == 0.0)) return 0.0;
  float3 mapped = NativeUe3DisplayMap(max(extendedGrade, 0.0) * max(fade, 1.0), peak, inputWhite);
  mapped = NativeUe3Hue(mapped, vanillaGrade, gradeChroma);
  // A concave SDR curve can already sit close to SDR white. Do not blend
  // toward a dimmer display-mapped shoulder and introduce a brightness dip.
  // Scale in linear luminance, preserving the corrected hue/chromaticity.
  float3 reference = vanillaGrade * max(fade, 1.0);
  float referenceY = dot(reference, float3(0.2126, 0.7152, 0.0722));
  float mappedY = dot(mapped, float3(0.2126, 0.7152, 0.0722));
  mapped = mappedY > 1e-8 ? mapped * max(1.0, referenceY / mappedY) : reference;
  // Keep the original diffuse/shadow look, with a continuous transition into
  // the extended scene. The final explicit fade still reaches zero.
  float weight = smoothstep(0.5, 1.0, max(scene.r, max(scene.g, scene.b)));
  float3 color = lerp(reference, mapped, weight);
  return NativeFitHdr(color, peak) * min(fade, 1.0);
}

// Tetrahedral interpolation in the LUT's original encoded domain. Coordinates
// already include the game's centre scale/bias and Xbox sampling adjustment.
float4 NativeUe3Tetrahedral(Texture3D<float4> lut, float3 centeredCoordinate) {
  uint3 size;
  lut.GetDimensions(size.x, size.y, size.z);
  float3 index = clamp(centeredCoordinate * float3(size) - 0.5, 0.0, float3(size - 1u));
  uint3 base = uint3(floor(index));
  uint3 next = min(base + 1u, size - 1u);
  float3 f = index - float3(base);
  uint3 first, second;
  float a, b, c;
  if (f.x >= f.y) {
    if (f.y >= f.z) { first=uint3(next.x,base.y,base.z); second=uint3(next.x,next.y,base.z); a=f.x; b=f.y; c=f.z; }
    else if (f.x >= f.z) { first=uint3(next.x,base.y,base.z); second=uint3(next.x,base.y,next.z); a=f.x; b=f.z; c=f.y; }
    else { first=uint3(base.x,base.y,next.z); second=uint3(next.x,base.y,next.z); a=f.z; b=f.x; c=f.y; }
  } else {
    if (f.x >= f.z) { first=uint3(base.x,next.y,base.z); second=uint3(next.x,next.y,base.z); a=f.y; b=f.x; c=f.z; }
    else if (f.y >= f.z) { first=uint3(base.x,next.y,base.z); second=uint3(base.x,next.y,next.z); a=f.y; b=f.z; c=f.x; }
    else { first=uint3(base.x,base.y,next.z); second=uint3(base.x,next.y,next.z); a=f.z; b=f.y; c=f.x; }
  }
  return lut.Load(int4(base,0)) * (1.0-a) + lut.Load(int4(first,0)) * (a-b) +
         lut.Load(int4(second,0)) * (b-c) + lut.Load(int4(next,0)) * c;
}
)COG_UE3";
}  // namespace xenos::renderer::native_hdr
