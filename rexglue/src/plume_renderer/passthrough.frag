#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_texid;

layout(location = 0) out vec4 o_color;

// The draw's descriptor set (plume_draw.cpp, DrawSetFor): its texture in 2D slot 0.
layout(set = 0, binding = 3) uniform texture2D g_textures[16];
layout(set = 0, binding = 6) uniform sampler g_samplers[16];

void main() {
    vec4 tex = texture(sampler2D(g_textures[0], g_samplers[0]), v_uv);
    // Keep the texture's own alpha ramp. Clipping it to a binary coverage
    // mask was a workaround for the outline pass bleeding a white halo
    // through soft edges - that came from the outline's black tint being
    // rejected and repainted white, and is fixed at the source now. The ramp
    // is what makes glints and soft sprite edges soft, and what antialiases
    // glyph edges, so squaring it off turned the logo's glint into a hard
    // white blob.
    if (tex.a <= 0.0) {
        discard;
    }
    o_color = vec4(tex.rgb * v_color.rgb, tex.a * v_color.a);
}
