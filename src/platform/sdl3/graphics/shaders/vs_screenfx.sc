// The screen-filter vertex shader: positions arrive already in clip space (the host builds one fullscreen
// triangle), so there is no transform to apply; the depth texture's UVs come straight from the position.
$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_position.xy * 0.5 + 0.5;
}
