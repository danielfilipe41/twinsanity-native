// The 3D rigid-mesh vertex shader: the world-space position through the
// model-to-clip matrix (column-vector, mul(u_mvp, v): the host uploads the
// transposed game matrix), with the three directional lights plus ambient
// (the VU1 lighting the bgfx shader reproduces, approximately: the retail
// normal packing decodes through a signed remap, lighting is NdotL).
// UVs and the vertex colour pass through.
$input a_position, a_texcoord0, a_color0, a_normal
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform mat4 u_mvp;
uniform vec4 u_lightDirs[3];
uniform vec4 u_lightCols[3];
uniform vec4 u_ambient;
// The material's texture scroll (renderer.h's Shader::scroll): the offset every UV takes
uniform vec4 u_scroll;

void main()
{
    gl_Position = mul(u_mvp, vec4(a_position, 1.0));
    vec3 normal = normalize(a_normal.xyz * 2.0 - 1.0);
    vec3 light = u_ambient.rgb;
    light += u_lightCols[0].rgb * max(dot(normal, u_lightDirs[0].xyz), 0.0);
    light += u_lightCols[1].rgb * max(dot(normal, u_lightDirs[1].xyz), 0.0);
    light += u_lightCols[2].rgb * max(dot(normal, u_lightDirs[2].xyz), 0.0);
    v_texcoord0 = a_texcoord0 + u_scroll.xy;
    // The lit vertex colour, saturated: the GS takes the vertex colour's bytes times the light
    // and keeps what fits, and TT-Lab's own shader clamps the same product; without it a
    // full light over a full ambient runs past white and only the target clamps
    v_color0 = vec4(min(a_color0.rgb * light, vec3(1.0)), a_color0.a);
}
