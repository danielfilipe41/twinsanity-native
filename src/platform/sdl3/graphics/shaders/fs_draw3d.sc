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
    vec4 texel = texture2D(s_tex, v_texcoord0);
    vec4 colour = texel * v_color0;

    // bgfx has no fixed-function alpha test, so a material with its ATE set has its ATST/AREF compared here and the
    // samples that fail are dropped.
    float method = u_materialAlpha.x;
    // The reference renderer's own comparison (twinssanity-elfree's GL_ALPHA_TEST): the fragment's alpha is the
    // texture's alpha times the vertex colour's alpha, both as the window holds them (0..1), and the reference is the
    // material's AREF over the game's full 128. That is NOT the GS's byte arithmetic: it compares (t/255)*(v/255) with
    // AREF/128, which keeps a texel only once t*v is past AREF*508 -- roughly four times the threshold the GS's own
    // (t*v)>>7 test uses. The reference port renders the levels with this, and it is what the platforms, the grass
    // cards and the tints match, so the host tests the same way
    float texelAlpha = texel.a;
    // The vertex alpha is uploaded scaled to the window's 0..1 (the file's 128 is full), so the fraction the
    // reference multiplies is that value over 255/128
    float vertexAlpha = clamp(v_color0.a, 0.0, 1.0) * (128.0 / 255.0);
    float tested = texelAlpha * vertexAlpha;
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
        passes = tested < reference;
    }
    else if (method < 3.5)     // less or equal
    {
        passes = tested <= reference;
    }
    else if (method < 4.5)     // equal
    {
        passes = abs(tested - reference) < (0.5 / 128.0);
    }
    else if (method < 5.5)     // greater or equal
    {
        passes = tested >= reference;
    }
    else if (method < 6.5)     // greater
    {
        passes = tested > reference;
    }
    else                       // not equal
    {
        passes = abs(tested - reference) >= (0.5 / 128.0);
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

    // A "brighten" material (the GS's alpha preset 4): its blend is Cd·As + Cd, the background multiplied by 1 + the
    // sample's alpha, which no bgfx blend factor can make from the shader's colour. The state blends with DST_COLOR,
    // ONE, so the colour written must BE the alpha: Cd·As + Cd then comes out exactly
    if (u_materialAlpha.w > 0.5)
    {
        float blended = min(1.0, tested);
        gl_FragColor = vec4(blended, blended, blended, blended);
        return;
    }

    gl_FragColor = colour;
}
