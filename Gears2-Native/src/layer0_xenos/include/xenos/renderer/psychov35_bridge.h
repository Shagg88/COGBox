// COGBox's fingerprint-checked UE3 scene/grade integration.
// Supplied PsychoV35 and its MIT notices remain unchanged in psychov35_source.h.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <xenos/renderer/psychov35_source.h>
#include <xenos/renderer/ue3_grade_bridge.h>

namespace xenos::renderer::native_hdr {
inline std::string Psycho35SceneSource(std::string source, uint32_t low, uint32_t high, bool gears3) {
  const auto constant = [=](uint32_t index) {
    if (low != high && index >= high) index = low + index - high;
    return "xe_c[" + std::to_string(index) + "]";
  };
  const auto replace = [&](const std::string& from, const std::string& to) {
    const auto at = source.find(from);
    if (at == std::string::npos || source.find(from, at + from.size()) != std::string::npos) return false;
    source.replace(at, from.size(), to);
    return true;
  };
  // Keep the entire vanilla program for the original grading/hue reference.
  // Extend the game curve/grade in a second, local path before display mapping.
  std::string grade;
  if (gears3) {
    grade = R"(
// Smoothed-power HDR boost adapted from RenoDX's Dishonored/common.hlsl.
// Copyright (c) 2025 Carlos Lopez Jr. / RenoDX contributors, MIT.
// See cogbox/licenses/RenoDX-Grading-LICENSE.txt.
float NativeGears3RenoDxBoost(float value, float power = 0.40) {
  value = max(value, 0.0);
  if (value <= 0.0 || power <= 0.0) return value;
  const float normalization = 0.04;
  const float smoothing = power * 2.0;
  // RenoDX Reinhard(x,p) is p*x/(x+p), not x/(x+p).
  float weight = smoothing * value / (value + smoothing);
  float expanded = normalization * pow(value / normalization, 1.0 + power);
  return max(value, lerp(value, expanded, weight));
}

float3 NativeGears3HdrBoost(float3 color, float power = 0.40, float strength = 1.0) {
  color = max(color, 0.0);
  // Apply the sample's scalar boost to luminance in this shader's linear
  // BT.709 working space; one RGB scale retains the extended LUT's color.
  float y = dot(color, float3(0.2126, 0.7152, 0.0722));
  if (y <= 0.0) return color;
  // COGBox 0.6.2: strength scales the added gain (1 = this tuned boost, 0 = none)
  return color * (1.0 + (NativeGears3RenoDxBoost(y, power) / y - 1.0) * max(strength, 0.0));
}

float3 NativeGears3Neutral(float3 scene) {
  float4 curve = $C4;
  float3 powerCurve = pow(max(scene * abs(curve.w), 0.0), $C252.y);
  float3 rationalCurve = scene * curve.y / max(abs(scene + curve.x), 1e-6);
  float3 weight = saturate((scene - curve.z) * $C253.z);
  float3 blended = lerp(powerCurve, rationalCurve, weight);
  return pow(max(saturate(lerp(blended, rationalCurve, $C5.x)), 0.0), 2.2);
}

float3 NativeGears3ExtendedNeutral(float3 scene) {
  // Join at the protected mid-gray value and its original derivative.
  // Joining far into the filmic shoulder retains its already-flattened slope
  // and suppresses the HDR range before the LUT/boost can carry it onward.
  const float pivot = 0.18;
  float h = max(pivot / 1024.0, 1e-5);
  float3 base = NativeGears3Neutral(pivot.xxx);
  float3 slope = max((NativeGears3Neutral((pivot+h).xxx) -
                     NativeGears3Neutral((pivot-h).xxx)) / (2.0*h), 0.0);
  return NativeGears3Neutral(min(max(scene, 0.0), pivot)) + slope * max(scene - pivot, 0.0);
}

float3 NativeGears3ReadLut(float3 linearInput) {
  float3 encoded = pow(saturate(linearInput), 1.0 / 2.2);
  uint3 size;
  xe_tex_3d_3.GetDimensions(size.x, size.y, size.z);
  // Original gamma-domain centre scale/bias and Xbox fetch adjustment.
  float3 coordinate = encoded * $C252.x + $C253.w + 0.0014648438 / float3(size);
  float4 sampled = NativeUe3Tetrahedral(xe_tex_3d_3, coordinate);
  sampled = xe_tex_post(sampled, 3u);
  return pow(max(sampled.rgb, 0.0), 2.2);
}

float3 NativeGears3ExtendedLut(float3 linearInput) {
  linearInput = max(linearInput, 0.0);
  float3 boundaryInput = saturate(linearInput);
  float3 excess = linearInput - boundaryInput;
  float distance = max(excess.r, max(excess.g, excess.b));
  float3 boundary = NativeGears3ReadLut(boundaryInput);
  if (distance <= 0.0) return boundary;
  uint3 size;
  xe_tex_3d_3.GetDimensions(size.x, size.y, size.z);
  float cell = 1.0 / max(float(max(size.x, max(size.y, size.z))) - 1.0, 1.0);
  // Derivatives are in LINEAR input/output units. An identity gamma-domain
  // LUT must remain identity outside the cube as well as inside it.
  float stepSize = 1.0 - pow(max(1.0-cell, 0.0), 2.2);
  float3 direction = excess / distance;
  float3 previous = NativeGears3ReadLut(boundaryInput - direction * stepSize);
  float3 edgeSlope = (boundary - previous) / stepSize;
  float3 usefulSlope = edgeSlope;
  [unroll] for (int i=2; i<=4; ++i) {
    float3 current = NativeGears3ReadLut(boundaryInput - direction * (stepSize * i));
    float3 candidate = (previous - current) / stepSize;
    usefulSlope = select(abs(usefulSlope) <= 1e-5, candidate, usefulSlope);
    previous = current;
  }
  return NativeUe3ContinueLut(boundary, edgeSlope, usefulSlope, distance, stepSize);
}

float3 NativeGears3ExtendedGrade(float3 scene, float grain) {
  float3 encoded = pow(max(NativeGears3ExtendedNeutral(scene), 0.0), 1.0 / 2.2);
  float3 grainBase = saturate(encoded);
  encoded = max(encoded + grain * ((1.0-grainBase)*(1.0-grainBase)*$C3.w + $C253.x), 0.0);
  return NativeGears3ExtendedLut(pow(encoded, 2.2));
}
)";
  } else {
    grade = R"(
float3 NativeGears2Grade(float3 scene) {
  // Analytic grading, not a LUT: original exposure, contrast, channel mix,
  // black floor and output exponent, evaluated on a valid central sample.
  float3 contrast = pow(max(abs(saturate(scene) * $C6.xyz), $C254.z), $C7.xyz);
  float3 graded = saturate((contrast * $C5.w + $C10.xyz + dot(contrast, $C8.xyz)) * $C9.xyz);
  return pow(max(graded, $C254.z), $C9.w * 2.2);
}

float3 NativeGears2ExtendedGrade(float3 scene) {
  // Continue both analytic power sections with their value and derivative
  // before the two SDR ceilings. The channel mix and black floor stay intact.
  float3 exposure = abs($C6.xyz);
  float3 contrast = NativeUe3TangentPower(max(max(scene,0.0) * exposure, $C254.z),
      $C7.xyz, max(exposure * 0.75, $C254.z));
  float3 mixed = max((contrast*$C5.w + $C10.xyz + dot(contrast,$C8.xyz)) * $C9.xyz, $C254.z);
  return NativeUe3TangentPower(mixed, ($C9.w * 2.2).xxx, 0.9.xxx);
}

// RenoDX Dishonored smoothed-power boost, as used by the Gears 3 profile.
// Copyright (c) 2025 Carlos Lopez Jr. / RenoDX contributors, MIT.
// See cogbox/licenses/RenoDX-Grading-LICENSE.txt.
float NativeGears2RenoDxBoost(float value, float power = 0.40) {
  value = max(value, 0.0);
  if (value <= 0.0 || power <= 0.0) return value;
  const float normalization = 0.04;
  float smoothing = power * 2.0;
  float weight = smoothing * value / (value + smoothing);
  float expanded = normalization * pow(value / normalization, 1.0 + power);
  return max(value, lerp(value, expanded, weight));
}

float3 NativeGears2PeakInput(float3 graded, float peak, float strength = 1.0) {
  graded = max(graded, 0.0);
  float intensity = dot(graded, float3(0.2126, 0.7152, 0.0722));
  if (intensity <= 1.0 || peak <= 1.0) return graded;
  // Adapt the same RenoDX boost to Gears 2's analytic grade: preserve SDR
  // white and its derivative, then smoothly bound added gain. This retains
  // the earlier flash protection instead of restoring peak-proportional gain.
  float requestedGain = NativeGears2RenoDxBoost(intensity) / intensity;
  float delta = max(requestedGain - 1.0, 0.0);
  float maximumGain = min(max(peak, 1.0), 4.0);
  float headroom = maximumGain - 1.0;
  float boundedDelta = headroom * delta / (headroom + delta);
  // COGBox 0.6.2: strength scales the added gain (1 = this tuned boost, 0 = none)
  float gain = 1.0 + smoothstep(1.0, 4.0, intensity) * boundedDelta * max(strength, 0.0);
  return graded * gain;
}
)";
  }
  for (const auto index : {252u, 253u, 254u, 10u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
    const auto token = "$C" + std::to_string(index);
    size_t at = 0;
    while ((at = grade.find(token, at)) != std::string::npos) {
      const auto value = constant(index);
      grade.replace(at, token.size(), value);
      at += value.size();
    }
  }
  if (!replace("XePsOut main(XePsIn xe_in) {", grade + R"(
XePsOut main(XePsIn xe_in) {
  float3 nativeScene = 0.0;
  float3 nativeUnfilteredScene = 0.0;
  bool nativeSceneHasCoverage = true;
  float3 nativeEncodedGrade = 0.0;
)")) return {};
  if (gears3) {
    const auto capture = "float4 s0 = r[3].xzyy;\n        float4 s1 = " + constant(4) + ".xxxx;";
    if (!replace(capture, "nativeScene = max(r[3].xzy, 0.0);\n        " + capture)) return {};
  } else {
    const std::string unfiltered = "r[3].w = tv.w;";
    if (!replace(unfiltered, unfiltered + "\n        nativeUnfilteredScene = max(r[3].xyz, 0.0);")) return {};
    const auto capture = "float4 s0 = r[0].yzww;\n        float4 s1 = r[0].xxxx;\n        float4 s2 = -(" + constant(5) + ".xyzz);";
    if (!replace(capture,
        "nativeSceneHasCoverage = r[0].x > 0.0 && isfinite(r[0].x) && all(isfinite(r[0].yzw * r[0].x));\n        "
        "nativeScene = NativeNormalizeScene(r[0].yzw, r[0].x, " + constant(5) +
        ".xyz, nativeUnfilteredScene);\n        " + capture)) return {};
  }
  const std::string outputRegister = gears3 ? "r[1]" : "r[0]";
  const auto reference = "float4 s0 = " + outputRegister + ".xyzz;\n        float4 s1 = " + constant(0) + ".xxxx;";
  if (!replace(reference, "nativeEncodedGrade = max(" + outputRegister + ".xyz, 0.0);\n        " + reference)) return {};
  const std::string begin = "  // Linear-light composition in scene reference-white units.\n";
  const std::string end = "  o.color0.rgb = nativeColor;";
  const auto start = source.find(begin), finish = source.find(end);
  if (start == std::string::npos || finish == std::string::npos || finish < start ||
      source.find(begin, start + begin.size()) != std::string::npos ||
      source.find(end, finish + end.size()) != std::string::npos) return {};
  source.replace(start, finish + end.size() - start,
      "  // Extended game curve / LUT, followed by primary PsychoV35.\n"
      "  float nativePeak = max(asfloat(xe_sys2.x), 1e-4);\n"
      "  float nativeHighlights = xe_sys2.z != 0u ? asfloat(xe_sys2.z) - 1.0 : 1.0;   // COGBox 0.6.2: strength + 1\n"
      "  float3 nativeExtendedGrade = " + std::string(gears3 ?
          "NativeGears3HdrBoost(NativeGears3ExtendedGrade(nativeScene, r[2].w), 0.40, nativeHighlights)" :
          "NativeGears2PeakInput(NativeGears2ExtendedGrade(nativeScene), nativePeak, nativeHighlights)") + ";\n"
      "  float3 nativeVanillaGrade = pow(max(nativeEncodedGrade, 0.0), 2.2);\n"
      + std::string(gears3 ? "" : "  if (!nativeSceneHasCoverage) nativeVanillaGrade = NativeGears2Grade(nativeScene);\n") +
      "  float nativeFade = pow(max(" + constant(0) + ".x * xe_color_exp_scale.x, 0.0), 2.2);\n"
      "  o.color0.rgb = NativeUe3MapExtendedGrade(nativeScene, nativeExtendedGrade, nativeVanillaGrade, nativePeak, nativeFade, 0.75, max(12.0, nativePeak * 2.0));\n");
  return std::string(kPsycho35Hlsl) + kUe3GradeHlsl + source;
}
inline std::string Gears3SceneSource(std::string source, uint32_t low, uint32_t high) {
  return Psycho35SceneSource(std::move(source), low, high, true);
}
inline std::string Gears2SceneSource(std::string source, uint32_t low, uint32_t high) {
  return Psycho35SceneSource(std::move(source), low, high, false);
}
}  // namespace xenos::renderer::native_hdr
