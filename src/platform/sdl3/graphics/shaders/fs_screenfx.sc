// The screen-filter fragment shader: the PS2's depth-as-palette trick. CopyDepthBuffer keeps the depth
// buffer's top two bits per pixel; FilterColours reads those (PSMT8H: the copied pixels' top bytes) as palette
// indexes over the frame. Here the R32F depth texture holds normalized depth directly, so depth * 255 picks one
// of the 256 palette entries the same way (the index's top byte selected the palette's entries on the GS).
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_depth, 0);
SAMPLER2D(s_palette, 1);

void main()
{
    float depth = texture2D(s_depth, v_texcoord0).x;
    float slot = clamp(depth * 255.0, 0.0, 255.0);
    vec2 uv = vec2((floor(slot + 0.5) + 0.5) / 256.0, 0.5);
    gl_FragColor = texture2D(s_palette, uv);
}
