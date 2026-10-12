#version 450

// Contrast adaptive sharpening, after AMD FidelityFX CAS (MIT licence,
// Copyright (c) 2019 Advanced Micro Devices, Inc.): a sharpening kernel over
// the pixel's four neighbours whose strength follows how much headroom the
// neighbourhood leaves before it clips - strong where the picture is soft and
// flat, weak where it is already contrasty, so edges do not ring and bright
// areas do not blow out. CAS's filter shape:
//
//     0 w 0
//     w 1 w
//     0 w 0
//
// with w between -1/8 (sharpness 0) and -1/5 (sharpness 1), scaled per
// channel by that headroom.

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D g_scene;
layout(set = 0, binding = 1) uniform sampler g_sampler;

layout(push_constant) uniform Params {
    vec2 texel;       // unused here; shared with fxaa.frag
    float sharpness;  // 0..1
} u;

vec3 Fetch(ivec2 p, ivec2 size) {
    return texelFetch(sampler2D(g_scene, g_sampler), clamp(p, ivec2(0), size - 1), 0).rgb;
}

void main() {
    ivec2 size = textureSize(sampler2D(g_scene, g_sampler), 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    //  a b c
    //  d e f
    //  g h i
    vec3 a = Fetch(p + ivec2(-1, -1), size);
    vec3 b = Fetch(p + ivec2(0, -1), size);
    vec3 c = Fetch(p + ivec2(1, -1), size);
    vec3 d = Fetch(p + ivec2(-1, 0), size);
    vec4 centre = texelFetch(sampler2D(g_scene, g_sampler), p, 0);
    vec3 e = centre.rgb;
    vec3 f = Fetch(p + ivec2(1, 0), size);
    vec3 g = Fetch(p + ivec2(-1, 1), size);
    vec3 h = Fetch(p + ivec2(0, 1), size);
    vec3 i = Fetch(p + ivec2(1, 1), size);

    // Soft minimum and maximum: the cross's plus the whole square's, so twice
    // their average.
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(min(min(mn, a), c), min(g, i));
    mn += mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(max(max(mx, a), c), max(g, i));
    mx += mx2;

    // The distance to the nearer end of the range over the maximum, shaped.
    vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    float peak = -1.0 / mix(8.0, 5.0, clamp(u.sharpness, 0.0, 1.0));
    vec3 w = amp * peak;
    vec3 sharpened = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    o_color = vec4(clamp(sharpened, 0.0, 1.0), centre.a);
}
