// The 3D mesh shaders' varyings and attributes, flat shaded: the GS's IIP bit cleared on a shader means its triangles take
// one vertex's values for the whole primitive instead of interpolating (the provoking vertex's -- the last one the GS sees
// for that triangle), which GLSL/HLSL/SPIR-V spell as the `flat` interpolation qualifier. Both the texture fractions and the
// lit colour are flat, as IIP clears every interpolated field on the console, not just the colour.
//
// Everything else is varying_draw3d.def.sc's, and the sources below it include the interpolated ones' bodies so the two can
// never drift apart.
flat vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
flat vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);

vec3 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
vec4 a_normal    : NORMAL;
vec3 a_indices   : BLENDINDICES;
vec3 a_weight    : BLENDWEIGHT;

// varying_draw3d.def.sc's instance matrix, here too because the flat rigid source includes the interpolated one's body
vec4 i_data0 : TEXCOORD7;
vec4 i_data1 : TEXCOORD6;
vec4 i_data2 : TEXCOORD5;
vec4 i_data3 : TEXCOORD4;
vec4 i_data4 : TEXCOORD3;
vec4 i_data5 : TEXCOORD2;
vec4 i_data6 : TEXCOORD1;
