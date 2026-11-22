// The 3D mesh shaders' varyings and attributes: the texture fractions and the
// lit colour straight through; positions, UVs, colours, normals and the skin
// joint bytes (indices raw 0-255, weights/colour/normal normalized) as the
// mesh_builder lays them out.
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);

vec3 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
vec4 a_normal    : NORMAL;
vec3 a_indices   : BLENDINDICES;
vec3 a_weight    : BLENDWEIGHT;

// The rigid draws' per-instance model-to-clip matrix: one instance per draw of a packet, so a packet the level places many times
// is one submit. TEXCOORD31 down are the slots bgfx's instance data fills (the particles take theirs the same way), and the mesh
// vertexes use TEXCOORD0 alone. The numbers matter only to Direct3D, which binds an attribute by its semantic: its renderer
// builds the instance vertex buffer's elements at TEXCOORD(31 - i), so a lower number here (TEXCOORD7 was this file's, harmless
// on GL, which binds by name) leaves every instance attribute unfed and every rigid mesh at a zero matrix
vec4 i_data0 : TEXCOORD31;
vec4 i_data1 : TEXCOORD30;
vec4 i_data2 : TEXCOORD29;
vec4 i_data3 : TEXCOORD28;
vec4 i_data4 : TEXCOORD27;
vec4 i_data5 : TEXCOORD26;
vec4 i_data6 : TEXCOORD25;
