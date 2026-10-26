// The 2D quad fragment shader: sample times the premultiplied vertex colour, with
// the material's alpha test. Untextured strips bind the 1x1 white texture, so the
// same program covers both paths with no branches.
$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

// The material's alpha test (the GS's TEST register): x the comparison (the GS's
// ATST, in glAlphaFunc numbers: 0 never .. 7 not equal), y the reference (the GS's
// AREF / 128: the game's alpha is full at 128). SubmitQuad/SubmitStrip set it from
// the material; the UI's flat material has no test and gets ALWAYS, so nothing is
// dropped
uniform vec4 u_materialAlpha;

void main()
{
    vec4 colour = texture2D(s_tex, v_texcoord0) * v_color0;

    // bgfx has no fixed-function alpha test, so the GS's ATE/ATST/AREF becomes this
    // comparison and a sample that fails is dropped. The equality tests need a
    // texel's slack (an 8 bit alpha in [0, 1])
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
    if (!passes)
    {
        discard;
    }

    gl_FragColor = colour;
}
