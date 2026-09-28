#version 450

// A decoded video frame, drawn from its planes (video.frag). The same vertex
// layout as passthrough.vert; the texture lane carries the planes' slots.

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_texid;
layout(location = 13) in vec4 in_uv;
layout(location = 17) in vec4 in_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
// Luma, Cb (or packed Cb,Cr), Cr slots, and 3 when the chroma is packed.
layout(location = 2) flat out uvec4 v_planes;

void main() {
    gl_Position = in_pos;
    v_color = in_color;
    v_uv = in_uv.xy;
    v_planes = uvec4(in_texid + 0.5);
}
