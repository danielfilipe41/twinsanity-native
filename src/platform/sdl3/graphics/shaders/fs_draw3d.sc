// The 3D meshes' fragment shader: sample times the lit vertex colour, with the
// material's alpha test. Untextured materials bind the 1x1 white texture, so the
// same program covers both paths with no branches.
$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

// The material's alpha test (the GS's TEST register): x the comparison (0 never,
// 1 always, 2 less, 3 less or equal, 4 equal, 5 greater or equal, 6 greater,
// 7 not equal; GL's glAlphaFunc numbers), y the reference the sample's alpha is
// tested against (the GS's AREF / 128: the game's alpha is full at 128). The draws
// set it from each submodel's material; a material without an alpha test gets
// ALWAYS, so nothing is dropped
uniform vec4 u_materialAlpha;

void main()
{
    vec4 colour = texture2D(s_tex, v_texcoord0) * v_color0;

    // bgfx has no fixed-function alpha test (and saves no state for one), so a
    // material with the GS's ATE set has its ATST/AREF compared here and the
    // samples that fail are dropped. The equality tests need a texel's slack
    // (an 8 bit alpha in [0, 1])
    float method = u_materialAlpha.x;
    float reference = u_materialAlpha.y;
    bool passes = true;
    if (method < 0.5)          // never
    {
        passes = false;
    }
    else if (method < 1.5)     // always
    {
        passes = true;
    }
    else if (method < 2.5)     // less
    {
        passes = colour.a < reference;
    }
    else if (method < 3.5)     // less or equal
    {
        passes = colour.a <= reference;
    }
    else if (method < 4.5)     // equal
    {
        passes = abs(colour.a - reference) < 1.0 / 255.0;
    }
    else if (method < 5.5)     // greater or equal
    {
        passes = colour.a >= reference;
    }
    else if (method < 6.5)     // greater
    {
        passes = colour.a > reference;
    }
    else                       // not equal
    {
        passes = abs(colour.a - reference) >= 1.0 / 255.0;
    }
    // The GS's alpha-test failure action (TEST's AFAIL): a pass that draws the pixels the test REJECTED sets z, so
    // FB_ONLY's colour write can be issued as a second draw with the test turned round and the depth write off
    if (u_materialAlpha.z > 0.5)
    {
        passes = !passes;
    }
    if (!passes)
    {
        discard;
    }

    gl_FragColor = colour;
}
