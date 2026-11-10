// Positions from the guest-memory buffer (t96): float2 per vertex at byte 0x1000.
[[vk::binding(96, 1)]] ByteAddressBuffer xe_shared_memory : register(t96, space0);
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 p = asfloat(xe_shared_memory.Load2(0x1000u + id * 8u));
  return float4(p, 0.5, 1.0);
}
