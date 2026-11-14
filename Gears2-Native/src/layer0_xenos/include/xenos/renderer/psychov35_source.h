// Vendored self-contained PsychoV35 supplied by the user.
// Copyright and MIT notices are retained inside the embedded shader and in
// renderer/psychov35/LICENSE.txt. Algorithm unchanged; line endings normalized.
#pragma once
namespace xenos::renderer::native_hdr {
inline constexpr const char* kPsycho35Hlsl = R"COG_PSYCHO35(
#ifndef PSYCHOV_CUSTOMTEST35_HLSLI_
#define PSYCHOV_CUSTOMTEST35_HLSLI_

/*
 * Copyright (C) 2026 Carlos Lopez
 * Modifications Copyright (C) 2026 Musa Haji
 * SPDX-License-Identifier: MIT
 */

// Self-contained HLSL: no external includes, settings, or resource bindings.
// Input/output: extended linear BT.709, D65, reference-white-relative units.
// Set to 1 for modern DXC/SM6 select intrinsics; 0 retains FXC/SM5 support.
#ifndef CUSTOM_PSYCHO35_USE_SM6
#define CUSTOM_PSYCHO35_USE_SM6 0
#endif

namespace renodx {
namespace tonemap {
namespace psychov {

float custom_psycho35_Select(bool condition, float true_value, float false_value) {
#if CUSTOM_PSYCHO35_USE_SM6
  return select(condition, true_value, false_value);
#else
  [flatten]
  if (condition) {
    return true_value;
  } else {
    return false_value;
  }
#endif
}

float3 custom_psycho35_Select(bool condition, float3 true_value, float3 false_value) {
#if CUSTOM_PSYCHO35_USE_SM6
  return select(condition, true_value, false_value);
#else
  [flatten]
  if (condition) {
    return true_value;
  } else {
    return false_value;
  }
#endif
}

float3 custom_psycho35_Select(bool condition, float3 true_value, float false_value) {
#if CUSTOM_PSYCHO35_USE_SM6
  return select(condition, true_value, false_value);
#else
  [flatten]
  if (condition) {
    return true_value;
  } else {
    return false_value;
  }
#endif
}

float3 custom_psycho35_Select(bool condition, float true_value, float3 false_value) {
#if CUSTOM_PSYCHO35_USE_SM6
  return select(condition, true_value, false_value);
#else
  [flatten]
  if (condition) {
    return true_value;
  } else {
    return false_value;
  }
#endif
}

float3 custom_psycho35_Select(bool3 condition, float3 true_value, float3 false_value) {
#if CUSTOM_PSYCHO35_USE_SM6
  return select(condition, true_value, false_value);
#else
  float3 result;
  [unroll]
  for (int i = 0; i < 3; ++i) {
    [flatten]
    if (condition[i]) {
      result[i] = true_value[i];
    } else {
      result[i] = false_value[i];
    }
  }
  return result;
#endif
}

float custom_psycho35_DivideSafe(float dividend, float divisor, float fallback) {
  return custom_psycho35_Select(divisor == 0.f, fallback, dividend / divisor);
}

float3 custom_psycho35_DivideSafe(float3 dividend, float3 divisor, float3 fallback) {
  return float3(custom_psycho35_DivideSafe(dividend.x, divisor.x, fallback.x),
                custom_psycho35_DivideSafe(dividend.y, divisor.y, fallback.y),
                custom_psycho35_DivideSafe(dividend.z, divisor.z, fallback.z));
}
// BT.709/BT.2020 D65 transport and the Stockman-CVRL two-degree XYZ fit.
// Preserve this observer basis together with its matching boundary data.
static const float3x3 CUSTOM_PSYCHO35_BT709_TO_XYZ = float3x3(
    .4123907993f, .3575843394f, .1804807884f,
    .2126390059f, .7151686788f, .0721923154f,
    .0193308187f, .1191947798f, .9505321522f);
static const float3x3 CUSTOM_PSYCHO35_XYZ_TO_BT709 = float3x3(
    3.2409699419f, -1.5373831776f, -.4986107603f,
    -.9692436363f, 1.8759675015f, .0415550574f,
    .0556300797f, -.2039769589f, 1.0569715142f);
static const float3x3 CUSTOM_PSYCHO35_BT2020_TO_XYZ = float3x3(
    .6369580483f, .1446169036f, .1688809752f,
    .2627002120f, .6779980715f, .0593017165f,
    .0000000000f, .0280726930f, 1.0609850577f);
static const float3x3 CUSTOM_PSYCHO35_XYZ_TO_BT2020 = float3x3(
    1.7166511880f, -.3556707838f, -.2533662814f,
    -.6666843518f, 1.6164812366f, .0157685458f,
    .0176398574f, -.0427706133f, .9421031212f);
static const float3x3 CUSTOM_PSYCHO35_XYZ_TO_LMS = float3x3(
    .2670502842655792f, .8471990148492798f, -.03470416612462053f,
    -.38706882411220156f, 1.165429935890458f, .10302286696614202f,
    .026727793989083093f, -.02729131667566509f, .5333267257603284f);
static const float3x3 CUSTOM_PSYCHO35_LMS_TO_XYZ = transpose(float3x3(
                                                       cross(CUSTOM_PSYCHO35_XYZ_TO_LMS[1], CUSTOM_PSYCHO35_XYZ_TO_LMS[2]),
                                                       cross(CUSTOM_PSYCHO35_XYZ_TO_LMS[2], CUSTOM_PSYCHO35_XYZ_TO_LMS[0]),
                                                       cross(CUSTOM_PSYCHO35_XYZ_TO_LMS[0], CUSTOM_PSYCHO35_XYZ_TO_LMS[1])))
                                                   / dot(CUSTOM_PSYCHO35_XYZ_TO_LMS[0], cross(CUSTOM_PSYCHO35_XYZ_TO_LMS[1], CUSTOM_PSYCHO35_XYZ_TO_LMS[2]));
static const float3x3 CUSTOM_PSYCHO35_BT709_TO_LMS_MAT = mul(CUSTOM_PSYCHO35_XYZ_TO_LMS, CUSTOM_PSYCHO35_BT709_TO_XYZ);
static const float3x3 CUSTOM_PSYCHO35_LMS_TO_BT709_MAT = mul(CUSTOM_PSYCHO35_XYZ_TO_BT709, CUSTOM_PSYCHO35_LMS_TO_XYZ);
static const float3x3 CUSTOM_PSYCHO35_LMS_TO_BT2020_MAT = mul(CUSTOM_PSYCHO35_XYZ_TO_BT2020, CUSTOM_PSYCHO35_LMS_TO_XYZ);
static const float3x3 CUSTOM_PSYCHO35_BT2020_TO_BT709_MAT = mul(CUSTOM_PSYCHO35_XYZ_TO_BT709, CUSTOM_PSYCHO35_BT2020_TO_XYZ);
static const float3 CUSTOM_PSYCHO35_YF_WEIGHTS = float3(.68990272f, .34832189f, 0.f);
static const float3 CUSTOM_PSYCHO35_MB_WEIGHTS = float3(.68990272f, .34832189f, .0371597069161f);
static const float3 CUSTOM_PSYCHO35_D65_WHITE_LMS = mul(CUSTOM_PSYCHO35_BT709_TO_LMS_MAT, 1.f.xxx);
static const float CUSTOM_PSYCHO35_D65_WHITE_YF = dot(CUSTOM_PSYCHO35_YF_WEIGHTS, CUSTOM_PSYCHO35_D65_WHITE_LMS);
static const float2 CUSTOM_PSYCHO35_MB_WHITE = (CUSTOM_PSYCHO35_D65_WHITE_LMS * CUSTOM_PSYCHO35_MB_WEIGHTS).xz / CUSTOM_PSYCHO35_D65_WHITE_YF;

// Seven-halfspace approximation of the CIE 170-2 MacLeod-Boynton boundary.
static const int CUSTOM_PSYCHO35_CIE1702_EDGE_COUNT = 7;
static const float2 CUSTOM_PSYCHO35_CIE1702_HALFSPACE_NORMALS[CUSTOM_PSYCHO35_CIE1702_EDGE_COUNT] = {
  float2(-0.043889f, -0.006807f),
  float2(-0.007821f, -0.008564f),
  float2(-0.000604f, -0.007942f),
  float2(0.f, -0.080835f),
  float2(0.953597f, 0.307020f),
  float2(-0.060969f, 0.019752f),
  float2(-0.106895f, 0.004035f),
};
static const float CUSTOM_PSYCHO35_CIE1702_HALFSPACE_NUMERATORS[CUSTOM_PSYCHO35_CIE1702_EDGE_COUNT] = {
  0.0065035249f,
  0.00104900495f,
  0.000207697044f,
  0.00165556648f,
  0.252472349f,
  0.0241967351f,
  0.0199621232f,
};

// Preserve an exact mean for equal-component vectors.
float custom_psycho35_Mean(float3 value) {
  return value.x + ((value.y - value.x) + (value.z - value.x)) / 3.f;
}

float3 custom_psycho35_TargetResidual(float3 normalized_lms, int target) {
  float3 physical_lms = normalized_lms * CUSTOM_PSYCHO35_D65_WHITE_LMS;
  return target == 0 ? mul(CUSTOM_PSYCHO35_LMS_TO_BT709_MAT, physical_lms)
                     : mul(CUSTOM_PSYCHO35_LMS_TO_BT2020_MAT, physical_lms);
}

// Local float3 copies of the shared sRGB, gamma, and sign helpers.
float3 custom_psycho35_srgb_Encode(float3 c) {
  return custom_psycho35_Select(
      c <= 0.0031308f,
      c * 12.92f,
      mad(1.055f, pow(c, 1.f / 2.4f), -0.055f));
}

float3 custom_psycho35_srgb_Decode(float3 c) {
  return custom_psycho35_Select(
      c <= 0.04045f,
      c / 12.92f,
      pow(mad(c, 1.f / 1.055f, 0.055f / 1.055f), 2.4f));
}

float3 custom_psycho35_gamma_Encode(float3 c, float gamma = 2.2f) {
  return pow(c, 1.f / gamma);
}

float3 custom_psycho35_gamma_Decode(float3 c, float gamma = 2.2f) {
  return pow(c, gamma);
}

float3 custom_psycho35_CopySign(float3 mag, float3 sgn) {
  uint3 sign_bits = asuint(sgn) & 0x80000000u;
  uint3 mag_bits = asuint(mag) & 0x7FFFFFFFu;
  float3 result = asfloat(sign_bits | mag_bits);
  return result;
}

// Preserve GammaSafe's original branches, operation order, and defaults.
float3 custom_psycho35_GammaSafe(float3 c, bool pow_to_srgb = false, float gamma = 2.2f) {
  if (pow_to_srgb) {
    return custom_psycho35_CopySign(custom_psycho35_srgb_Decode(custom_psycho35_gamma_Encode(abs(c), gamma)), c);
  } else {
    return custom_psycho35_CopySign(custom_psycho35_gamma_Decode(custom_psycho35_srgb_Encode(abs(c)), gamma), c);
  }
}

// Map a finite input white to peak while leaving values through anchor intact.
// Requires anchor < peak, compression_strength >= 1, and anchor < clip.
#define CUSTOM_PSYCHO35_C_INFINITY_ROLLOFF_GENERATOR(T)                                                                           \
  T custom_psycho35_CInfinityRollOff(T color, T peak, T anchor, float compression_strength, T clip) {                             \
    T shoulder_range = peak - anchor;                                                                                             \
    T clip_range = clip - anchor;                                                                                                 \
    T clipped_distance = min(max(color - anchor, (T)0.f), clip_range);                                                            \
    T remaining_distance = clip_range - clipped_distance;                                                                         \
    T inverse_position = custom_psycho35_DivideSafe(                                                                              \
        shoulder_range * remaining_distance, abs(clip - peak) * clipped_distance, (T)0.f);                                        \
    T flat_weight = exp2(-inverse_position * rcp(compression_strength));                                                          \
    T denominator = flat_weight + inverse_position;                                                                               \
    T transition = clipped_distance * flat_weight * rcp(clip_range * denominator);                                                \
    T exponent = log(2.f) * inverse_position * rcp(compression_strength);                                                         \
    T one_minus_weight = custom_psycho35_Select(exponent < 0.01f,                                                                 \
                                                exponent * (1.f + exponent * (-0.5f + exponent * (1.f / 6.f - exponent / 24.f))), \
                                                1.f - flat_weight);                                                               \
    T deficit = custom_psycho35_Select(peak < clip, inverse_position - one_minus_weight,                                          \
                                       denominator + sign(peak - clip));                                                          \
    T headroom = shoulder_range * (remaining_distance / clip_range) * (deficit / denominator);                                    \
    T response = custom_psycho35_Select(clipped_distance > 0.5f * shoulder_range, peak - headroom,                                \
                                        mad(peak - clip, transition, min(color, clip)));                                          \
    return custom_psycho35_Select(color >= clip, peak, response);                                                                 \
  }

CUSTOM_PSYCHO35_C_INFINITY_ROLLOFF_GENERATOR(float)
CUSTOM_PSYCHO35_C_INFINITY_ROLLOFF_GENERATOR(float3)
#undef CUSTOM_PSYCHO35_C_INFINITY_ROLLOFF_GENERATOR

// Derivatives accompany the grade because test35's optional boundary trade
// must use the response actually rendered, not the original sigmoid's speed.
float3 custom_psycho35_CInfinityTransition(float3 position, out float3 derivative) {
  const bool3 interior = (position > 0.f) & (position < 1.f);
  const float3 t = custom_psycho35_Select(interior, position, 0.5f);
  const float3 value = rcp(1.f + exp2((1.f - 2.f * t) / (t * (1.f - t))));
  derivative = custom_psycho35_Select(interior, log(2.f) * value * (1.f - value) * (rcp(t * t) + rcp((1.f - t) * (1.f - t))), 0.f);
  return custom_psycho35_Select(interior, value, saturate(position));
}

// Input and output are adaptation-relative cone signals. logarithmic_slope
// is d log(output) / d log(input), separately for each cone.
float3 custom_psycho35_ApplyAnchoredTonalGrading(
    float3 normalized, float contrast, float flare,
    float highlight_contrast, float shadow_contrast, float highlights, float shadows,
    out float3 logarithmic_slope) {
  float3 graded = normalized;
  logarithmic_slope = 1.f;
  if (contrast != 1.f || flare > 0.f) {
    float3 exponent = contrast;
    float3 exponent_derivative = 0.f;
    if (flare > 0.f) {
      const bool3 shadow = normalized < 1.f;
      const float3 distance = custom_psycho35_Select(shadow, 1.f - normalized, 1.f);
      const float3 weight = custom_psycho35_Select(shadow, exp2(-normalized / distance), 0.f);
      const float3 flare_weight = weight * flare / (normalized + flare);
      exponent *= 1.f + flare_weight;
      exponent_derivative = -contrast * flare_weight * normalized
                            * (log(2.f) / (distance * distance) + rcp(normalized + flare));
    }
    // Exact zero cones stay zero; they do not acquire an arbitrary log floor.
    const float3 stops = log2(custom_psycho35_Select(normalized == 0.f, 1.f, normalized));
    const float3 highlight_stops = max(stops, 0.f);
    const float3 displacement = (contrast - 1.f) * highlight_stops;
    const float3 magnitude = abs(displacement);
    const float3 weight = custom_psycho35_Select(magnitude > 0.f, exp2(-rcp(magnitude)), 0.f);
    const float3 denominator = mad(magnitude, weight, 1.f);
    const float3 output_highlights = highlight_stops + displacement / denominator;
    graded = exp2(mad(exponent, min(stops, 0.f), output_highlights));
    logarithmic_slope = custom_psycho35_Select(normalized < 1.f,
                                               exponent + log(2.f) * stops * exponent_derivative,
                                               1.f + (contrast - 1.f) * (1.f - log(2.f) * weight) / (denominator * denominator));
    graded = custom_psycho35_Select(normalized == 0.f, 0.f, graded);
  }
  if (highlight_contrast != 1.f) {
    const float3 distance = max(graded - 1.f, 0.f);
    const float3 squared = distance * distance;
    const float3 weight = custom_psycho35_Select(squared > 0.f, exp2(-rcp(squared)), 0.f);
    const float3 flat_distance = (1.f + squared) * weight;
    const float3 weighted_inverse = custom_psycho35_Select(weight > 0.f, weight / squared, 0.f);
    const float3 flat_derivative = 2.f * distance * weight
                                   + custom_psycho35_DivideSafe(2.f * log(2.f) * (1.f + squared) * weighted_inverse, distance, 0.f);
    const float power = 0.5f * (highlight_contrast - 1.f);
    const float3 factor = pow(1.f + flat_distance, power);
    const float3 adjusted = graded + distance * (factor - 1.f);
    logarithmic_slope *= custom_psycho35_DivideSafe(graded, adjusted, 1.f)
                         * (factor + distance * factor * power * flat_derivative / (1.f + flat_distance));
    graded = adjusted;
  }
  if (shadow_contrast != 1.f) {
    const float3 distance = saturate(1.f - graded);
    const float3 squared = distance * distance;
    const float3 weight = custom_psycho35_Select(squared > 0.f, exp2(1.f - rcp(squared)), 0.f);
    const float3 flat_distance = squared * distance * weight;
    const float3 flat_derivative = -weight * (3.f * squared + 2.f * log(2.f));
    logarithmic_slope *= 1.f + graded * (1.f - shadow_contrast) * flat_derivative / (1.f + flat_distance);
    graded *= pow(1.f + flat_distance, 1.f - shadow_contrast);
  }
  if (highlights != 1.f || shadows != 1.f) {
    const float inverse_range = 1.f / 7.f;
    const float3 stops = log2(custom_psycho35_Select(graded == 0.f, 1.f, graded));
    float3 displacement = 0.f;
    float3 slope = 1.f;
    if (highlights != 1.f) {
      const float adjustment = highlights - 1.f;
      const float amount = adjustment * mad(1.5f, abs(adjustment), 0.5f);
      float3 derivative;
      displacement += amount * custom_psycho35_CInfinityTransition((stops - 1.f) * inverse_range, derivative);
      slope += amount * derivative * inverse_range;
    }
    if (shadows != 1.f) {
      const float adjustment = shadows - 1.f;
      const float amount = adjustment * mad(1.5f, abs(adjustment), 0.5f);
      float3 derivative;
      displacement += amount * custom_psycho35_CInfinityTransition((-1.f - stops) * inverse_range, derivative);
      slope -= amount * derivative * inverse_range;
    }
    graded *= exp2(displacement);
    logarithmic_slope *= slope;
  }
  logarithmic_slope = custom_psycho35_Select(normalized == 0.f, 0.f, logarithmic_slope);
  return graded;
}

// Return normalized cone response and its derivative with respect to log(q).
// Compression is the supplied shoulder strength; there is no automatic fit.
float3 custom_psycho35_Response(
    float3 source_ratio, float3 anchor_out, float peak, float compression, float clip_point,
    float contrast, float flare, float highlight_contrast, float shadow_contrast, float highlights, float shadows,
    out float3 derivative) {
  float3 grade_slope;
  const float3 graded = custom_psycho35_ApplyAnchoredTonalGrading(
                            source_ratio, contrast, flare, highlight_contrast, shadow_contrast, highlights, shadows, grade_slope)
                        * anchor_out;
  const float3 peak_lms = peak * CUSTOM_PSYCHO35_D65_WHITE_LMS;
  const float3 clip_lms = clip_point * CUSTOM_PSYCHO35_D65_WHITE_LMS;
  const float3 response = custom_psycho35_CInfinityRollOff(graded, peak_lms, anchor_out, compression, clip_lms);
  const float3 range = clip_lms - anchor_out;
  const float3 distance = min(max(graded - anchor_out, 0.f), range);
  const float3 inverse = custom_psycho35_DivideSafe((peak_lms - anchor_out) * (range - distance), abs(clip_lms - peak_lms) * distance, 0.f);
  const float3 weight = exp2(-inverse / compression);
  const float3 denominator = weight + inverse;
  const float3 inverse_derivative = custom_psycho35_DivideSafe((peak_lms - anchor_out) * range, abs(clip_lms - peak_lms) * distance * distance, 0.f);
  const float3 transition_derivative = (weight / denominator
                                        + distance * weight * inverse_derivative * (1.f + log(2.f) * inverse / compression)
                                              / (denominator * denominator))
                                       / range;
  float3 response_slope = mad(peak_lms - clip_lms, transition_derivative, 1.f);
  response_slope = custom_psycho35_Select(graded <= anchor_out, 1.f, custom_psycho35_Select(graded >= clip_lms, 0.f, response_slope));
  derivative = response_slope * graded * grade_slope / peak_lms;
  return custom_psycho35_Select(response == peak_lms, 1.f, response / peak_lms);
}

float3 custom_psychotm_test35(
    float3 bt709_linear_input,
    float peak_value = 1000.f / 203.f,
    float exposure = 1.f,
    float highlights = 1.f,
    float shadows = 1.f,
    float contrast = 1.f,
    float flare = 0.f,
    float highlight_contrast = 1.f,
    float shadow_contrast = 1.f,
    float purity_scale = 1.f,
    float highlight_saturation = 1.f,
    float dechroma = 0.f,
    float3 current_adaptive_state_bt709 = 0.18f,
    float3 current_background_state_bt709 = 0.18f,
    float sdr_eotf_emulation = 0.f,
    float gamut_compression = 1.f,
    int gamut_compression_mode = 0,
    float compression = 1.5f,
    float clip_point = 100.f,
    bool human_gamut_conditioning = false,
    bool gamut_fill = false,
    int fit_mode = 2,
    float fit_power = 0.f) {
  // Convert extended BT.709 directly without source-gamut contraction.
  const float3 input_lms = mul(CUSTOM_PSYCHO35_BT709_TO_LMS_MAT, bt709_linear_input * exposure);
  const float3 anchor_in = mul(CUSTOM_PSYCHO35_BT709_TO_LMS_MAT, current_adaptive_state_bt709);
  const float3 anchor_out = all(current_background_state_bt709 == current_adaptive_state_bt709)
                                ? anchor_in
                                : mul(CUSTOM_PSYCHO35_BT709_TO_LMS_MAT, current_background_state_bt709);
  float3 source_ratio = input_lms / anchor_in;
  if (all(source_ratio == 0.f)) return 0.f;
  if (purity_scale != 1.f || highlight_saturation != 1.f || dechroma != 0.f) {
    float effective_purity = purity_scale;
    if (highlight_saturation != 1.f || dechroma != 0.f) {
      const float relative_yf = max(dot(CUSTOM_PSYCHO35_YF_WEIGHTS, source_ratio) / dot(CUSTOM_PSYCHO35_YF_WEIGHTS, 1.f.xxx), 0.f);
      const float t = saturate(log2(max(relative_yf, 1.f)) / (2.75f * log2(10.f)));
      const float rolloff = t * t * t * mad(t, mad(6.f, t, -15.f), 10.f);
      effective_purity *= mad(-dechroma, rolloff, 1.f);
      effective_purity *= mad(highlight_saturation - 1.f, rolloff * rolloff * mad(0.5f, rolloff, 0.5f) * (2.f / 3.f), 1.f);
    }
    const float neutral = dot(CUSTOM_PSYCHO35_YF_WEIGHTS, input_lms) / dot(CUSTOM_PSYCHO35_YF_WEIGHTS, anchor_in);
    const float3 direction = source_ratio - neutral;
    // Keep test35's common purity bound, rather than clipping cones separately.
    if (all(source_ratio >= 0.f) && neutral > 0.f && effective_purity > 1.f) {
      [unroll]
      for (int cone = 0; cone < 3; ++cone) {
        if (direction[cone] < 0.f) effective_purity = min(effective_purity, neutral / -direction[cone]);
      }
      source_ratio = max(neutral + effective_purity * direction, 0.f);
    } else {
      source_ratio = neutral + effective_purity * direction;
    }
  }
  if (sdr_eotf_emulation != 0.f) {
    source_ratio = custom_psycho35_GammaSafe(source_ratio * anchor_in / CUSTOM_PSYCHO35_D65_WHITE_LMS)
                   * CUSTOM_PSYCHO35_D65_WHITE_LMS / anchor_in;
  }
  if (human_gamut_conditioning) {
    // Retain test35's homogeneous human-boundary intersection and local A/C
    // trade, evaluating the substituted anchored response and its derivative.
    const float mean_q = custom_psycho35_Mean(source_ratio);
    if (!(mean_q > 0.f) || isinf(mean_q)) return asfloat(0x7fc00000u).xxx;
    const float3 direction_q = source_ratio - mean_q;
    float3 origin_weighted = (anchor_in * mean_q) * CUSTOM_PSYCHO35_MB_WEIGHTS;
    float3 direction_weighted = (anchor_in * direction_q) * CUSTOM_PSYCHO35_MB_WEIGHTS;
    const float origin_y = origin_weighted.x + origin_weighted.y;
    if (!(origin_y > 0.f) || isinf(origin_y)) return asfloat(0x7fc00000u).xxx;
    origin_weighted /= origin_y;
    direction_weighted /= origin_y;
    if (any(isnan(origin_weighted)) || any(isinf(origin_weighted))
        || any(isnan(direction_weighted)) || any(isinf(direction_weighted))) return asfloat(0x7fc00000u).xxx;
    const float direction_y = direction_weighted.x + direction_weighted.y;
    float boundary_t = 1.f;
    [unroll]
    for (int edge = 0; edge < CUSTOM_PSYCHO35_CIE1702_EDGE_COUNT; ++edge) {
      const float2 normal = CUSTOM_PSYCHO35_CIE1702_HALFSPACE_NORMALS[edge];
      const float offset = CUSTOM_PSYCHO35_CIE1702_HALFSPACE_NUMERATORS[edge] + dot(normal, CUSTOM_PSYCHO35_MB_WHITE);
      const float margin = offset * (origin_weighted.x + origin_weighted.y) - dot(normal, origin_weighted.xz);
      const float denominator = dot(normal, direction_weighted.xz) - offset * direction_y;
      if (!(margin >= 0.f) || isnan(denominator) || isinf(denominator)) return asfloat(0x7fc00000u).xxx;
      if (denominator > 0.f) boundary_t = min(boundary_t, margin / denominator);
    }
    [unroll]
    for (int cone = 0; cone < 3; ++cone) {
      if (direction_q[cone] < 0.f) boundary_t = min(boundary_t, mean_q / -direction_q[cone]);
    }
    if (!(origin_weighted.x + origin_weighted.y + boundary_t * direction_y > 0.f)) return asfloat(0x7fc00000u).xxx;
    if (boundary_t < 1.f) {
      float3 boundary_q = mean_q + boundary_t * direction_q;
      if (any(boundary_q < -1e-6f * mean_q)) return asfloat(0x7fc00000u).xxx;
      if (any(boundary_q <= 0.f)) boundary_q = lerp(boundary_q, mean_q.xxx, 1e-6f);
      if (any(boundary_q <= 0.f) || any(isnan(boundary_q)) || any(isinf(boundary_q))) return asfloat(0x7fc00000u).xxx;
      float3 exposure_derivative;
      const float3 response = custom_psycho35_Response(
          boundary_q, anchor_out, peak_value, compression, clip_point, contrast,
          flare, highlight_contrast, shadow_contrast, highlights, shadows, exposure_derivative);
      const float derivative_scale = max(exposure_derivative.x, max(exposure_derivative.y, exposure_derivative.z));
      if (!(derivative_scale > 0.f) || isinf(derivative_scale)) return asfloat(0x7fc00000u).xxx;
      exposure_derivative /= derivative_scale;
      const float3 incoming_derivative = exposure_derivative * direction_q / boundary_q;
      const float3 residual = response - custom_psycho35_Mean(response);
      const float radius = length(residual);
      if (!(radius > 0.f) || isinf(radius)
          || any(isnan(exposure_derivative)) || any(isinf(exposure_derivative))
          || any(isnan(incoming_derivative)) || any(isinf(incoming_derivative))) return asfloat(0x7fc00000u).xxx;
      const float incoming_speed = length(float2(sqrt(3.f) * custom_psycho35_Mean(incoming_derivative), dot(residual / radius, incoming_derivative)));
      const float exposure_speed = length(float2(sqrt(3.f) * custom_psycho35_Mean(exposure_derivative), dot(residual / radius, exposure_derivative)));
      if (!(exposure_speed > 0.f) || isnan(exposure_speed) || isinf(exposure_speed)
          || isnan(incoming_speed) || isinf(incoming_speed)) return asfloat(0x7fc00000u).xxx;
      boundary_q *= 1.f + (1.f - boundary_t) * incoming_speed / exposure_speed;
      if (any(isnan(boundary_q)) || any(isinf(boundary_q))) return asfloat(0x7fc00000u).xxx;
      source_ratio = boundary_q;
    }
    source_ratio = max(source_ratio, 0.f);
  }
  if (all(source_ratio == 0.f)) return 0.f;
  float3 unused_derivative;
  const float3 response = custom_psycho35_Response(
      source_ratio, anchor_out, peak_value, compression, clip_point, contrast,
      flare, highlight_contrast, shadow_contrast, highlights, shadows, unused_derivative);
  const float3 deficit = 1.f - response;
  const float response_mean = custom_psycho35_Mean(response);
  const float mean = peak_value * response_mean;
  const float headroom = peak_value * custom_psycho35_Mean(deficit);
  float3 residual = response_mean > 0.5f ? custom_psycho35_Mean(deficit) - deficit : response - response_mean;
  const float scale = max(abs(residual.x), max(abs(residual.y), abs(residual.z)));
  const float3 scaled = scale > 0.f ? residual / scale : 0.f;
  const float radius = length(scaled);
  float3 direction = radius > 0.f ? scaled / radius : 0.f;
  // The mean-A2 source direction stays adaptation-relative, before the tonal
  // response. Normalize its exposure scale before taking a length, as in v35.
  const float drive = max(abs(source_ratio.x), max(abs(source_ratio.y), abs(source_ratio.z)));
  const float3 normalized_source = source_ratio / drive;
  const float3 source_residual = normalized_source - custom_psycho35_Mean(normalized_source);
  const float source_radius = length(source_residual);
  if (source_radius > 0.f && radius > 0.f) {
    const float3 midpoint = source_residual / source_radius + direction;
    const float midpoint_length = length(midpoint);
    if (midpoint_length > 0.f) direction = midpoint / midpoint_length;
  }
  residual = peak_value * scale * radius * direction;
  if (gamut_compression == 0.f || fit_mode == 0) return mean + custom_psycho35_TargetResidual(residual, 0);
  if (all(residual == 0.f)) return mean.xxx;
  const float3 target_displacement = custom_psycho35_TargetResidual(residual, gamut_compression_mode);
  if (fit_mode == 1) {
    float3 clipped = clamp(mean + target_displacement, 0.f, peak_value);
    if (gamut_compression_mode != 0) clipped = mul(CUSTOM_PSYCHO35_BT2020_TO_BT709_MAT, clipped);
    return lerp(mean + custom_psycho35_TargetResidual(residual, 0), clipped, gamut_compression);
  }
  if (fit_mode == 4) {
    // Keep test35's current local-shoulder diagnostic unchanged.
    const float knee = 0.95f;
    float support = rcp(knee);
    [unroll]
    for (int channel = 0; channel < 3; ++channel) {
      if (target_displacement[channel] > 0.f)
        support = min(support, headroom / target_displacement[channel]);
      else if (target_displacement[channel] < 0.f)
        support = min(support, mean / -target_displacement[channel]);
    }
    if (support >= rcp(knee)) return mean + custom_psycho35_TargetResidual(residual, 0);
    const float excess = max(0.f, 1.f - knee * support);
    const float shoulder = support * (knee + (1.f - knee) * excess / (excess + (1.f - knee) * support));
    return mean + custom_psycho35_TargetResidual(residual * lerp(1.f, shoulder, gamut_compression), 0);
  }
  if (fit_power == 0.f || (fit_mode != 4 && (gamut_fill || fit_mode == 2))) {
    float support = 1.f;
    [unroll]
    for (int channel = 0; channel < 3; ++channel) {
      if (target_displacement[channel] > 0.f)
        support = min(support, headroom / target_displacement[channel]);
      else if (target_displacement[channel] < 0.f)
        support = min(support, mean / -target_displacement[channel]);
    }
    return mean + custom_psycho35_TargetResidual(residual * lerp(1.f, support, gamut_compression), 0);
  }
  if (fit_power == 2.f) {
    float3 pressure = 0.f;
    bool endpoint_contact = false;
    [unroll]
    for (int channel = 0; channel < 3; ++channel) {
      if (target_displacement[channel] != 0.f) {
        const float available = target_displacement[channel] > 0.f ? headroom : mean;
        if (available == 0.f)
          endpoint_contact = true;
        else
          pressure[channel] = abs(target_displacement[channel]) / available;
      }
    }
    if (endpoint_contact) return mean + custom_psycho35_TargetResidual(residual * (1.f - gamut_compression), 0);
    const float normalization = max(1.f, max(pressure.x, max(pressure.y, pressure.z)));
    if (all(isfinite(pressure))) {
      const float inverse = rcp(normalization);
      const float3 normalized_pressure = pressure / normalization;
      const float support = inverse * rsqrt(inverse * inverse + dot(normalized_pressure, normalized_pressure));
      return mean + custom_psycho35_TargetResidual(residual * lerp(1.f, support, gamut_compression), 0);
    }
  }
  float3 log_pressure = asfloat(0xff800000u);
  bool endpoint_contact = false;
  [unroll]
  for (int channel = 0; channel < 3; ++channel) {
    if (target_displacement[channel] != 0.f) {
      const float available = target_displacement[channel] > 0.f ? headroom : mean;
      if (available == 0.f)
        endpoint_contact = true;
      else
        log_pressure[channel] = log(abs(target_displacement[channel])) - log(available);
    }
  }
  const float largest_log = max(0.f, max(log_pressure.x, max(log_pressure.y, log_pressure.z)));
  const float3 scaled_pressure = exp(fit_power * (log_pressure - largest_log));
  const float support = endpoint_contact ? 0.f
                                         : exp(-largest_log - log(exp(-fit_power * largest_log) + scaled_pressure.x + scaled_pressure.y + scaled_pressure.z) / fit_power);
  return mean + custom_psycho35_TargetResidual(residual * lerp(1.f, support, gamut_compression), 0);
}

}  // namespace psychov
}  // namespace tonemap
}  // namespace renodx

#endif  // PSYCHOV_CUSTOMTEST35_HLSLI_
)COG_PSYCHO35";
}

