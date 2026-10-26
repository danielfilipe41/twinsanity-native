// The host decals' and distortion's fragment shader: the bound texture (a decal page's, or the frame copy the
// distorting hexagons sample) times the vertex colour. The GS does not premultiply: its blend equations weight the
// source's colour by the source's alpha themselves, so the colour comes out of the shader as it is.
$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

void main()
{
    vec4 texel = texture2D(s_tex, v_texcoord0);
    gl_FragColor = vec4(texel.rgb * v_color0.rgb, texel.a * v_color0.a);
}
