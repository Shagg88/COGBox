// Copies a 4x2 R32_UINT texture (t0) into guest memory (u0) at c0.x, byte-swapped as the guest stores words.
Texture2D<uint> xe_words : register(t0);
RWByteAddressBuffer xe_guest : register(u0);
cbuffer XeWordsConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(4, 2, 1)] void main(uint3 id : SV_DispatchThreadID) {
  uint v = xe_words.Load(int3(id.xy, 0));
  v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  xe_guest.Store(c0.x + (id.y * 4u + id.x) * 4u, v);
}
