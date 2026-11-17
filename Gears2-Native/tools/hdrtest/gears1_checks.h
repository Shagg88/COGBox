// Authored Gears 1 grade/near-black fixtures; no game shader data. MIT.
#pragma once
#include <xenos/renderer/gears1_hdr_bridge.h>
static int Gears1Check(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  constexpr unsigned count = 32;
  const float levels[count] = {0,     .000001f, .00001f, .0001f, .001f, .002f, .004f, .008f,
                               .016f, .018f,    .019f,   .020f,  .021f, .022f, .024f, .032f,
                               .05f,  .1f,      .18f,    .5f,    1,     2,     4,     8,
                               16,    64,       65000,   0,      1,     2,     4,     8};
  for (float gamma : {1.f / 2.2f, .5f})
    for (float shadows : {0.f, .02f}) {
      std::string source = R"(
cbuffer Params:register(b0){uint4 xe_sys2;float4 xe_color_exp_scale;};
Texture2D<float4> scene:register(t0);RWTexture2D<float4> output:register(u0);
struct XePsIn{uint pixel;};struct XePsOut{float4 color0;float4 color1;};
static const float4 xe_c[256]={
)";
      for (unsigned i = 0; i < 256; ++i) {
        if (i == 3)
          source += "float4(" + std::to_string(shadows) + "," + std::to_string(shadows) + "," +
                    std::to_string(shadows) + ",.8),";
        else if (i == 4)
          source += "float4(.9,1.1,.7,0),";
        else if (i == 5)
          source += "float4(.9,1.1,.8,0),";
        else if (i == 6)
          source += "float4(.03,.06,.01,0),";
        else if (i == 7)
          source += "float4(.8,1.1,.9," + std::to_string(gamma) + "),";
        else
          source += "float4(0,0,0,0),";
      }
      source += R"(};
XePsOut main(XePsIn xe_in) {
  float4 r[4];float3 value=scene.Load(int3(xe_in.pixel,0,0)).rgb;
  r[1]=float4(value.zxy,1);r[0]=float4(1,0,0,0);r[3]=float4(value.yxz,1);
  if(xe_in.pixel==27)r[0].x=asfloat(0x7f800000u);
  {
    float4 s0 = r[1].yzxx;
        float4 s1 = r[0].xxxx;
        float4 s2 = -(xe_c[3].yxzz);
  }
  // Independent bounded version of the captured register chain.
  r[0].xyz=saturate(value-xe_c[3].yxz);
  r[1].xyz=r[0].zxy*xe_c[4].zyx;
  r[0].xyz=log2(abs(r[1].xyz))*xe_c[5].zyx;
  r[1].xyz=exp2(r[0].zyx);
  float3 mixed=r[1].xyz*xe_c[3].w+xe_c[8].xyz+dot(r[1].zxy,xe_c[6].zxy);
  XePsOut o;o.color0=float4(pow(saturate(mixed*xe_c[7].xyz),xe_c[7].www)*xe_color_exp_scale.x,0);
  // Linear-light composition in scene reference-white units.
  float3 nativeColor=0;
  o.color0.rgb = nativeColor;
  return o;
}
)";
      if (!hdr::Gears1SceneSource("wrong interface", 256, 256).empty())
        return 150;
      source = hdr::Gears1SceneSource(source, 256, 256);
      if (source.empty())
        return 151;
      const auto at = source.find("XePsOut main(");
      source.replace(at, strlen("XePsOut main"), "XePsOut TestMaterial");
      source += R"(
[numthreads(8,1,1)]void main(uint3 p:SV_DispatchThreadID) {
  if(p.x>=32)return;XePsIn i;i.pixel=p.x;
  float3 value=scene.Load(int3(p.xy,0)).rgb;
  if(xe_sys2.y==1)output[p.xy]=float4(NativeGears1Grade(value),0);
  else if(xe_sys2.y==2)output[p.xy]=float4(NativeGears1Grade(value,true),0);
  else if(xe_sys2.y==3)output[p.xy]=TestMaterial(i).color1;
  else output[p.xy]=TestMaterial(i).color0;
}
)";
      std::vector<uint8_t> code;
      std::string error;
      if (!compiler.Compile(source, gpu::ShaderStage::kCompute, backend.GetShaderIL(), code,
                            error)) {
        std::printf("FAIL Gears 1 compile: %s\n", error.c_str());
        return 152;
      }
      const auto shader =
          backend.CreateShader(gpu::ShaderStage::kCompute, code.data(), code.size());
      const auto pipeline = shader ? backend.CreateComputePipeline(shader) : 0;
      gpu::TextureDesc td;
      td.width = count;
      td.height = 1;
      td.format = gpu::Format::kR16G16B16A16Float;
      td.usage = gpu::kUsageSampled | gpu::kUsageStorage;
      const auto input = backend.CreateTexture(td), out = backend.CreateTexture(td);
      if (!input || !out || !pipeline)
        return 153;
      uint16_t data[count * 4]{};
      for (unsigned i = 0; i < count; ++i)
        for (unsigned c = 0; c < 3; ++c)
          data[4 * i + c] = ToHalf(levels[i] * (i >= 28 ? (c == i % 3 ? 1.f : .1f) : 1.f));
      backend.UploadTexture(input, 0, 0, data, count * 8, 1, 1);
      auto read = [&](unsigned mode, float fade, float peak) {
        gpu::ComputeBindings b;
        b.constant_count = 8;
        b.constants[0] = std::bit_cast<uint32_t>(peak);
        b.constants[1] = mode;
        b.constants[4] = std::bit_cast<uint32_t>(fade);
        b.srv[0] = input;
        b.uav_texture = out;
        backend.Dispatch(pipeline, b, (count + 7) / 8, 1, 1);
        const auto rb = backend.ReadbackTexture(out, {0, 0, count, 1});
        backend.WaitForFence(backend.Submit());
        uint32_t pitch = 0;
        const auto* bytes = backend.ReadbackData(rb, pitch);
        std::vector<uint16_t> result;
        if (bytes) {
          result.resize(count * 4);
          std::memcpy(result.data(), bytes, count * 8);
        }
        backend.ReleaseReadback(rb);
        return result;
      };
      const auto bounded = read(2, 1, 20), sdr = read(3, 1, 20), extended = read(1, 1, 20);
      if (bounded.empty() || sdr.empty() || extended.empty())
        return 154;
      for (unsigned i = 0; i < count; ++i)
        for (unsigned c = 0; c < 3; ++c)
          if (std::abs(std::pow(Half(sdr[4 * i + c]), 2.2f) - Half(bounded[4 * i + c])) > .003f)
            return 160;
      for (float peak : {1.f, 5.f, 4000.f / 203.f})
        for (float fade : {1.f, .5f, 0.f}) {
          const auto result = read(0, fade, peak);
          if (result.empty())
            return 154;
          for (unsigned i = 0; i < count; ++i)
            for (unsigned c = 0; c < 3; ++c) {
              const float value = Half(result[4 * i + c]),
                          reference = Half(bounded[4 * i + c]) * std::pow(fade, 2.2f);
              if (!std::isfinite(value) || value < 0 || value > peak + .02f ||
                  (fade == 0 && value != 0))
                return 155;
              // The original grade deliberately subtracts the authored shadow offset.
              // The HDR path must not create any additional zeroes above that floor.
              if (reference > 0.000001f && value == 0) {
                std::printf("FAIL added black crush pixel %u channel %u\n", i, c);
                return 156;
              }
              if (i <= 18 && std::abs(value - reference) > std::max(.000003f, reference * .004f)) {
                std::printf("FAIL Gears 1 shadow/diffuse %u: %g != %g\n", i, value, reference);
                return 157;
              }
              if (i == 27 && value != 0)
                return 158;
            }
          // Colored highlights retain the unclipped grade's RGB ratios even
          // when the SDR companion has clipped channels and the peak fit runs.
          if (fade == 1 && peak > 1)
            for (unsigned i = 28; i < count; ++i) {
              float maximum = 0, mappedMaximum = 0;
              for (unsigned c = 0; c < 3; ++c) {
                maximum = std::max(maximum, Half(extended[4 * i + c]));
                mappedMaximum = std::max(mappedMaximum, Half(result[4 * i + c]));
              }
              if (maximum < 1.01f || mappedMaximum <= 0) continue;
              for (unsigned c = 0; c < 3; ++c)
                if (std::abs(Half(result[4 * i + c]) / mappedMaximum -
                             Half(extended[4 * i + c]) / maximum) > .004f) {
                  std::printf("FAIL Gears 1 highlight hue pixel %u channel %u\n", i, c);
                  return 161;
                }
            }
          if (peak > 19 && fade == 1 && Half(result[4 * 25 + 1]) <= 1.f)
            return 159;
        }
      backend.DestroyTexture(input);
      backend.DestroyTexture(out);
      std::printf(
          "PASS Gears 1 gamma %.4f shadows %.3f: near-black separation, authored black floor, "
          "diffuse, HDR, peak fit, fades, invalid coverage\n",
          gamma, shadows);
    }
  return 0;
}
