// video.vert / video.frag for D3D12 (XERENGE_RENDER_API=d3d12): a decoded
// video frame turned from its planes into RGB, BT.601 studio range. Each plane
// arrives as its bytes packed four to an RGBA8 texel; the vertex names the
// planes' bindless indices (luma, Cb or packed Cb,Cr, Cr; w is 3 when packed).
//
//   dxc -T vs_6_0 -E VSMain -Fh plume_video_vs_dxil.h -Vn kVideoVsDxil video.hlsl
//   dxc -T ps_6_0 -E PSMain -Fh plume_video_ps_dxil.h -Vn kVideoPsDxil video.hlsl

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
    nointerpolation uint4 planes : TEXCOORD1;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    // The positions are prepared in Vulkan's clip space (y -1 at the top), as
    // the GLSL passes take them; D3D's has y +1 at the top.
    output.pos = float4(input.pos.x, -input.pos.y, input.pos.z, input.pos.w);
    output.color = input.color;
    output.uv = input.uv.xy;
    output.planes = uint4(input.texid + 0.5);
    return output;
}

// One byte of a plane: `stride` bytes per sample (1, or 2 for packed Cb,Cr),
// `lane` which of them.
float PlaneByte(uint plane, int2 at, int stride, int lane, int2 size) {
    at = clamp(at, int2(0, 0), size - 1);
    int byteX = at.x * stride + lane;
    float4 texel = g_Texture2D[NonUniformResourceIndex(plane)].Load(int3(byteX >> 2, at.y, 0));
    int c = byteX & 3;
    return c == 0 ? texel.r : (c == 1 ? texel.g : (c == 2 ? texel.b : texel.a));
}

// Bilinear, by hand: the sampler cannot filter bytes packed into texels.
float SamplePlane(uint plane, float2 uv, int stride, int lane) {
    uint w, h;
    g_Texture2D[NonUniformResourceIndex(plane)].GetDimensions(w, h);
    int2 size = int2(int(w) * 4 / stride, int(h));
    float2 pos = uv * float2(size) - 0.5;
    int2 i = int2(floor(pos));
    float2 f = pos - floor(pos);
    float a = PlaneByte(plane, i, stride, lane, size);
    float b = PlaneByte(plane, i + int2(1, 0), stride, lane, size);
    float c = PlaneByte(plane, i + int2(0, 1), stride, lane, size);
    float d = PlaneByte(plane, i + int2(1, 1), stride, lane, size);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float y = SamplePlane(input.planes.x, input.uv, 1, 0);
    float2 cbcr;
    if (input.planes.w >= 3u) {
        cbcr = float2(SamplePlane(input.planes.y, input.uv, 2, 0), SamplePlane(input.planes.y, input.uv, 2, 1));
    } else {
        cbcr = float2(SamplePlane(input.planes.y, input.uv, 1, 0), SamplePlane(input.planes.z, input.uv, 1, 0));
    }
    float c = (y - 16.0 / 255.0) * (298.0 / 256.0);
    float cb = cbcr.x - 128.0 / 255.0;
    float cr = cbcr.y - 128.0 / 255.0;
    float3 rgb = float3(c + (409.0 / 256.0) * cr,
                        c - (100.0 / 256.0) * cb - (208.0 / 256.0) * cr,
                        c + (516.0 / 256.0) * cb);
    return float4(saturate(rgb) * input.color.rgb, input.color.a);
}
