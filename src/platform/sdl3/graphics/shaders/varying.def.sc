// The 2D quad shaders' varyings: the texture fractions straight through, the colour premultiplied on the CPU
// (texture paths by 1/128 like the GS's modulate, flat strips by 1/255 like RGBAQ straight).
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);

vec3 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
