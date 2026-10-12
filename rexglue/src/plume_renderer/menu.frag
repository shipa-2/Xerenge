#version 450

// The in-game menu's pixels: the font atlas (or its white pixel, for the
// rectangles) times the vertex colour.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D g_atlas;
layout(set = 0, binding = 1) uniform sampler g_sampler;

void main() {
    o_color = v_color * texture(sampler2D(g_atlas, g_sampler), v_uv);
}
