// The host decals' and distortion's vertex shader: the CPU builds the quads' coordinates in the game's own homogeneous
// screen space (the GS's 1/16th of a pixel, middle 2048, Y down, every component already multiplied by the clip w the
// projection gives the corner), and each vertex carries that w in its STQ's q, which the GS divides the texture
// coordinates by as well. The position must therefore go out with that same w -- dividing x and y by 1 here instead
// (as this did) left every decal w times its size and centred 32768 * (w - 1) / 4096 units off, which put a ripple
// a few units from the camera tens of screens away and made the ones at w = 1 look like they sat on the lens.
//
// The distortion hexagons come through the same program with q = 1 (their quads are built already in clip space), so
// they are unaffected.
$input a_position, a_texcoord0, a_color0
$output v_color0, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position.xy, a_position.z, a_texcoord0.z);
    v_texcoord0 = a_texcoord0.xy / a_texcoord0.z;
    v_color0 = a_color0;
}
