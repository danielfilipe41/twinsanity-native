// The host particles' fragment shader: the page's texture times the particle's colour (the table's step colour, the
// GS's modulate: the colour's channels over 255 and the alpha over 128). The particle page's blend mode picks the
// state the draw submits with.
// x: the alpha test (the GS's ATST as the draws' numbering, or ALWAYS when the draw has none),
// y: its reference (retail's SetPageAlphaTest: GEQUAL against 5/128)
$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
uniform vec4 u_particleAlpha;

void main()
{
    vec4 texel = texture2D(s_tex, v_texcoord0);

    // The GS's modulate: the texture's colour times the primitive's colour, and the texture's alpha times the
    // primitive's alpha. The GS does NOT premultiply: its blend equations weight the source's colour by the
    // source's alpha themselves (the additive preset is (Cs - 0) * As + Cd), so the colour has to come out of the
    // shader as it is. Premultiplying it here applies the alpha twice: the puff's soft edges (a low alpha in the
    // texture) are crushed to nothing and what is left is a hard little disc instead of dust.
    vec4 colour = vec4(texel.rgb * v_color0.rgb, texel.a * v_color0.a);

    // The GS's alpha test: the cutout mode asks for GEQUAL against 5 (retail's SetPageAlphaTest), the others ALWAYS.
    // bgfx has no fixed-function alpha test, so it runs here, the same mapping fs_draw3d.sc uses for materials
    float method = u_particleAlpha.x;
    float reference = u_particleAlpha.y;
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
