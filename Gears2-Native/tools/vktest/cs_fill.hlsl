// Fills mip c0.y of a storage image (u0, R32_UINT) with c0.x + x + y * 16.
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { xe_dest[id.xy] = c0.x + id.x + id.y * 16u; }
