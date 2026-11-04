// SMAA's third pass: neighbourhood blending (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", 2012).
//
// The weights texture holds, per pixel, the share of the two neighbours the pixel should be mixed with: r and b for the
// horizontal case, g and a for the vertical one. This pass reads the pixel's own weights and the two neighbours that
// carry the opposite pair, decides which axis has the stronger share (a pixel on a vertical edge takes the horizontal
// weights, and the other way round), and mixes the frame at those two neighbours. A pixel the weights pass left alone
// sums to zero and is copied through untouched, which is most of the frame.
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
SAMPLER2D(s_blend, 1);
// xy: one texel in UV units; zw: the frame's size in texels (unused here, shared by SMAA's three passes)
uniform vec4 u_aaPt;

void main()
{
    vec4 neighbours = vec4(1.0, 0.0, 0.0, 1.0) * u_aaPt.xyxy + v_texcoord0.xyxy;

    // The four weights that describe this pixel: its own pair on one axis and the neighbours' on the other
    vec4 a;
    a.x = texture2DLod(s_blend, neighbours.xy, 0.0).a;
    a.y = texture2DLod(s_blend, neighbours.zw, 0.0).g;
    vec2 own = texture2DLod(s_blend, v_texcoord0, 0.0).xz;
    a.z = own.y;
    a.w = own.x;

    if (dot(a, vec4(1.0, 1.0, 1.0, 1.0)) < 1e-5)
    {
        // No edge here (or the edge's weight rounded away): the frame as it is
        gl_FragColor = vec4(texture2DLod(s_tex, v_texcoord0, 0.0).rgb, 1.0);
        return;
    }

    // Which axis wins: the horizontal pair against the vertical one
    bool horizontal = max(a.x, a.z) > max(a.y, a.w);
    vec4 blendingOffset = horizontal ? vec4(a.x, 0.0, a.z, 0.0) : vec4(0.0, a.y, 0.0, a.w);
    vec2 blendingWeight = horizontal ? a.xz : a.yw;
    // The two shares are normalised so the mix adds up to the pixel's own colour
    blendingWeight /= dot(blendingWeight, vec2(1.0, 1.0));

    // The two neighbours the mix reads: one texel back and forth along the winning axis
    vec4 blendingCoord = blendingOffset * vec4(u_aaPt.x, u_aaPt.y, -u_aaPt.x, -u_aaPt.y) + v_texcoord0.xyxy;

    vec3 colour = blendingWeight.x * texture2DLod(s_tex, blendingCoord.xy, 0.0).rgb;
    colour += blendingWeight.y * texture2DLod(s_tex, blendingCoord.zw, 0.0).rgb;
    gl_FragColor = vec4(colour, 1.0);
}
