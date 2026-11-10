// Patch 0088 benchmark (gn_vktest --bench): a constant- and buffer-heavy pixel shader, like the game's - 64 dynamically
// indexed float4 constants (b1) and 64 guest-memory loads (t96) per pixel; BENCH_PRECISE marks the sum precise (the
// SPIR-V gets NoContraction, as our DXBC -> SPIR-V shaders do everywhere).
cbuffer XePsColour : register(b0) { float4 size; };
cbuffer XeBench : register(b1) { float4 c[256]; };
[[vk::binding(96, 1)]] ByteAddressBuffer xe_shared_memory : register(t96, space0);
float4 main(float4 pos : SV_Position) : SV_Target0 {
  uint i = uint(pos.x) & 255u;
#ifdef BENCH_PRECISE
  precise float4 acc = 0.0;
#else
  float4 acc = 0.0;
#endif
  [loop] for (uint k = 0u; k < 64u; ++k) {
    float4 a = c[(i + k * 7u) & 255u];
    float4 g = asfloat(xe_shared_memory.Load4(((uint(pos.y) * 64u + k) & 4095u) * 16u));
    acc = acc * a + g;
  }
  return acc * size.z;
}
