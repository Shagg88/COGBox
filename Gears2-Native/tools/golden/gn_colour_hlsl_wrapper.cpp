// Writes every colour_hlsl.h snippet plus a cs_6_6 entry point that calls each function, for a DXC compile check:
//   g++ -std=c++23 -I<repo>/src/layer0_xenos/include gn_colour_hlsl_wrapper.cpp -o w && ./w > c.hlsl
//   dxc -T cs_6_6 -E main -Gis c.hlsl -Fo c.dxil
// Our code (MIT).
#include <cstdio>
#include "xenos/renderer/colour_hlsl.h"
int main() {
  for (const char* s : xenos::renderer::hlsl::kHlslColourAll) std::fputs(s, stdout);
  std::fputs(R"(
ByteAddressBuffer xe_in : register(t0);
RWByteAddressBuffer xe_out : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint a = xe_in.Load(id.x * 64u), b = xe_in.Load(id.x * 64u + 4u), c = xe_in.Load(id.x * 64u + 8u);
  float f = asfloat(a);
  bool canary = (c & 1u) != 0u;
  uint o = 0u;
  o ^= XeFloat32To7e3(f) ^ asuint(XeFloat7e3To32(a)) ^ XePack7e3(asfloat(xe_in.Load4(16u))) ^ XeFloatToAlpha2(f);
  o ^= asuint(XeUnpack7e3(b).x) ^ asuint(XeClamp7e3RenderTargetRange(asfloat(xe_in.Load4(32u)), canary).y);
  o ^= XeHost7e3Fp16ToGuest(uint2(a, b)) ^ XeGuest7e3ToHostFp16(a).y;
  o ^= asuint(XeFixed16ResolveDecode(a, canary)) ^ XeFixed16GuestEncode(f);
  o ^= XePackFixed(f, b & 15u, c >> 4u, canary);
  uint2 r = XeColourResolvePixel(a & 15u, xe_in.Load4(16u), xe_in.Load4(32u), b & 7u, c, canary, (c & 2u) != 0u);
  o ^= r.x ^ r.y ^ (XeColourResolveIsRaw(a & 15u, c, (b & 1u) != 0u, canary) ? 1u : 0u);
  o ^= XeDepthStencilFromColourWord(a).x ^ XeColourWordFromDepthStencil(a, b) ^ XeSwapTileHalfColumns(a & 127u, b & 3u, canary, c & 3u);
  xe_out.Store(id.x * 4u, o);
}
)", stdout);
}
