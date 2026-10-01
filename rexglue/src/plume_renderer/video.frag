#version 450

// A decoded video frame, turned from its planes into RGB: BT.601 studio
// range, the coefficients the CPU conversion uses. Converting on the CPU cost
// 8 ms a frame on a laptop.
//
// Each plane arrives as its bytes packed four to an RGBA8 texel: nothing is
// widened on the CPU, and no R8 texture is made (an earlier attempt with R8,
// and with this conversion inside passthrough.frag, lost the device on AMD's
// Windows driver). The slots are the same for the whole draw, so they index
// the texture array uniformly.

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec4 v_planes;

layout(location = 0) out vec4 o_color;

// The draw's descriptor set (plume_draw.cpp, DrawSetFor): luma in 2D slot 0, Cb (or packed
// Cb,Cr) in 1, Cr in 2.
layout(set = 0, binding = 3) uniform texture2D g_textures[16];
layout(set = 0, binding = 6) uniform sampler g_samplers[16];

// One byte of a plane: `stride` bytes per sample (1, or 2 for packed Cb,Cr),
// `lane` which of them.
float PlaneByte(texture2D plane, ivec2 at, int stride, int lane, ivec2 size) {
    at = clamp(at, ivec2(0), size - 1);
    int byte_x = at.x * stride + lane;
    vec4 texel = texelFetch(sampler2D(plane, g_samplers[0]), ivec2(byte_x >> 2, at.y), 0);
    int c = byte_x & 3;
    return c == 0 ? texel.r : (c == 1 ? texel.g : (c == 2 ? texel.b : texel.a));
}

// Bilinear, by hand: the sampler cannot filter bytes packed into texels.
float SamplePlane(texture2D plane, int stride, int lane) {
    ivec2 texels = textureSize(sampler2D(plane, g_samplers[0]), 0);
    ivec2 size = ivec2(texels.x * 4 / stride, texels.y);
    vec2 pos = v_uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(pos));
    vec2 f = pos - floor(pos);
    float a = PlaneByte(plane, i, stride, lane, size);
    float b = PlaneByte(plane, i + ivec2(1, 0), stride, lane, size);
    float c = PlaneByte(plane, i + ivec2(0, 1), stride, lane, size);
    float d = PlaneByte(plane, i + ivec2(1, 1), stride, lane, size);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    float y = SamplePlane(g_textures[0], 1, 0);
    vec2 cbcr;
    if (v_planes.w >= 3u) {
        cbcr = vec2(SamplePlane(g_textures[1], 2, 0), SamplePlane(g_textures[1], 2, 1));
    } else {
        cbcr = vec2(SamplePlane(g_textures[1], 1, 0), SamplePlane(g_textures[2], 1, 0));
    }
    float c = (y - 16.0 / 255.0) * (298.0 / 256.0);
    float cb = cbcr.x - 128.0 / 255.0;
    float cr = cbcr.y - 128.0 / 255.0;
    vec3 rgb = vec3(c + (409.0 / 256.0) * cr,
                    c - (100.0 / 256.0) * cb - (208.0 / 256.0) * cr,
                    c + (516.0 / 256.0) * cb);
    o_color = vec4(clamp(rgb, 0.0, 1.0) * v_color.rgb, v_color.a);
}
