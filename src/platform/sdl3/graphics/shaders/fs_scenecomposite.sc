// The scene composite's fragment shader: the scene target's colour over the backbuffer. The alpha is forced to 1
// (the scene target's is only meaningful as the frame's coverage -- the shadow pass reads it -- and the window is
// opaque, so passing it through would let the desktop show through wherever the scene drew nothing).
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);

void main()
{
    vec3 colour = texture2D(s_tex, v_texcoord0).rgb;
    gl_FragColor = vec4(colour, 1.0);
}
