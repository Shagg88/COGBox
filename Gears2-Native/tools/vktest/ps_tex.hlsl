[[vk::binding(3, 1)]] Texture2D<float4> xe_tex_2d_3 : register(t3, space0);
[[vk::binding(3, 2)]] SamplerState xe_sampler_3 : register(s3, space0);
cbuffer XePsColour : register(b0) { float4 size; };
float4 main(float4 pos : SV_Position) : SV_Target0 { return xe_tex_2d_3.Sample(xe_sampler_3, pos.xy / size.xy); }
