// The host decals' and distortion's varyings: clip-space positions straight from the CPU, the texture's S, T and Q
// (the decals' corners are projected; the distortion's hexagons pass Q of 1) and the premultiplied colour
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);

vec3 a_position  : POSITION;
vec3 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
