// Fullscreen copy for D3D12 (Xbox swapchain images cannot be COPY_DEST).
// One oversized triangle from SV_VertexID, the scene sampled at its UVs.
//
//   fxc /T vs_5_1 /E VSMain /O3 /Fh plume_copy_vs_dxbc.h /Vn kCopyVsDxbc copy.hlsl
//   fxc /T ps_5_1 /E PSMain /O3 /Fh plume_copy_ps_dxbc.h /Vn kCopyPsDxbc copy.hlsl

Texture2D<float4> g_Texture : register(t0, space0);
SamplerState g_Sampler : register(s1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    return g_Texture.Sample(g_Sampler, i.uv);
}
