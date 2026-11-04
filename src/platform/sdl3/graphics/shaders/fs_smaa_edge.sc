// SMAA's first pass: luma edge detection (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", 2012).
//
// A pixel is an edge if its luma differs from its left or top neighbour by more than the threshold, and that difference
// is large compared with the luma steps around it (the local contrast adaptation): the second test is what keeps a
// textured surface from being read as a hundred tiny edges. The result is the edges texture the weights pass searches:
// r marks a vertical edge, g a horizontal one, exactly the pair SMAA's own shaders write.
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
// xy: one texel in UV units; zw: the frame's size in texels (unused here, shared by SMAA's three passes)
uniform vec4 u_aaPt;

// The luma difference an edge has to beat, in the frame's own [0, 1] colour range. SMAA's recommended 0.1 (its "high"
// preset): lower finds more edges and starts anti-aliasing gradients, higher misses soft ones
#define SMAA_THRESHOLD 0.1
// How much bigger an edge has to be than its neighbourhood before it counts. 2.0 is SMAA's default
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0

float Luma(vec3 colour)
{
    return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 texel = u_aaPt.xy;
    float lumaCentre = Luma(texture2DLod(s_tex, v_texcoord0, 0.0).rgb);
    float lumaLeft = Luma(texture2DLod(s_tex, v_texcoord0 - vec2(texel.x, 0.0), 0.0).rgb);
    float lumaTop = Luma(texture2DLod(s_tex, v_texcoord0 - vec2(0.0, texel.y), 0.0).rgb);

    // The two neighbours the edges are tested against, and the step each of them has on its far side
    vec2 delta = vec2(abs(lumaCentre - lumaLeft), abs(lumaCentre - lumaTop));
    vec2 edges = step(vec2(SMAA_THRESHOLD), delta);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0)
    {
        // Nothing on either axis: the weights pass reads this as "no edge here" and leaves the pixel alone
        gl_FragColor = vec4(0.0);
        return;
    }

    float lumaRight = Luma(texture2DLod(s_tex, v_texcoord0 + vec2(texel.x, 0.0), 0.0).rgb);
    float lumaBottom = Luma(texture2DLod(s_tex, v_texcoord0 + vec2(0.0, texel.y), 0.0).rgb);
    float lumaLeftLeft = Luma(texture2DLod(s_tex, v_texcoord0 - vec2(texel.x + texel.x, 0.0), 0.0).rgb);
    float lumaTopTop = Luma(texture2DLod(s_tex, v_texcoord0 - vec2(0.0, texel.y + texel.y), 0.0).rgb);

    vec2 maxDelta = max(delta, vec2(abs(lumaCentre - lumaRight), abs(lumaCentre - lumaBottom)));
    maxDelta = max(maxDelta, vec2(abs(lumaLeft - lumaLeftLeft), abs(lumaTop - lumaTopTop)));
    float finalDelta = max(maxDelta.x, maxDelta.y);
    // The edge survives only where it is the picture's own step, not one texel of a busy gradient
    edges *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta);
    gl_FragColor = vec4(edges, 0.0, 1.0);
}
