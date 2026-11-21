// The 3D meshes' fragment shader for a material whose alpha test never rejects anything: the sample times the vertex
// colour, and nothing else.
//
// fs_draw3d.sc is the general one, and for a draw whose ATE is clear its chain of comparisons passes every time and its
// discard never runs -- but the shader still contains a discard, and a fragment shader that may discard is one the hardware
// cannot depth-test before it samples (early-Z). Almost every opaque draw of a level is this case (draws_host.cpp's
// g_noAlphaTest: always, no AFAIL, no brighten preset), so they take this program instead and keep early-Z, which is the
// fill-rate cost a phone feels first. What it writes is what the general shader writes for them: texture * vertex colour
$input v_texcoord0, v_color0

#if BGFX_SHADER_LANGUAGE_ESSL
// WebGL (ESSL) requires a fragment shader to state its float precision; desktop GLSL does not
precision highp float;
#endif

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

void main()
{
    gl_FragColor = texture2D(s_tex, v_texcoord0) * v_color0;
}
