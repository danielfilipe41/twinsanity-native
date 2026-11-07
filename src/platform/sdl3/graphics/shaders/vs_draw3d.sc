// The 3D rigid-mesh vertex shader: the world-space position through the
// model-to-clip matrix (column-vector, mul(mvp, v): the host uploads the
// transposed game matrix), with the three directional lights plus ambient
// (the VU1 lighting the bgfx shader reproduces, approximately: the retail
// normal packing decodes through a signed remap, lighting is NdotL).
// UVs and the vertex colour pass through.
//
// The matrix arrives per instance (i_data0-3): the host draws a packet's repeated copies -- a prop the level places many times --
// as one instanced submit, so what differs between them is the instance data and the packet's vertexes are uploaded once. The light
// set and the material's own uniforms stay the submit's, since a batch only holds draws that share them.
$input a_position, a_texcoord0, a_color0, a_normal, i_data0, i_data1, i_data2, i_data3, i_data4, i_data5, i_data6
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

// The scene's lights packed into one uniform: the three directions, then the three colours, then the ambient. A submit sets the
// light set on every draw, and the driver copies each uniform separately, so one call for the set instead of three is what keeps it
// from being part of the per-draw cost
uniform vec4 u_lights[7];

// The material's texture scroll (renderer.h's Shader::scroll): the offset every UV takes
// The material's uniforms packed: [0] the texture scroll (renderer.h's Shader::scroll), [1] the material's own colour, whose fourth
// lane is the "written" flag -- a submit that leaves the uniform alone gives zero, which reads as no tint rather than black
uniform vec4 u_material[2];


void main()
{
    // The four vec4s are the model-to-clip matrix's columns: exactly the bytes a mat4 uniform took, now one per instance
    mat4 mvp = mat4(i_data0, i_data1, i_data2, i_data3);
    gl_Position = mul(mvp, vec4(a_position, 1.0));
    // i_data4-6 are the model's rotation: the scene's light directions turned into the model's own space, which is what the host's
    // LightUniforms did per draw (a model lights with the scene's lights rotated by its matrix). Doing it here keeps the light set a
    // submit's uniform, so a packet's draws share one submission
    mat3 rot = mat3(i_data4.xyz, i_data5.xyz, i_data6.xyz);
    vec3 normal = normalize(a_normal.xyz * 2.0 - 1.0);
    vec3 light = u_lights[6].rgb;
    light += u_lights[3].rgb * max(dot(normal, mul(rot, u_lights[0].xyz)), 0.0);
    light += u_lights[4].rgb * max(dot(normal, mul(rot, u_lights[1].xyz)), 0.0);
    light += u_lights[5].rgb * max(dot(normal, mul(rot, u_lights[2].xyz)), 0.0);
    v_texcoord0 = a_texcoord0 + u_material[0].xy;
    // The lit vertex colour, saturated: the GS takes the vertex colour's bytes times the light
    // and keeps what fits, and TT-Lab's own shader clamps the same product; without it a
    // full light over a full ambient runs past white and only the target clamps
    // The material's colour scales the lit colour for the one type whose program does that (0x1F) and not at all for
    // every other material, whose draws leave the uniform's flag lane zero
    vec3 lit = min(a_color0.rgb * light, vec3(1.0));
    if (u_material[1].w != 0.0)
    {
        lit = min(lit * u_material[1].rgb, vec3(1.0));
    }
    v_color0 = vec4(lit, a_color0.a);
}
