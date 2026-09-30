#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_texid;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D g_textures[4096];
layout(set = 3, binding = 0) uniform sampler g_samplers[16];

void main() {
    uint texid = min(v_texid, 4095u);
    vec4 tex = texture(nonuniformEXT(sampler2D(g_textures[texid], g_samplers[0])), v_uv);
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
