// The scene composite's vertex shader: the fullscreen triangle's positions arrive in clip space and its UVs come
// straight from them (the overhang's samples clamp to the scene target's edge).
$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
