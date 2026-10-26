// The 3D skinned-mesh vertex shader: the rigid path plus up to three joints
// per vertex (a_indices raw 0-255 lanes, a_weight the float weights the skin
// packet's address-5 row carries above each index; renormalized by the sum).
// Joint matrices ride the u_joints array (WriteJoints' dedup key kept
// host-side). A blend skin's shapes are already in the positions the host
// uploads: the draw's own weights scale them into the vertexes there
// (BakeBlendShapes, draws_host.cpp). u_blendWeights still uploads beside this
// program as the draw's weight array (HostDraw3DBlendWeights, u_joints'
// counterpart); the vertex path below moves nothing with it.
$input a_position, a_texcoord0, a_color0, a_normal, a_indices, a_weight
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform mat4 u_mvp;
uniform vec4 u_lightDirs[3];
uniform vec4 u_lightCols[3];
uniform vec4 u_ambient;
// The material's texture scroll (renderer.h's Shader::scroll): the offset every UV takes
uniform vec4 u_scroll;
// The retail skins' joint indices reach 61 over the whole disc (mesh_builder.h
// SkinMaxJoints), so the array matches it
uniform mat4 u_joints[128];
uniform vec4 u_blendWeights[4];

vec3 SkinPosition(vec3 position)
{
    // The packet's weights as they come: retail's skinning program multiplies the joints' positions by them directly
    // (VU1 Parameter, ITOF0 of the packed weights then MULx/MULy/MULz, no divide by their sum), so renormalising here
    // was the host's own addition. Only a vertex with no weight at all needs the fallback, and retail's data has none
    vec3 weights = a_weight.xyz;
    float total = weights.x + weights.y + weights.z;
    weights = total > 0.0 ? weights : vec3(1.0, 0.0, 0.0);
    // Indices are stored as floats 0-255 directly
    int first = int(a_indices.x + 0.5);
    int second = int(a_indices.y + 0.5);
    int third = int(a_indices.z + 0.5);
    vec3 skinned = mul(u_joints[first], vec4(position, 1.0)).xyz * weights.x;
    skinned += mul(u_joints[second], vec4(position, 1.0)).xyz * weights.y;
    skinned += mul(u_joints[third], vec4(position, 1.0)).xyz * weights.z;
    return skinned;
}

void main()
{
    vec3 world = SkinPosition(a_position);
    gl_Position = mul(u_mvp, vec4(world, 1.0));
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
