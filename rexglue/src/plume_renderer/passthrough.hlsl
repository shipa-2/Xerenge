// passthrough.vert / passthrough.frag for D3D12 (XERENGE_RENDER_API=d3d12).
// The same vertex layout (TEXCOORD0-3: position, texture index, UV, colour);
// the texture is the bindless one the vertex names, from the layout the game's
// shaders use on D3D12 (t0 space0, s0 space3).
//
//   dxc -T vs_6_0 -E VSMain -Fh plume_passthrough_vs_dxil.h -Vn kPassthroughVsDxil passthrough.hlsl
//   dxc -T ps_6_0 -E PSMain -Fh plume_passthrough_ps_dxil.h -Vn kPassthroughPsDxil passthrough.hlsl
//
// and for D3D12 without DXIL (the Xbox One), shader model 5.1:
//
//   fxc /T vs_5_1 /E VSMain /O3 /enable_unbounded_descriptor_tables /Fh plume_passthrough_vs_dxbc.h /Vn kPassthroughVsDxbc passthrough.hlsl
//   fxc /T ps_5_1 /E PSMain /O3 /enable_unbounded_descriptor_tables /Fh plume_passthrough_ps_dxbc.h /Vn kPassthroughPsDxbc passthrough.hlsl

Texture2D<float4> g_Texture2D[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space3);

struct VSInput {
    float4 pos : TEXCOORD0;
    float4 texid : TEXCOORD1;
    float4 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    nointerpolation uint texid : TEXCOORD1;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    // The positions are prepared in Vulkan's clip space (y -1 at the top), as
    // the GLSL passes take them; D3D's has y +1 at the top.
    output.pos = float4(input.pos.x, -input.pos.y, input.pos.z, input.pos.w);
    // The vertex colour as FillVertices left it (see passthrough.vert).
    output.color = input.color;
    output.uv = input.uv.xy;
    output.texid = uint(input.texid.x + 0.5);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float4 tex = g_Texture2D[NonUniformResourceIndex(input.texid)].Sample(g_Samplers[0], input.uv);
    if (tex.a <= 0.0) {
        discard;
    }
    return float4(tex.rgb * input.color.rgb, tex.a * input.color.a);
}
