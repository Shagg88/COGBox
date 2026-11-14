// Properties of the production curve and LUT extension, using authored charts.
template<class Pipeline, class Texture>
static int ToneExtensionCheck(gpu::Backend& backend, Pipeline pipeline,
    Texture input, Texture lut, Texture output, unsigned count, float exposure,
    const uint16_t* inputs, const std::vector<uint16_t>& originalLut) {
  auto dispatch=[&](unsigned mode) {
    gpu::ComputeBindings b;b.constant_count=8;
    b.constants[0]=std::bit_cast<uint32_t>(19.f);b.constants[1]=mode;
    b.constants[4]=std::bit_cast<uint32_t>(1.f);
    b.srv[0]=input;b.srv[1]=lut;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);
    const auto rb=backend.ReadbackTexture(output,{0,0,static_cast<int32_t>(count),1});
    backend.WaitForFence(backend.Submit());
    uint32_t pitch=0;const auto* bytes=backend.ReadbackData(rb,pitch);
    std::vector<uint16_t> result;
    if(bytes) {result.resize(count*4);std::memcpy(result.data(),bytes,count*8);}
    backend.ReleaseReadback(rb);return result;
  };
  auto result=dispatch(1);
  if(result.empty())return 60;
  const float tint[]={.5f,1.f,.25f};
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
    const float expected=std::max(Half(inputs[4*i+c]),0.f)*tint[c];
    const float value=Half(result[4*i+c]);
    // The chart's FP16 encoded knots introduce up to 0.5% error in a linear
    // secant; this remains far below a gamma-domain continuation's error.
    if(!std::isfinite(value)||std::abs(value-expected)>std::max(.001f,expected*.005f)) {
      std::printf("FAIL linear-unit LUT extension pixel %u channel %u: %g != %g\n",i,c,value,expected);return 61;
    }
  }
  // Fixture curve is (x/(x+exposure))^2.2. Compare the continuation to its
  // analytic derivative independently of the shader's numerical derivative.
  const float pivot=.18f;
  const float atPivot=std::pow(pivot/(pivot+exposure),2.2f);
  const float slope=2.2f*std::pow(pivot/(pivot+exposure),1.2f)*exposure/
      ((pivot+exposure)*(pivot+exposure));
  result=dispatch(2);if(result.empty())return 60;
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
    const float x=std::max(Half(inputs[4*i+c]),0.f),bounded=std::min(x,pivot);
    const float expected=(std::pow(bounded/(bounded+exposure),2.2f)+slope*std::max(x-pivot,0.f))/65536.f;
    const float value=Half(result[4*i+c]);
    if(!std::isfinite(value)||std::abs(value-expected)>std::max(1e-7f,expected*.003f)) {
      std::printf("FAIL filmic tangent pixel %u channel %u: %g != %g\n",i,c,value,expected);return 62;
    }
  }
  result=dispatch(3);if(result.empty())return 60;
  if(std::abs(Half(result[0])-slope)>slope*.01f ||
      std::abs(Half(result[1])-slope)>slope*.01f ||
      std::abs(Half(result[2])-atPivot)>.001f) {
    std::puts("FAIL filmic tangent value/slope continuity");return 63;
  }
  auto continuous=[&]() {
    const auto seam=dispatch(4);
    return !seam.empty() && std::abs(Half(seam[0])-Half(seam[1]))<.002f &&
        std::abs(Half(seam[2])-Half(seam[1]))<.002f;
  };
  if(!continuous()) {std::puts("FAIL LUT boundary continuity");return 64;}
  // A clipped final cell must join smoothly and recover a useful earlier
  // gradient. This exercises actual LUT loads, gamma decode and extrapolation.
  auto chart=originalLut;
  for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)
    chart[4*(15+16*(y+16*z))]=chart[4*(14+16*(y+16*z))];
  backend.UploadTexture(lut,0,0,chart.data(),16*8,16,16);
  if(!continuous()) {std::puts("FAIL flat-cell LUT boundary continuity");return 65;}
  result=dispatch(1);if(result.empty())return 60;
  if(Half(result[4*20])<1.f) {
    std::puts("FAIL LUT interior slope recovery beyond flat final cell");return 66;
  }
  // No useful gradient anywhere: a constant LUT must remain constant.
  for(unsigned i=0;i<16*16*16;++i)for(unsigned c=0;c<3;++c)chart[4*i+c]=ToHalf(.5f);
  backend.UploadTexture(lut,0,0,chart.data(),16*8,16,16);
  result=dispatch(1);if(result.empty())return 60;
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)
    if(std::abs(Half(result[4*i+c])-std::pow(.5f,2.2f))>.001f) {
      std::puts("FAIL constant LUT acquired invented HDR gradient");return 67;
    }
  backend.UploadTexture(lut,0,0,originalLut.data(),16*8,16,16);
  std::puts("PASS curve/LUT extension: analytic tangent, linear-unit identity, boundary continuity, flat-cell recovery, constant-chart stability");
  return 0;
}

template<class Pipeline, class Texture>
static int Gears3BoostCheck(gpu::Backend& backend, Pipeline pipeline, Texture input,
    Texture lut, Texture output, unsigned count, const uint16_t* inputs) {
  auto dispatch=[&](unsigned mode,float peak) {
    gpu::ComputeBindings b;b.constant_count=8;
    b.constants[0]=std::bit_cast<uint32_t>(peak);b.constants[1]=mode;
    b.constants[4]=std::bit_cast<uint32_t>(1.f);
    b.srv[0]=input;b.srv[1]=lut;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);
    const auto rb=backend.ReadbackTexture(output,{0,0,static_cast<int32_t>(count),1});
    backend.WaitForFence(backend.Submit());uint32_t pitch=0;
    const auto* bytes=backend.ReadbackData(rb,pitch);std::vector<uint16_t> result;
    if(bytes){result.resize(count*4);std::memcpy(result.data(),bytes,count*8);}
    backend.ReleaseReadback(rb);return result;
  };
  const auto gain=dispatch(5,4000.f/203.f);if(gain.empty())return 90;
  // Golden gains from the upstream scalar implementation at these FP16 input
  // values. In particular Reinhard(x,.4) tends to .4, not to 1.
  const unsigned index[]={2,3,4,5,6,7};
  const float expected[]={1.04358643f,1.f,1.25818684f,1.71824755f,2.34099808f,7.585821f};
  for(unsigned i=0;i<6;++i)for(unsigned c=0;c<3;++c) {
    if(std::abs(Half(gain[4*index[i]+c])-expected[i])>expected[i]*.002f) {
      std::printf("FAIL RenoDX boost reference vector %u: %g != %g\n",index[i],Half(gain[4*index[i]+c]),expected[i]);return 91;
    }
  }
  for(unsigned i=0;i<count;++i) {
    float common=0,y=0;const float weights[]={.2126f,.7152f,.0722f};
    for(unsigned c=0;c<3;++c)y+=std::max(Half(inputs[4*i+c]),0.f)*weights[c];
    for(unsigned c=0;c<3;++c) {
      const float value=Half(gain[4*i+c]);
      if(!std::isfinite(value)||value<.999f||(y<=.04f && std::abs(value-1.f)>.001f))return 92;
      if(Half(inputs[4*i+c])>0) {
        if(common && std::abs(value-common)>std::max(.004f,value*.002f))return 93;
        common=value;
      }
    }
  }
  for(float peak:{1000.f/203.f,4000.f/203.f}) {
    const auto before=dispatch(6,peak),after=dispatch(7,peak);
    if(before.empty()||after.empty())return 90;
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
      const float value=Half(after[4*i+c]),old=Half(before[4*i+c]);
      if(!std::isfinite(value)||value<0||value>peak+peak/1024.f)return 94;
      const float maximum=std::max({Half(inputs[4*i]),Half(inputs[4*i+1]),Half(inputs[4*i+2])});
      if(maximum<=.5f && std::abs(value-old)>.001f)return 95;
    }
    auto y=[&](const auto& values,unsigned i) {
      return .2126f*Half(values[4*i])+.7152f*Half(values[4*i+1])+.0722f*Half(values[4*i+2]);
    };
    unsigned measured=4;float bestGain=0;
    for(unsigned i:{4u,18u,19u,20u,21u,5u,6u}) {
      const float oldY=y(before,i),newY=y(after,i);
      // A peak-limited emitter cannot brighten further. Measure a finite
      // highlight that has display headroom, rather than demanding clipping.
      if(oldY>.04f && oldY<peak*.6f && newY/oldY>bestGain) {
        bestGain=newY/oldY;measured=i;
      }
    }
    if(peak>19.f && bestGain<1.02f) {
      std::puts("FAIL Gears 3 boost did not extend finite LUT highlight range");return 96;
    }
    std::printf("PASS Gears 3 RenoDX boost peak %.0f nits: finite LUT highlight %.1f -> %.1f nits; SDR low range, chromaticity and bounds\n",
        peak*203.f,y(before,measured)*203.f,y(after,measured)*203.f);
    const auto display=dispatch(10,peak);if(display.empty())return 90;
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
      const float value=Half(display[4*i+c]);
      if(!std::isfinite(value)||value<0||value>peak+peak/1024.f)return 98;
    }
    if(std::abs(Half(display[4*2+1])-.18f)>.001f||
        std::abs(Half(display[4*3+1])-.01f)>.001f||
        (peak>19.f && (Half(display[4*5+1])<peak*.95f||Half(display[4*19+1])>peak*.4f))) {
      std::puts("FAIL Gears 3 selected-peak/anchor calibration");return 99;
    }
    std::printf("PASS Gears 3 peak calibration %.0f nits: strong neutral %.1f nits, modest %.1f nits, grey anchor stable\n",
        peak*203.f,Half(display[4*5+1])*203.f,Half(display[4*19+1])*203.f);
  }
  return 0;
}
