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
// is one submit. TEXCOORD7 down are the slots bgfx's instance data fills (the particles take theirs the same way), and the mesh
// vertexes use TEXCOORD0 alone
vec4 i_data0 : TEXCOORD7;
vec4 i_data1 : TEXCOORD6;
vec4 i_data2 : TEXCOORD5;
vec4 i_data3 : TEXCOORD4;
vec4 i_data4 : TEXCOORD3;
vec4 i_data5 : TEXCOORD2;
vec4 i_data6 : TEXCOORD1;
