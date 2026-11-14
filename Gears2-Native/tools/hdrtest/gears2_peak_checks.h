// Calibrate display-range use with finite, representative highlight inputs,
// rather than testing only an FP16-max emitter. Uses the production functions.
template<class Pipeline, class Texture>
static int Gears2PeakCheck(gpu::Backend& backend, Pipeline pipeline, Texture input,
    Texture output, unsigned count, const uint16_t* inputData) {
  auto dispatch=[&](unsigned mode,float peak) {
    gpu::ComputeBindings b;b.constant_count=8;
    b.constants[0]=std::bit_cast<uint32_t>(peak);b.constants[1]=mode;
    b.constants[4]=std::bit_cast<uint32_t>(1.f);
    b.srv[0]=input;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);
    const auto rb=backend.ReadbackTexture(output,{0,0,static_cast<int32_t>(count),1});
    backend.WaitForFence(backend.Submit());
    uint32_t pitch=0;const auto* bytes=backend.ReadbackData(rb,pitch);
    std::vector<uint16_t> result;
    if(bytes){result.resize(count*4);std::memcpy(result.data(),bytes,count*8);}
    backend.ReleaseReadback(rb);return result;
  };
  float previousHighlight=0;
  for(float nits:{1000.f,4000.f,10000.f}) {
    const float peak=nits/203.f;
    const auto result=dispatch(5,peak);if(result.empty())return 70;
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
      const float value=Half(result[4*i+c]);
      if(!std::isfinite(value)||value<0||value>peak+peak/1024.f) {
        std::puts("FAIL Gears 2 display-range calibration bounds");return 71;
      }
    }
    // Diffuse grey, dark grey and SDR white must not move with display peak.
    for(unsigned i:{2u,3u,4u})for(unsigned c=0;c<3;++c) {
      const float expected=i==2?.18f:i==3?.01f:1.f;
      if(std::abs(Half(result[4*i+c])-expected)>.001f) {
        std::puts("FAIL Gears 2 peak changed SDR reference values");return 72;
      }
    }
    // A moderate flash must retain substantial headroom. Strong emitters can
    // still use the selected peak; 4-white surfaces must not be forced there.
    const float highlight=Half(result[4*21+1]);
    const float bright=Half(result[4*5+1]);
    const float emitter=Half(result[4*6+1]);
    if((nits==4000 && (highlight<4.f||highlight>peak*.65f||Half(result[4*19+1])>2.f)) || emitter<peak*.95f) {
      std::printf("FAIL Gears 2 flash/emitter separation: flash %g, modest %g, strong %g of %g\n",
          highlight,Half(result[4*19+1]),emitter,peak);return 73;
    }
    if(nits==4000 && (bright<peak*.8f||bright>peak*.98f)) {
      std::puts("FAIL Gears 2 bright-highlight range/detail separation");return 79;
    }
    if(highlight<=previousHighlight) {
      std::puts("FAIL Gears 2 selected peak did not increase highlight range");return 74;
    }
    previousHighlight=highlight;
    if(nits==4000) {
      const auto seam=dispatch(6,peak);if(seam.empty())return 70;
      if(std::abs(Half(seam[0])-1.f)>.002f||std::abs(Half(seam[1])-1.f)>.025f||Half(seam[2])!=1.f) {
        std::puts("FAIL Gears 2 expansion value/slope at SDR white");return 75;
      }
    }
    std::printf("PASS Gears 2 peak %g nits: moderate flash %.1f nits, modest light %.1f nits, bright highlight %.1f nits, strong emitter %.1f nits; diffuse/white stable\n",
        nits,highlight*203.f,Half(result[4*19+1])*203.f,bright*203.f,emitter*203.f);
  }
  // Extra gain must stay bounded, use luminance instead of a hot channel,
  // and stop growing with display peak once there is sufficient headroom.
  const auto lowPeakGain=dispatch(9,4.f),highPeakGain=dispatch(9,4000.f/203.f);
  if(lowPeakGain.empty()||highPeakGain.empty())return 70;
  for(unsigned i=0;i<count;++i) {
    const float rgb[]={std::max(Half(inputData[4*i]),0.f),std::max(Half(inputData[4*i+1]),0.f),
        std::max(Half(inputData[4*i+2]),0.f)};
    const float y=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
    float commonGain=0;
    for(unsigned c=0;c<3;++c) {
      const float gain=Half(highPeakGain[4*i+c]);
      // The GPU ratio and FP16 store can round x/x one half-float ULP below
      // unity even when the production function returns x unchanged.
      if(!std::isfinite(gain)||gain<.999f||gain>4.004f || gain!=Half(lowPeakGain[4*i+c]) ||
          (y<=1.f && std::abs(gain-1.f)>.001f)) {
        std::printf("FAIL Gears 2 bounded luminance boost pixel %u channel %u: Y %g, gain %g vs low-peak %g\n",
            i,c,y,gain,Half(lowPeakGain[4*i+c]));return 76;
      }
      if(rgb[c]>0) {
        if(commonGain && std::abs(gain-commonGain)>.004f) {
          std::puts("FAIL Gears 2 boost changed RGB chromaticity");return 77;
        }
        commonGain=gain;
      }
    }
  }
  std::puts("PASS Gears 2 flash control: 4x gain bound, luminance gate, common RGB gain, display-independent drive");
  const auto reference=dispatch(10,4000.f/203.f);if(reference.empty())return 70;
  const unsigned indices[]={2,3,4,5,6,7};
  const float golden[]={1.04358643f,1.f,1.25818684f,1.71824755f,2.34099808f,7.585821f};
  for(unsigned i=0;i<6;++i) {
    if(std::abs(Half(reference[4*indices[i]])-golden[i])>golden[i]*.002f) {
      std::puts("FAIL Gears 2 RenoDX boost upstream reference vector");return 78;
    }
  }
  std::puts("PASS Gears 2 RenoDX boost kernel: upstream scalar reference vectors");
  return 0;
}

// Compare the production color restoration with its hue-only predecessor.
template<class Pipeline, class Texture>
static int Gears2ColorCheck(gpu::Backend& backend, Pipeline pipeline, Texture input,
    Texture output, unsigned count) {
  auto dispatch=[&](unsigned mode,float peak) {
    gpu::ComputeBindings b;b.constant_count=8;
    b.constants[0]=std::bit_cast<uint32_t>(peak);b.constants[1]=mode;
    b.constants[4]=std::bit_cast<uint32_t>(1.f);b.srv[0]=input;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);
    const auto rb=backend.ReadbackTexture(output,{0,0,static_cast<int32_t>(count),1});
    backend.WaitForFence(backend.Submit());uint32_t pitch=0;
    const auto* bytes=backend.ReadbackData(rb,pitch);std::vector<uint16_t> result;
    if(bytes){result.resize(count*4);std::memcpy(result.data(),bytes,count*8);}
    backend.ReleaseReadback(rb);return result;
  };
  auto lab=[](float r,float g,float b) {
    const float l=std::cbrt(.4122214708f*r+.5363325363f*g+.0514459929f*b);
    const float m=std::cbrt(.2119034982f*r+.6806995451f*g+.1073969566f*b);
    const float s=std::cbrt(.0883024619f*r+.2817188376f*g+.6299787005f*b);
    return std::array<float,3>{.2104542553f*l+.7936177850f*m-.0040720468f*s,
        1.9779984951f*l-2.4285922050f*m+.4505937099f*s,
        .0259040371f*l+.7827717662f*m-.8086757660f*s};
  };
  const float grades[6][3]={{.9f,.5f,.1f},{.15f,.4f,.95f},
      {1,.2f,.01f},{.65f,.1f,.8f},{.5f,.5f,.5f},{0,0,0}};
  for(float peak:{1000.f/203.f,4000.f/203.f}) {
    const auto before=dispatch(8,peak),after=dispatch(7,peak);
    if(before.empty()||after.empty())return 80;
    for(unsigned i=0;i<count;++i) {
      const unsigned card=i%6,level=i/6;float oldRgb[3],rgb[3];
      for(unsigned c=0;c<3;++c) {
        oldRgb[c]=Half(before[4*i+c]);rgb[c]=Half(after[4*i+c]);
        if(!std::isfinite(rgb[c])||rgb[c]<0||rgb[c]>peak+peak/1024.f)return 81;
        if((level<2||card==4) && std::abs(rgb[c]-oldRgb[c])>std::max(.001f,oldRgb[c]*.002f)) {
          std::puts("FAIL SDR color restoration changed diffuse/neutral values");return 82;
        }
        if(card==5 && rgb[c]!=0)return 83;
      }
      if(level<2||card>=4)continue;
      const auto reference=lab(grades[card][0],grades[card][1],grades[card][2]);
      const auto oldLab=lab(oldRgb[0],oldRgb[1],oldRgb[2]),newLab=lab(rgb[0],rgb[1],rgb[2]);
      const float referenceC=std::hypot(reference[1],reference[2]);
      const float oldC=std::hypot(oldLab[1],oldLab[2]),newC=std::hypot(newLab[1],newLab[2]);
      const float target=referenceC/reference[0];
      const float oldError=std::abs(oldC/oldLab[0]-target),newError=std::abs(newC/newLab[0]-target);
      if(newError>oldError*.4f+.003f) {
        std::printf("FAIL SDR relative chroma recovery card %u level %u: error %g -> %g\n",card,level,oldError,newError);return 84;
      }
      const float cosine=(reference[1]*newLab[1]+reference[2]*newLab[2])/(referenceC*newC);
      if(cosine<.999f) {std::puts("FAIL SDR highlight hue changed");return 85;}
      const float oldPeak=std::max({oldRgb[0],oldRgb[1],oldRgb[2]}),newPeak=std::max({rgb[0],rgb[1],rgb[2]});
      if(newPeak<oldPeak*.9f) {std::puts("FAIL SDR color restoration lost HDR boost");return 86;}
      if(oldPeak<peak*.6f && newPeak<peak*.9f) {
        const float oldY=.2126f*oldRgb[0]+.7152f*oldRgb[1]+.0722f*oldRgb[2];
        const float newY=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
        if(std::abs(newY-oldY)>std::max(.002f,oldY*.005f)) {
          std::puts("FAIL SDR chroma restoration altered unbounded HDR luminance");return 87;
        }
      }
    }
  }
  std::puts("PASS Gears 2 highlight color: SDR relative chroma recovery, hue, neutral/diffuse/black, HDR boost and luminance/peak bounds");
  return 0;
}
