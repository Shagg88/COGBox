// Authored 3D-LUT/interpolation and analytic continuation properties, no game data.
static int Ue3GradeBridgeCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  constexpr unsigned count=20;
  const float points[count][3]={{.2f,.5f,.8f},{.2f,.8f,.5f},{.5f,.2f,.8f},
      {.5f,.8f,.2f},{.8f,.2f,.5f},{.8f,.5f,.2f},
      {0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1},
      {-.2f,1.2f,.5f},{.18f,.18f,.18f},{.5f,.5f,.5f},{1,1,1},{100,100,100},{65000,65000,65000}};
  const std::string source=std::string(hdr::kPsycho35Hlsl)+hdr::kUe3GradeHlsl+R"(
cbuffer Params : register(b0) { uint mode; };
Texture2D<float4> points : register(t0);
Texture3D<float4> lut : register(t1);
RWTexture2D<float4> output : register(u0);
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if(id.x>=20) return;
  float3 p=points.Load(int3(id.xy,0)).rgb;
  if(mode==0) output[id.xy]=NativeUe3Tetrahedral(lut,(p+0.5)/2.0);
  else if(mode==1) {
    p=max(p,0.0);
    output[id.xy]=float4(NativeUe3TangentPower(p,1.0.xxx,0.75.xxx),1);
  }
  else if(mode>=5) {
    float exponent=mode==5?2.0:0.25;
    float p=0.75,h=0.001;
    float a=NativeUe3TangentPower((p-h).xxx,exponent.xxx,p.xxx).x;
    float b=NativeUe3TangentPower(p.xxx,exponent.xxx,p.xxx).x;
    float c=NativeUe3TangentPower((p+h).xxx,exponent.xxx,p.xxx).x;
    output[id.xy]=float4((b-a)/h,(c-b)/h,b,1);
  }
  else {
    float3 weighted=max(p,0.0);
    float reciprocal;
    uint test=id.x%6;
    if(test==0) reciprocal=asfloat(0x7f800000u);
    else if(test==1) reciprocal=asfloat(0x7fc00000u);
    else if(test==2) reciprocal=-1.0;
    else if(test==3) reciprocal=0.0;
    else if(test==4) { reciprocal=asfloat(0x7f7fffffu); weighted=float3(4,2,1); }
    else { reciprocal=1e20; weighted*=1e-20; }
    float3 normalized=NativeNormalizeScene(weighted,reciprocal,0.0);
    if(mode==2) output[id.xy]=float4(normalized,1);
    else if(mode==4) output[id.xy]=float4(NativeNormalizeScene(weighted,reciprocal,
        0.01.xxx,float3(.3,.5,.1)),1);
    else output[id.xy]=float4(NativeUe3MapExtendedGrade(test==5?0.0:normalized,
        0.0001.xxx,float3(.3,.5,.1),19.0,1.0),1);
  }
}
)";
  std::vector<uint8_t> bytes;std::string error;
  if(!compiler.Compile(source,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error)) {
    std::printf("FAIL UE3 grading compile: %s\n",error.c_str());return 50;
  }
  const auto shader=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto pipeline=shader?backend.CreateComputePipeline(shader):0;
  gpu::TextureDesc d;d.width=count;d.height=1;d.format=gpu::Format::kR16G16B16A16Float;
  d.usage=gpu::kUsageSampled|gpu::kUsageStorage;
  const auto input=backend.CreateTexture(d),output=backend.CreateTexture(d);
  d.dimension=gpu::TextureDimension::k3D;d.width=d.height=d.depth_or_layers=2;d.usage=gpu::kUsageSampled;
  const auto lut=backend.CreateTexture(d);
  if(!pipeline||!input||!output||!lut)return 51;
  uint16_t inputData[count*4]{},lutData[8*4]{};
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)inputData[i*4+c]=ToHalf(points[i][c]);
  // RGB is an identity/affine chart. Alpha is 1 only at the white corner:
  // tetrahedral interpolation is min(R,G,B), whereas trilinear is R*G*B.
  for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x) {
    const unsigned i=x+2*(y+2*z);lutData[4*i]=ToHalf(float(x));lutData[4*i+1]=ToHalf(float(y));
    lutData[4*i+2]=ToHalf(float(z));lutData[4*i+3]=ToHalf(float(x*y*z));
  }
  backend.UploadTexture(input,0,0,inputData,count*8,1,1);
  backend.UploadTexture(lut,0,0,lutData,2*8,2,2);
  for(unsigned mode=0;mode<7;++mode) {
    gpu::ComputeBindings b;b.constants[0]=mode;b.constant_count=4;b.srv[0]=input;b.srv[1]=lut;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);
    const auto rb=backend.ReadbackTexture(output,{0,0,count,1});backend.WaitForFence(backend.Submit());
    uint32_t pitch=0;const auto* result=backend.ReadbackData(rb,pitch);uint16_t actual[count*4]{};
    if(result)std::memcpy(actual,result,sizeof(actual));backend.ReleaseReadback(rb);if(!result)return 52;
    for(unsigned i=0;i<count;++i) {
      if(mode>=5) {
        const float e=mode==5?2.f:.25f;
        const float value=std::pow(.75f,e),slope=e*std::pow(.75f,e-1.f);
        if(std::abs(Half(actual[4*i])-slope)>.004f||std::abs(Half(actual[4*i+1])-slope)>.004f||
            std::abs(Half(actual[4*i+2])-value)>.001f) {
          std::puts("FAIL analytic tangent value/slope continuity");return 57;
        }
        continue;
      }
      float expectedAlpha=1.f;
      for(unsigned c=0;c<3;++c) {
        const float inputValue=Half(inputData[4*i+c]);
        const float fallback[]={.3f,.5f,.1f};
        const float expected=mode==4?(i%6==5?std::max(inputValue-.01f,0.f):fallback[c]-.01f):
            mode==3?fallback[c]:mode==2?(i%6==5?std::max(inputValue,0.f):0.f):
            mode==1?std::max(inputValue,0.f):std::clamp(inputValue,0.f,1.f);
        if(!mode)expectedAlpha=std::min(expectedAlpha,expected);
        const float value=Half(actual[4*i+c]);
        if(!std::isfinite(value)||std::abs(value-expected)>std::max(.001f,std::abs(expected)*.0015f)) {
          std::printf("FAIL UE3 mode %u pixel %u channel %u: %g != %g\n",mode,i,c,value,expected);return 53;
        }
      }
      const float alpha=Half(actual[4*i+3]);
      if((!mode&&std::abs(alpha-expectedAlpha)>.001f)||(mode&&(!(alpha>0)||alpha>1)))return 54;
      if(mode==1 && i==16 && alpha!=1.f)return 55;
    }
  }
  backend.DestroyTexture(input);backend.DestroyTexture(output);backend.DestroyTexture(lut);
  std::puts("PASS UE3 grading: all 6 tetrahedra, LUT identity/centres/corners/edge clamp, nonlinear interpolation, neutral tangent identity, invalid edge normalization fallback and valid tiny coverage");
  return 0;
}
