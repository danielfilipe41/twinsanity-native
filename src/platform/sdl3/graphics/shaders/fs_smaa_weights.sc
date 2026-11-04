// SMAA's second pass: blending weights (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", 2012).
//
// For every pixel the edges texture marked, this finds the edge's span (how far the stair step runs along itself, by
// walking the edges texture until it stops answering, in two texel steps), then asks the area texture what share of the
// pixel each side of the span should have. Two searches per axis: the orthogonal one walks left/right (or up/down), the
// diagonal one walks the pattern's own staircase, which is what lets SMAA find the ends of a 45-degree line rather than
// stopping at the first step. The corner rounding trims the weight where two perpendicular edges meet, so corners come
// out sharp instead of smeared.
//
// The area texture is SMAA's own precomputed table: for each (edge shape, distance from the span's end, subtexel phase)
// it holds the two coverage shares, which is the whole reason SMAA beats FXAA on quality without searching per pixel on
// the CPU. Its layout is fixed at 160x560 and both it and the search texture are the ones the original distribution
// ships (see shaders/smaa_area_tex.bin.h).
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_edges, 0);
SAMPLER2D(s_area, 1);
SAMPLER2D(s_search, 2);
// xy: one texel in UV units; zw: the frame's size in texels
uniform vec4 u_aaPt;

// SMAA's "high" preset: 16 orthogonal search steps and 8 diagonal ones, with the diagonal pattern detection and the 25%
// corner rounding on. Fewer steps cut a long edge's span short (visible as unchamfered ends on shallow lines), and
// dropping the diagonal search leaves 45-degree edges stepped
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25

// The area texture's own geometry: 16 distances along the orthogonal block, 20 along the diagonal one, seven subtexel
// phases stacked per distance row
#define SMAA_AREATEX_MAX_DISTANCE 16.0
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20.0
#define SMAA_AREATEX_PIXEL_SIZE vec2(1.0 / 160.0, 1.0 / 560.0)
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
// The search texture ships padded from 64x16 to 66x33 by its generator; the sampling maps an edges pair onto it
#define SMAA_SEARCHTEX_SIZE vec2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE vec2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)

// The edges texture's two channels: a vertical edge in r, a horizontal one in g
vec2 EdgesSample(vec2 uv)
{
    return texture2DLod(s_edges, uv, 0.0).rg;
}

// The same at an integer texel offset. The sampler is linear, and a linear tap at an integral offset lands on exactly the
// texel the offset names, so this matches the reference's SampleLevel with an integer offset without needing one (bgfx's
// shader dialect has no offset argument on every backend)
vec2 EdgesSampleOffset(vec2 uv, vec2 offset)
{
    return EdgesSample(uv + offset * u_aaPt.xy);
}

vec2 AreaSample(vec2 uv)
{
    return texture2DLod(s_area, uv, 0.0).rg;
}

float SearchSample(vec2 uv)
{
    return texture2DLod(s_search, uv, 0.0).x;
}

// The edges texture's diagonal patterns are stored at bilinear-accessible phases: the reference decodes the two channels
// back to whole 0/1 steps before reading a pattern out of them
vec2 DecodeDiag(vec2 e)
{
    e.x = e.x * abs(5.0 * e.x - 5.0 * 0.75);
    return round(e);
}

vec4 DecodeDiag(vec4 e)
{
    e.xz = e.xz * abs(5.0 * e.xz - 5.0 * 0.75);
    return round(e);
}

// Walks a diagonal edge towards dir, a texel step at a time, until the edges texture stops answering. Returns how many
// steps were taken (starting at -1, as the reference counts) and the last step's edge strength in e
vec2 SearchDiag(vec2 uv, vec2 dir, bool decode, out vec2 e)
{
    float distance = -1.0;
    float running = 1.0;
    vec2 coord = uv;
    while (distance < SMAA_MAX_SEARCH_STEPS_DIAG - 1.0 && running > 0.9)
    {
        coord += dir * u_aaPt.xy;
        distance += 1.0;
        e = EdgesSample(coord);
        if (decode)
        {
            e = DecodeDiag(e);
        }
        running = dot(e, vec2(0.5, 0.5));
    }

    return vec2(distance, running);
}

// The area table's diagonal block: the subtexel phase picks one of the seven stacked rows for this edge shape
vec2 AreaDiag(vec2 dist, vec2 e, float offset)
{
    vec2 uv = SMAA_AREATEX_MAX_DISTANCE_DIAG * e + dist;
    uv = SMAA_AREATEX_PIXEL_SIZE * uv + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    // The diagonal block sits in the right half of the texture
    uv.x += 0.5;
    uv.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return AreaSample(uv);
}

// Both diagonals' weights, in the same two-pass shape as the reference: the "/" diagonal first, then the "\" one
vec2 CalculateDiagWeights(vec2 uv, vec2 e)
{
    vec2 weights = vec2(0.0);
    vec4 d = vec4(0.0);
    vec2 end;

    if (e.x > 0.0)
    {
        d.xz = SearchDiag(uv, vec2(-1.0, 1.0), false, end);
        d.x += (end.y > 0.9) ? 1.0 : 0.0;
    }
    d.yw = SearchDiag(uv, vec2(1.0, -1.0), false, end);

    if (d.x + d.y > 2.0)
    {
        vec4 coords = vec4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * u_aaPt.xyxy + uv.xyxy;
        vec4 c;
        c.xy = EdgesSampleOffset(coords.xy, vec2(-1.0, 0.0));
        c.zw = EdgesSampleOffset(coords.zw, vec2(1.0, 0.0));
        c = DecodeDiag(c);
        c = c.yxwz;

        vec2 cc = 2.0 * c.xz + c.yw;
        // A diagonal that ran out of texture instead of out of edge has no shape worth an area lookup
        cc = vec2(d.z > 0.9 ? 0.0 : cc.x, d.w > 0.9 ? 0.0 : cc.y);
        weights += AreaDiag(d.xy, cc, 0.0);
    }

    d.xz = SearchDiag(uv, vec2(-1.0, -1.0), true, end);
    if (EdgesSampleOffset(uv, vec2(1.0, 0.0)).x > 0.0)
    {
        d.yw = SearchDiag(uv, vec2(1.0, 1.0), true, end);
        d.y += (end.y > 0.9) ? 1.0 : 0.0;
    }
    else
    {
        d.yw = vec2(0.0);
    }

    if (d.x + d.y > 2.0)
    {
        vec4 coords = vec4(-d.x, -d.x, d.y, d.y) * u_aaPt.xyxy + uv.xyxy;
        vec2 near = vec2(EdgesSampleOffset(coords.xy, vec2(-1.0, 0.0)).y, EdgesSampleOffset(coords.xy, vec2(0.0, -1.0)).x);
        vec2 far = EdgesSampleOffset(coords.zw, vec2(1.0, 0.0)).yx;
        vec2 cc = 2.0 * vec2(near.x, far.x) + vec2(near.y, far.y);
        cc = vec2(d.z > 0.9 ? 0.0 : cc.x, d.w > 0.9 ? 0.0 : cc.y);
        weights += AreaDiag(d.xy, cc, 0.0).yx;
    }

    return weights;
}

// How far along the pattern the edges pair reaches, read out of the search texture: the pair is an index into the packed
// 64x16 table, offset by which half of the pattern it covers
float SearchLength(vec2 e, float offset)
{
    vec2 scale = SMAA_SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias = SMAA_SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0, 1.0);
    bias += vec2(0.5, -0.5);
    scale *= vec2(1.0) / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= vec2(1.0) / SMAA_SEARCHTEX_PACKED_SIZE;
    return SearchSample(scale * e + bias);
}

// The span's left end: walk left in two-texel steps while the pattern keeps up its alternating r/g (the 0.8281 is the
// reference's threshold on the pattern's own strength), then let the search texture say how much further it runs
float SearchXLeft(vec2 uv, float end, out vec2 e)
{
    e = vec2(0.0, 1.0);
    while (uv.x > end && e.y > 0.8281 && e.x == 0.0)
    {
        e = EdgesSample(uv);
        uv -= vec2(2.0 * u_aaPt.x, 0.0);
    }

    float offset = -(255.0 / 127.0) * SearchLength(e, 0.0) + 3.25;
    return u_aaPt.x * offset + uv.x;
}

float SearchXRight(vec2 uv, float end, out vec2 e)
{
    e = vec2(0.0, 1.0);
    while (uv.x < end && e.y > 0.8281 && e.x == 0.0)
    {
        e = EdgesSample(uv);
        uv += vec2(2.0 * u_aaPt.x, 0.0);
    }

    float offset = -(255.0 / 127.0) * SearchLength(e, 0.5) + 3.25;
    return -u_aaPt.x * offset + uv.x;
}

float SearchYUp(vec2 uv, float end, out vec2 e)
{
    e = vec2(1.0, 0.0);
    while (uv.y > end && e.x > 0.8281 && e.y == 0.0)
    {
        e = EdgesSample(uv);
        uv -= vec2(0.0, 2.0 * u_aaPt.y);
    }

    float offset = -(255.0 / 127.0) * SearchLength(e.yx, 0.0) + 3.25;
    return u_aaPt.y * offset + uv.y;
}

float SearchYDown(vec2 uv, float end, out vec2 e)
{
    e = vec2(1.0, 0.0);
    while (uv.y < end && e.x > 0.8281 && e.y == 0.0)
    {
        e = EdgesSample(uv);
        uv += vec2(0.0, 2.0 * u_aaPt.y);
    }

    float offset = -(255.0 / 127.0) * SearchLength(e.yx, 0.5) + 3.25;
    return -u_aaPt.y * offset + uv.y;
}

// The area table's orthogonal block: the edge pair names the pattern, the distance the span's halves are
vec2 Area(vec2 dist, float e1, float e2, float offset)
{
    vec2 uv = SMAA_AREATEX_MAX_DISTANCE * round(4.0 * vec2(e1, e2)) + dist;
    uv = SMAA_AREATEX_PIXEL_SIZE * uv + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    uv.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return AreaSample(uv);
}

// The corner rounding: where two perpendicular edges meet, the reference trims the weight by the pattern found just past
// the span's end, so the corner reads as a corner instead of being chamfered away
void DetectHorizontalCornerPattern(inout vec2 weights, vec4 uv, vec2 d)
{
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * EdgesSampleOffset(uv.xy, vec2(0.0, 1.0)).x;
    factor.x -= rounding.y * EdgesSampleOffset(uv.zw, vec2(1.0, 1.0)).x;
    factor.y -= rounding.x * EdgesSampleOffset(uv.xy, vec2(0.0, -2.0)).x;
    factor.y -= rounding.y * EdgesSampleOffset(uv.zw, vec2(1.0, -2.0)).x;

    weights *= saturate(factor);
}

void DetectVerticalCornerPattern(inout vec2 weights, vec4 uv, vec2 d)
{
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * EdgesSampleOffset(uv.xy, vec2(1.0, 0.0)).y;
    factor.x -= rounding.y * EdgesSampleOffset(uv.zw, vec2(1.0, 1.0)).y;
    factor.y -= rounding.x * EdgesSampleOffset(uv.xy, vec2(-2.0, 0.0)).y;
    factor.y -= rounding.y * EdgesSampleOffset(uv.zw, vec2(-2.0, 1.0)).y;

    weights *= saturate(factor);
}

void main()
{
    vec2 texel = u_aaPt.xy;
    vec2 pixcoord = v_texcoord0 * u_aaPt.zw;

    // The search's starting points: a quarter texel in from each side, and the far ends a full search's reach away
    vec4 offset0 = vec4(-0.25, -0.125, 1.25, -0.125) * u_aaPt.xyxy + v_texcoord0.xyxy;
    vec4 offset1 = vec4(-0.125, -0.25, -0.125, 1.25) * u_aaPt.xyxy + v_texcoord0.xyxy;
    vec4 offset2 = vec4(-2.0, 2.0, -2.0, 2.0) * SMAA_MAX_SEARCH_STEPS;
    offset2 = vec4(offset2.x * texel.x, offset2.y * texel.x, offset2.z * texel.y, offset2.w * texel.y) +
              vec4(offset0.xz, offset1.yw);

    vec4 weights = vec4(0.0);
    vec2 e = EdgesSample(v_texcoord0);

    // A horizontal edge (g) runs along x: its span is walked left and right
    if (e.y > 0.0)
    {
        weights.rg = CalculateDiagWeights(v_texcoord0, e);
        // The diagonal patterns accounted for it, or there is nothing diagonal here: fall back to the orthogonal span
        if (weights.x == -weights.y)
        {
            vec2 d;
            vec3 coords;
            vec2 end;

            coords.x = SearchXLeft(offset0.xy, offset2.x, end);
            coords.y = offset1.y;
            d.x = coords.x;
            float e1 = EdgesSample(coords.xy).x;

            coords.z = SearchXRight(offset0.zw, offset2.y, end);
            d.y = coords.z;

            d = abs(round(vec2(u_aaPt.z, u_aaPt.z) * d - pixcoord.x));
            vec2 sqrtD = sqrt(d);

            float e2 = EdgesSampleOffset(coords.zy, vec2(1.0, 0.0)).x;
            weights.rg = Area(sqrtD, e1, e2, 0.0);

            coords.y = v_texcoord0.y;
            DetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);
        }
    }

    // A vertical edge (r) runs along y: its span is walked up and down
    if (e.x > 0.0)
    {
        vec2 d;
        vec3 coords;
        vec2 end;

        coords.y = SearchYUp(offset1.xy, offset2.z, end);
        coords.x = offset0.x;
        d.x = coords.y;
        float e1 = EdgesSample(coords.xy).y;

        coords.z = SearchYDown(offset1.zw, offset2.w, end);
        d.y = coords.z;

        d = abs(round(vec2(u_aaPt.w, u_aaPt.w) * d - pixcoord.y));
        vec2 sqrtD = sqrt(d);

        float e2 = EdgesSampleOffset(coords.xz, vec2(0.0, 1.0)).y;
        weights.ba = Area(sqrtD, e1, e2, 0.0);

        coords.x = v_texcoord0.x;
        DetectVerticalCornerPattern(weights.ba, coords.xyxz, d);
    }

    gl_FragColor = weights;
}
