// FXAA: one fullscreen pass over the composited frame, the cheapest of the port's antialiasing modes.
//
// Timothy Lottes' FXAA 3.11, the quality variant: the luma of a plus and its four corners says whether a pixel sits on an
// edge and which axis the edge runs along; a search along that axis finds both ends of the edge's span; the pixel then
// blends towards the average of the two ends, with the subpixel term (how far this pixel is from the span's ends) choosing
// where in the span it lands. No look-up tables and no extra targets, so it costs one pass and one texture read of the
// frame at each of its taps, but it also blurs the whole span: text and thin geometry go soft.
//
// The knobs are Lottes' defaults for the quality preset, kept as named constants because the artefact when one is off is
// recognisable (too high a threshold leaves stair steps, too low softens flat areas, a low cap eats single-pixel detail).
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
// xy: one texel in UV units (the taps' spacing); zw: unused (a spare pair the host leaves zero)
uniform vec4 u_fxaaPt;

// How much local contrast a pixel needs before it counts as an edge, relative to the brightest luma around it, and the
// floor below which a dark area is left alone (FXAA's own "don't process darks" trim)
#define FXAA_EDGE_THRESHOLD (1.0 / 8.0)
#define FXAA_EDGE_THRESHOLD_MIN (1.0 / 24.0)
// How far out along the edge the search may walk before it gives up
#define FXAA_SEARCH_STEPS 12
// The step at which the walk decides it has left the edge
#define FXAA_SEARCH_THRESHOLD (1.0 / 4.0)
// The subpixel blend's strength and its cap (the cap keeps single-pixel detail such as wires from vanishing)
#define FXAA_SUBPIX_TRIM (1.0 / 4.0)
#define FXAA_SUBPIX_CAP (3.0 / 4.0)
#define FXAA_SUBPIX_TRIM_SCALE (1.0 / (1.0 - FXAA_SUBPIX_TRIM))

// FXAA's own luma: green weighted against red, which is the pair the eye reads a stair step through (Lottes' ComputeFxaaLuma)
float Luma(vec3 rgb)
{
    return rgb.y * (0.587 / 0.299) + rgb.x;
}

// One texel at an integer offset (the sampler is linear, and at a texel's centre linear sampling is that texel exactly).
// The coordinate comes in rather than the varying being read here: bgfx's shader dialect does not let a helper see one
vec3 Texel(vec2 uv, vec2 tap)
{
    return texture2DLod(s_tex, uv + tap * u_fxaaPt.xy, 0.0).rgb;
}

void main()
{
    vec3 rgbM = texture2DLod(s_tex, v_texcoord0, 0.0).rgb;
    float lumaM = Luma(rgbM);
    float lumaNorth = Luma(Texel(v_texcoord0, vec2(0.0, -1.0)));
    float lumaWest = Luma(Texel(v_texcoord0, vec2(-1.0, 0.0)));
    float lumaEast = Luma(Texel(v_texcoord0, vec2(1.0, 0.0)));
    float lumaSouth = Luma(Texel(v_texcoord0, vec2(0.0, 1.0)));

    // The edge exists only if the centre and its four neighbours span enough luma; flat areas keep every bit of detail
    float rangeMin = min(lumaM, min(min(lumaNorth, lumaWest), min(lumaSouth, lumaEast)));
    float rangeMax = max(lumaM, max(max(lumaNorth, lumaWest), max(lumaSouth, lumaEast)));
    float range = rangeMax - rangeMin;
    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD))
    {
        gl_FragColor = vec4(rgbM, 1.0);
        return;
    }

    // The subpixel term: how unlike its neighbourhood the centre is. A thin line or a dot gets a smaller blend, so it
    // survives; the middle of a stair step gets the full one
    vec3 rgbAccum = Texel(v_texcoord0, vec2(0.0, -1.0)) + Texel(v_texcoord0, vec2(-1.0, 0.0)) + rgbM + Texel(v_texcoord0, vec2(1.0, 0.0)) + Texel(v_texcoord0, vec2(0.0, 1.0));
    float lumaAvg = (lumaNorth + lumaWest + lumaEast + lumaSouth) * 0.25;
    float rangeLocal = abs(lumaAvg - lumaM);
    float blendSubpix = max(0.0, (rangeLocal / range) - FXAA_SUBPIX_TRIM) * FXAA_SUBPIX_TRIM_SCALE;
    blendSubpix = min(FXAA_SUBPIX_CAP, blendSubpix);
    rgbAccum += Texel(v_texcoord0, vec2(-1.0, -1.0)) + Texel(v_texcoord0, vec2(1.0, -1.0)) + Texel(v_texcoord0, vec2(-1.0, 1.0)) + Texel(v_texcoord0, vec2(1.0, 1.0));
    rgbAccum *= 1.0 / 9.0;

    float lumaNW = Luma(Texel(v_texcoord0, vec2(-1.0, -1.0)));
    float lumaNE = Luma(Texel(v_texcoord0, vec2(1.0, -1.0)));
    float lumaSW = Luma(Texel(v_texcoord0, vec2(-1.0, 1.0)));
    float lumaSE = Luma(Texel(v_texcoord0, vec2(1.0, 1.0)));

    // Which way the edge runs: the second derivative along each axis, summed over the three rows (columns) it crosses
    float edgeVertical = abs(0.25 * lumaNW - 0.5 * lumaNorth + 0.25 * lumaNE) +
                         abs(0.5 * lumaWest - lumaM + 0.5 * lumaEast) +
                         abs(0.25 * lumaSW - 0.5 * lumaSouth + 0.25 * lumaSE);
    float edgeHorizontal = abs(0.25 * lumaNW - 0.5 * lumaWest + 0.25 * lumaSW) +
                           abs(0.5 * lumaNorth - lumaM + 0.5 * lumaSouth) +
                           abs(0.25 * lumaNE - 0.5 * lumaEast + 0.25 * lumaSE);
    bool horizontalSpan = edgeHorizontal >= edgeVertical;

    // The walk goes up or left first, whichever side of the edge is darker: that is the side the blend has to reach
    float stepLength = horizontalSpan ? -u_fxaaPt.y : -u_fxaaPt.x;
    if (!horizontalSpan)
    {
        lumaNorth = lumaWest;
        lumaSouth = lumaEast;
    }

    float gradientNeg = abs(lumaNorth - lumaM);
    float gradientPos = abs(lumaSouth - lumaM);
    lumaNorth = (lumaNorth + lumaM) * 0.5;
    lumaSouth = (lumaSouth + lumaM) * 0.5;
    if (gradientNeg < gradientPos)
    {
        lumaNorth = lumaSouth;
        gradientNeg = gradientPos;
        stepLength = -stepLength;
    }

    // Both halves of the span are walked at once, out of phase by half a step so the first tap of each already sits off
    // the centre's pixel
    vec2 searchNegative = v_texcoord0 + (horizontalSpan ? vec2(0.0, stepLength * 0.5) : vec2(stepLength * 0.5, 0.0));
    vec2 searchPositive = searchNegative;
    vec2 searchOffset = horizontalSpan ? vec2(u_fxaaPt.x, 0.0) : vec2(0.0, u_fxaaPt.y);
    searchNegative -= searchOffset;
    searchPositive += searchOffset;
    gradientNeg *= FXAA_SEARCH_THRESHOLD;

    float lumaEndNegative = lumaNorth;
    float lumaEndPositive = lumaNorth;
    bool doneNegative = false;
    bool donePositive = false;
    for (int step = 0; step < FXAA_SEARCH_STEPS; step++)
    {
        if (!doneNegative)
        {
            lumaEndNegative = Luma(texture2DLod(s_tex, searchNegative, 0.0).rgb);
        }
        if (!donePositive)
        {
            lumaEndPositive = Luma(texture2DLod(s_tex, searchPositive, 0.0).rgb);
        }
        doneNegative = doneNegative || abs(lumaEndNegative - lumaNorth) >= gradientNeg;
        donePositive = donePositive || abs(lumaEndPositive - lumaNorth) >= gradientNeg;
        if (doneNegative && donePositive)
        {
            break;
        }
        if (!doneNegative)
        {
            searchNegative -= searchOffset;
        }
        if (!donePositive)
        {
            searchPositive += searchOffset;
        }
    }

    float distanceNegative = horizontalSpan ? v_texcoord0.x - searchNegative.x : v_texcoord0.y - searchNegative.y;
    float distancePositive = horizontalSpan ? searchPositive.x - v_texcoord0.x : searchPositive.y - v_texcoord0.y;
    bool negativeCloser = distanceNegative < distancePositive;
    lumaEndNegative = negativeCloser ? lumaEndNegative : lumaEndPositive;
    // The span's ends are on the same side of the edge's luma (the pixel is not inside a step of it): no blend, the pixel
    // is already correct
    if ((lumaM - lumaNorth < 0.0) == (lumaEndNegative - lumaNorth < 0.0))
    {
        stepLength = 0.0;
    }

    float spanLength = distancePositive + distanceNegative;
    float distanceNear = negativeCloser ? distanceNegative : distancePositive;
    float subPixelOffset = (0.5 - distanceNear / spanLength) * stepLength;
    vec3 rgbFiltered = texture2DLod(s_tex, v_texcoord0 + (horizontalSpan ? vec2(0.0, subPixelOffset) : vec2(subPixelOffset, 0.0)), 0.0).rgb;
    gl_FragColor = vec4(mix(rgbFiltered, rgbAccum, blendSubpix), 1.0);
}
