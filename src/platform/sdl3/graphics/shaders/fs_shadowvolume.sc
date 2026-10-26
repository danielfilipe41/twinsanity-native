// The shadow volumes' fragment shader: ONE constant colour, so a shell contributes exactly the state's blend factor and not a
// fraction of it. The mesh's own vertex colours must not enter: retail's Color Only (0x0B, bucket 21) writes the record's
// constant +-8 per shell, not the mesh's, and a shell has to count the same in whichever channel a pass lands in. The volumes'
// pass (shadows.cpp) draws each volume twice, the away shells into R and the facing ones into G, 8/255 each, and the resolve's
// count of shells is their difference
$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
