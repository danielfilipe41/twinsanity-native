// The host particles' varyings: the texture fractions the render table's rectangle gives and the colour premultiplied
// on the CPU (a table byte over 128, the GS's modulate at Q=1 with 0x80 the identity)
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);

// The unit quad's corner: x picks the edge (0 the first corner, 1 the other), y the side of the edge
vec3 a_position  : POSITION;

// One particle an instance: its clip position, the quad's four corners (two a quadword), its premultiplied colour
// and the texture rectangle - all the CPU worked out from the block's record and the system's render table
vec4 i_data0 : TEXCOORD7;
vec4 i_data1 : TEXCOORD6;
vec4 i_data2 : TEXCOORD5;
vec4 i_data3 : TEXCOORD4;
vec4 i_data4 : TEXCOORD3;
vec4 i_data5 : TEXCOORD2;
