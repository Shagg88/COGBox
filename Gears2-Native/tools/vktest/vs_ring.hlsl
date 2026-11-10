// Positions from the upload ring (t97, patch 0078 UploadTransient): float2 per vertex at the byte offset in b1.x.
[[vk::binding(97, 1)]] ByteAddressBuffer xe_upload_memory : register(t97, space0);
cbuffer XeRingOffset : register(b1) { uint4 xe_off; };
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 p = asfloat(xe_upload_memory.Load2(xe_off.x + id * 8u));
  return float4(p, 0.5, 1.0);
}
