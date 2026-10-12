#version 450

// The in-game menu's vertices (plume_menu_draw.cpp): Dear ImGui's draw lists,
// in pixels from the top left, put on the clip space by a scale and a shift.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform Transform {
    vec2 scale;
    vec2 shift;
} u_transform;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = vec4(in_pos * u_transform.scale + u_transform.shift, 0.0, 1.0);
}
