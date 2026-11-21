// The overdraw map's fragment shader: a triangle covering a pixel adds one step of grey and nothing else, so a pixel's
// brightness is how many times the frame shaded it. The draws put it behind the geometry's own vertex shader -- so the
// triangles are where the frame puts them, skinning and all -- turn the depth test off so nothing hides anything, and set
// the blend to add (draws_host.cpp, HostSetOverdraw; `--overdraw` on any host build, `?overdraw` on the page)
$input v_texcoord0, v_color0

#if BGFX_SHADER_LANGUAGE_ESSL
// WebGL (ESSL) requires a fragment shader to state its float precision; desktop GLSL does not
precision highp float;
#endif

#include <bgfx_shader.sh>

void main()
{
    // Sixteen layers reach white: fewer layers read as a shade of grey, more saturate, so a bright patch is a place the frame
    // drew many times over. The same step on every channel keeps the map neutral against the picture's own colours
    gl_FragColor = vec4(0.0625, 0.0625, 0.0625, 0.0625);
}
