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
    // Pass the vertex colour through untouched. The C++ side (FillVertices in
    // plume_draw.cpp) already knows for certain whether a real COLOR0
    // attribute was bound (HasColorAttr) - when it wasn't, it fills this slot
    // itself from a resolved UI tint constant, or opaque white if no tint
    // constant could be found either (ResolveUiTint / the fallback beside its
    // call site). Guessing here from the colour's magnitude/alpha cannot tell
    // "no attribute, defaulted to black" from "a real, legitimately dark
    // colour" (e.g. a genuine black drop-shadow with its own alpha), so it
    // was liable to paint a white halo around exactly that case.
    v_color = in_color;
    v_uv = in_uv.xy;
    v_texid = uint(in_texid.x + 0.5);
}
