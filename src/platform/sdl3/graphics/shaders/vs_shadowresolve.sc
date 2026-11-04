// The shadows' resolve vertex shader: positions arrive already in clip space (the host builds one fullscreen
// triangle), so there is no transform to apply; the half-res target's UVs come from the position, with V mirrored where
// the backend's rows are the other way round (u_rtVSign, host_graphics.h's HostRenderTargetVSign: the resolve reads render
// targets and writes the frame, so the flip the composite answers with its geometry has to be made up here)
$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_rtVSign;

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = vec2(a_position.x * 0.5 + 0.5, a_position.y * 0.5 * u_rtVSign.x + 0.5);
}
