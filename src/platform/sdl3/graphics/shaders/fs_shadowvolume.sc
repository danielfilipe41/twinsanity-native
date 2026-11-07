// The shadow volumes' fragment shader: ONE constant colour, so a shell contributes exactly the state's blend factor and not a
// fraction of it. The mesh's own vertex colours must not enter: retail's Color Only (0x0B, bucket 21) writes the record's
// constant +-8 per shell, not the mesh's, and a shell has to count the same in whichever channel a pass lands in. The volumes'
// pass (shadows.cpp) counts the away shells in R and the facing ones in G, 8/255 each, and the resolve's count of shells is
// their difference.
//
// Which channel is the face's own facing: the blend adds this colour times the state's factor, so a shell away from the camera
// adds 8/255 to R and one facing it 8/255 to G -- the same pair of draws the pass used to make, in one, when the state writes
// every channel (shadows.cpp's VolumeStateBoth). A pass that writes one channel instead still gets its own: the mask picks the
// channel and the facing agrees with the mask, since the cull that drew the shell is what decided the facing
$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = gl_FrontFacing ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
