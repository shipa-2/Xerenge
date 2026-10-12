#version 450

// One triangle covering the target, from the vertex index alone: the post
// passes (plume_postfx.cpp) draw without a vertex buffer. v_uv runs 0..1
// across the target, top left to bottom right as the texture is stored.

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
