cbuffer XePsColour : register(b0) { float4 colour; };
float4 main(float4 pos : SV_Position) : SV_Target0 { return colour; }
