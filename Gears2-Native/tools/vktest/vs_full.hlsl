// A rectangle from 6 vertices (two triangles): x0, y0, x1, y1 in NDC from the vertex shader's b1, depth in b1[1].x.
cbuffer XeVsRect : register(b1) { float4 rect; float4 depth; };
float4 main(uint id : SV_VertexID) : SV_Position {
  const uint corner[6] = {0u, 1u, 2u, 2u, 1u, 3u};
  uint c = corner[id % 6u];
  float2 uv = float2(float(c & 1u), float(c >> 1u));
  return float4(lerp(rect.xy, rect.zw, uv), depth.x, 1.0);
}
