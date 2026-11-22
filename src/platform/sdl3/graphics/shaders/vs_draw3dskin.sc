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
// The scene's lights packed into one uniform: the three directions, then the three colours, then the ambient. A submit sets the
// light set on every draw, and the driver copies each uniform separately, so one call for the set instead of three is what keeps it
// from being part of the per-draw cost
uniform vec4 u_lights[7];

// The material's texture scroll (renderer.h's Shader::scroll): the offset every UV takes
// The material's uniforms packed: [0] the texture scroll (renderer.h's Shader::scroll), [1] the material's own colour, whose fourth
// lane is the "written" flag -- a submit that leaves the uniform alone gives zero, which reads as no tint rather than black
uniform vec4 u_material[2];

// The joints as three vectors each, not a mat4 each (mesh_builder.h SkinMaxJoints names the count and why): a joint's
// matrix is an affine transform, its three axes in the first three rows and its position in the fourth, and the fourth
// lane of each of those axes is the constant the fourth column would hold. Sending three vectors a joint and putting
// the position in the lane that constant would waste is what keeps the array inside the 256 vertex uniform vectors a
// GL ES 3.0 device is only guaranteed to have -- four vectors a joint, over 128 of them, was 512 and no phone took it
// (0: ERROR: too many uniforms)
uniform vec4 u_joints[228];
uniform vec4 u_blendWeights[4];

// A vertex through one joint's matrix, straight from its three packed rows: the retail matrices are row-major with the
// position in their fourth row (math.h PositionRow) and a vertex is multiplied by their transpose, so the packed rows are
// this matrix's three columns and the position its fourth. The three rows' own fourth lane is what a transposed affine
// matrix leaves at zero
//
// This is a vertex times that matrix's three axes plus its position, the same arithmetic a mat4 built from the rows and
// multiplied would do, with the three rows weighed directly instead of a matrix being assembled first: assembling one per
// joint per vertex meant building up to three mat4s and taking the fourth row's dot product the skinning never uses
vec3 JointPosition(int index, vec3 position)
{
    int at = index * 3;
    vec4 x = u_joints[at];
    vec4 y = u_joints[at + 1];
    vec4 z = u_joints[at + 2];
    return x.xyz * position.x + y.xyz * position.y + z.xyz * position.z + vec3(x.w, y.w, z.w);
}

// The attributes are parameters rather than globals: shaderc's SPIR-V path compiles HLSL, where the $input attributes
// are main's parameters and a helper function cannot see them (the GLSL path declares them as globals, so the same
// source works for both)
vec3 SkinPosition(vec3 position, vec3 indices, vec3 inputWeights)
{
    // The packet's weights as they come: retail's skinning program multiplies the joints' positions by them directly
    // (VU1 Parameter, ITOF0 of the packed weights then MULx/MULy/MULz, no divide by their sum), so renormalising here
    // was the host's own addition. Only a vertex with no weight at all needs the fallback, and retail's data has none
    vec3 weights = inputWeights;
    float total = weights.x + weights.y + weights.z;
    weights = total > 0.0 ? weights : vec3(1.0, 0.0, 0.0);
    // Indices are stored as floats 0-255 directly. Clamped: the index's own range is wider than the array, and a joint
    // past it (no skin on the disc reaches the end of it, and the host says so when one does) must read a matrix rather
    // than whatever uniform follows
    int last = 228 / 3 - 1;
    int first = min(int(indices.x + 0.5), last);
    int second = min(int(indices.y + 0.5), last);
    int third = min(int(indices.z + 0.5), last);
    vec3 skinned = JointPosition(first, position) * weights.x;
    skinned += JointPosition(second, position) * weights.y;
    skinned += JointPosition(third, position) * weights.z;
    return skinned;
}

void main()
{
    vec3 world = SkinPosition(a_position, a_indices, a_weight.xyz);
    gl_Position = mul(u_mvp, vec4(world, 1.0));
    vec3 normal = normalize(a_normal.xyz * 2.0 - 1.0);
    vec3 light = u_lights[6].rgb;
    light += u_lights[3].rgb * max(dot(normal, u_lights[0].xyz), 0.0);
    light += u_lights[4].rgb * max(dot(normal, u_lights[1].xyz), 0.0);
    light += u_lights[5].rgb * max(dot(normal, u_lights[2].xyz), 0.0);
    v_texcoord0 = a_texcoord0 + u_material[0].xy;
    // The lit vertex colour, saturated: the GS takes the vertex colour's bytes times the light
    // and keeps what fits, and TT-Lab's own shader clamps the same product; without it a
    // full light over a full ambient runs past white and only the target clamps
    // The material's colour scales the lit colour for the one type whose program does that (0x1F) and not at all for
    // every other material, whose draws leave the uniform's flag lane zero
    vec3 lit = min(a_color0.rgb * light, vec3(1.0, 1.0, 1.0));
    if (u_material[1].w != 0.0)
    {
        lit = min(lit * u_material[1].rgb, vec3(1.0, 1.0, 1.0));
    }
    v_color0 = vec4(lit, a_color0.a);
}
