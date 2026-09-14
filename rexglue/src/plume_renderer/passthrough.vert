#version 450

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_texid;
layout(location = 13) in vec4 in_uv;
layout(location = 17) in vec4 in_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_texid;

void main() {
    gl_Position = in_pos;
    vec3 c = in_color.rgb;
    float mag = max(max(abs(c.r), abs(c.g)), abs(c.b));
    v_color = mag < 0.04 ? vec4(1.0) : vec4(c, 1.0);
    v_uv = in_uv.xy;
    v_texid = uint(in_texid.x + 0.5);
}
