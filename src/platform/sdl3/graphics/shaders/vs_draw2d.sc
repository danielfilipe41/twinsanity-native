// The 2D quad vertex shader: positions arrive already in clip space (fractions of the screen mapped to
// [-1, 1] on the CPU, Y flipped), so there is no transform to apply; UVs and the premultiplied colour pass through.
$input a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
}
