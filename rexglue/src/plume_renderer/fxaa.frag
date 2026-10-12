#version 450

// FXAA: fast approximate anti-aliasing, after FXAA 3.11 by Timothy Lottes
// (NVIDIA), the "quality" algorithm in its medium preset (12): find an edge
// from the luma contrast around the pixel, walk along it to both ends, and
// shift the sample across the edge by how far the pixel is from the nearer
// end; plus the lone-pixel blend for sub-pixel detail. Rewritten compactly
// for this renderer; NVIDIA's original is distributed under a BSD-style
// licence, and the method is theirs.
//
// Pixels that are not on an edge leave after five fetches. The threshold is
// what keeps flat areas - and, mostly, text, whose strokes have contrast but
// are too short to walk along - from being blurred.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D g_scene;
layout(set = 0, binding = 1) uniform sampler g_sampler;

layout(push_constant) uniform Params {
    vec2 texel;       // 1 / target size
    float sharpness;  // unused here; shared with cas.frag
} u;

// Contrast below the larger of these, in luma (0..1), is not an edge: the
// first absolute, the second a fraction of the neighbourhood's brightest.
const float kEdgeThresholdMin = 0.0625;
const float kEdgeThreshold = 0.125;
// How much of a lone bright or dark pixel's blend towards its neighbours is
// kept (0 none, 1 all).
const float kSubpixel = 0.75;
// Steps of the walk along the edge, in pixels each.
const int kSearchSteps = 8;
const float kStep[8] = float[8](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec4 Fetch(ivec2 p, ivec2 size) {
    return texelFetch(sampler2D(g_scene, g_sampler), clamp(p, ivec2(0), size - 1), 0);
}

float LumaAt(vec2 uv) {
    return Luma(textureLod(sampler2D(g_scene, g_sampler), uv, 0.0).rgb);
}

void main() {
    ivec2 size = textureSize(sampler2D(g_scene, g_sampler), 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centre = Fetch(p, size);
    float lumaM = Luma(centre.rgb);
    float lumaN = Luma(Fetch(p + ivec2(0, -1), size).rgb);
    float lumaS = Luma(Fetch(p + ivec2(0, 1), size).rgb);
    float lumaW = Luma(Fetch(p + ivec2(-1, 0), size).rgb);
    float lumaE = Luma(Fetch(p + ivec2(1, 0), size).rgb);

    float rangeMax = max(max(lumaM, lumaN), max(max(lumaS, lumaW), lumaE));
    float rangeMin = min(min(lumaM, lumaN), min(min(lumaS, lumaW), lumaE));
    float range = rangeMax - rangeMin;
    if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold)) {
        o_color = centre;
        return;
    }

    float lumaNW = Luma(Fetch(p + ivec2(-1, -1), size).rgb);
    float lumaNE = Luma(Fetch(p + ivec2(1, -1), size).rgb);
    float lumaSW = Luma(Fetch(p + ivec2(-1, 1), size).rgb);
    float lumaSE = Luma(Fetch(p + ivec2(1, 1), size).rgb);

    // Horizontal against vertical: which way the second derivative of luma is
    // stronger over the three rows and the three columns around the pixel.
    float edgeHorz = abs(-2.0 * lumaW + (lumaNW + lumaSW)) +
                     abs(-2.0 * lumaM + (lumaN + lumaS)) * 2.0 +
                     abs(-2.0 * lumaE + (lumaNE + lumaSE));
    float edgeVert = abs(-2.0 * lumaS + (lumaSW + lumaSE)) +
                     abs(-2.0 * lumaM + (lumaW + lumaE)) * 2.0 +
                     abs(-2.0 * lumaN + (lumaNW + lumaNE));
    bool horzSpan = edgeHorz >= edgeVert;

    // The two neighbours across the edge, and the one the edge is steeper
    // towards - the side the pixel is moved to.
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool steeper1 = abs(gradient1) >= abs(gradient2);
    float gradient = max(abs(gradient1), abs(gradient2));
    float lengthSign = horzSpan ? u.texel.y : u.texel.x;
    if (steeper1) {
        lengthSign = -lengthSign;
    }
    float lumaLocalAverage = 0.5 * ((steeper1 ? luma1 : luma2) + lumaM);

    // The lone-pixel blend: how far the pixel stands from the average of its
    // eight neighbours, as a fraction of the range, smoothed.
    float lumaAverage = ((lumaN + lumaS + lumaW + lumaE) * 2.0 +
                         (lumaNW + lumaNE + lumaSW + lumaSE)) * (1.0 / 12.0);
    float subpixC = clamp(abs(lumaAverage - lumaM) / range, 0.0, 1.0);
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;

    // Start on the boundary between the pixel and its neighbour across the
    // edge, and walk both ways along it until luma there has moved a quarter
    // of the gradient away from the pair's average - the end of the edge.
    vec2 pos = v_uv;
    vec2 along = horzSpan ? vec2(u.texel.x, 0.0) : vec2(0.0, u.texel.y);
    if (horzSpan) {
        pos.y += lengthSign * 0.5;
    } else {
        pos.x += lengthSign * 0.5;
    }
    float gradientScaled = gradient * 0.25;
    vec2 posN = pos - along * kStep[0];
    vec2 posP = pos + along * kStep[0];
    float endN = LumaAt(posN) - lumaLocalAverage;
    float endP = LumaAt(posP) - lumaLocalAverage;
    bool doneN = abs(endN) >= gradientScaled;
    bool doneP = abs(endP) >= gradientScaled;
    for (int i = 1; i < kSearchSteps && !(doneN && doneP); ++i) {
        if (!doneN) {
            posN -= along * kStep[i];
            endN = LumaAt(posN) - lumaLocalAverage;
            doneN = abs(endN) >= gradientScaled;
        }
        if (!doneP) {
            posP += along * kStep[i];
            endP = LumaAt(posP) - lumaLocalAverage;
            doneP = abs(endP) >= gradientScaled;
        }
    }

    float distN = horzSpan ? v_uv.x - posN.x : v_uv.y - posN.y;
    float distP = horzSpan ? posP.x - v_uv.x : posP.y - v_uv.y;
    bool nearerN = distN < distP;
    float endNearer = nearerN ? endN : endP;
    float span = distN + distP;
    float distNearer = nearerN ? distN : distP;

    // The edge only moves the pixel when the nearer end bends the way the
    // pixel itself is off the pair's average; otherwise the pixel is on the
    // edge's far side already.
    bool goodSpan = (endNearer < 0.0) != (lumaM - lumaLocalAverage < 0.0);
    float pixelOffset = goodSpan ? 0.5 - distNearer / span : 0.0;
    pixelOffset = max(pixelOffset, subpixF * subpixF * kSubpixel);

    vec2 uv = v_uv;
    if (horzSpan) {
        uv.y += pixelOffset * lengthSign;
    } else {
        uv.x += pixelOffset * lengthSign;
    }
    o_color = vec4(textureLod(sampler2D(g_scene, g_sampler), uv, 0.0).rgb, centre.a);
}
