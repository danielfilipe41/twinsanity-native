// The host decals' and distortion's fragment shader: the bound texture (a decal page's, or the frame copy the
// distorting hexagons sample) times the vertex colour. The GS does not premultiply: its blend equations weight the
// source's colour by the source's alpha themselves, so the colour comes out of the shader as it is.
$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

// The decal pages' alpha test (the GS's TEST: every page mode is built with ATST GEQUAL 5 and AFAIL KEEP, the same
// bytes a material's shader carries). x is the comparison and y the reference (the GS's AREF / 128), exactly as the
// 3D shader's u_materialAlpha takes them; the distorting hexagons share this program and leave x at zero, so nothing
// is dropped for them
uniform vec4 u_decalAlpha;

void main()
{
    vec4 texel = texture2D(s_tex, v_texcoord0);
    gl_FragColor = vec4(texel.rgb * v_color0.rgb, texel.a * v_color0.a);

    // The console's comparison in 8 bit bytes: the texture's alpha byte times the vertex colour's, shifted down by
    // seven and saturated against the reference. The decoder doubles the channels, so 255 is its saturation of the
    // game's full 128 (a page with no alpha at all is full) and every other value halves back to the file's byte
    if (u_decalAlpha.x > 0.5)
    {
        float decodedByte = floor(texel.a * 255.0 + 0.5);
        float texelByte = decodedByte >= 255.0 ? 128.0 : decodedByte * 0.5;
        float vertexByte = floor(clamp(v_color0.a, 0.0, 1.0) * 128.0 + 0.5);
        float tested = min(255.0, floor(texelByte * vertexByte / 128.0));
        if (tested < u_decalAlpha.y * 128.0)
        {
            discard;
        }
    }
}
