#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_texid;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D g_textures[1024];
layout(set = 3, binding = 0) uniform sampler g_samplers[16];

void main() {
    uint texid = min(v_texid, 1023u);
    vec4 tex = texture(nonuniformEXT(sampler2D(g_textures[texid], g_samplers[0])), v_uv);
    if (tex.a < 0.5) {
        discard;
    }
    o_color = vec4(tex.rgb * v_color.rgb, tex.a);
}
