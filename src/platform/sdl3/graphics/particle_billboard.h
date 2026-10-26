// The camera axes and screen scale a particle quad is built with.
//
// A particle quad faces the camera, with its size along the camera's right and up. The console's VU1 builds it in
// screen space: the particle's place goes through the block matrix, the shape is added along the screen's axes, and
// the view's matrix to the screen supplies the scales. The host cannot take those axes from the block matrix, whose
// rows and columns carry the screen's translation, so it uses the view matrix the same way the console does.
#pragma once

#include "game/math.h"
#include "game/renderer.h"

#include <cmath>

namespace Platform::Graphics
{
// The table's shape is in ten thousandths of a world unit (ParticleSimulation's SizeUnit and the asm's shape)
constexpr f32 ParticleSizeUnit = 0x1.A36E2Ep-14f;

// The screen units a world unit takes along the camera's right and up (the lengths of the view matrix to the screen's
// first two rows). The frame's culling loads this view, and every view of a frame shares it.
//
// The two scales differ: the console's 512x512 display is square, but the projection is made for the TV and squashes
// screen x against y. Retail's shape data is square (the waterfall's mist is 19995 by 19995), so the host scales the
// shape's x by the ratio of the two scales to keep round puffs round.
inline void ParticleScreenScale(f32* across, f32* down)
{
    const Matrix4x4& toScreen = HostParticleToScreen();
    f32 x = 0.0f;
    f32 y = 0.0f;
    for (u32 lane = 0; lane < 3; lane++)
    {
        x += toScreen.m[0][lane] * toScreen.m[0][lane];
        y += toScreen.m[1][lane] * toScreen.m[1][lane];
    }

    *across = std::sqrt(x);
    *down = std::sqrt(y);
}

// The factor the shape's x has to be multiplied by to land on the screen with the same size per unit as its y: the
// screen's x is squashed against its y (the projection is made for the TV, the frame is the display's own square
// size), and the retail's shape is square in the data's terms.
inline f32 ShapeScreenRatio(f32 across, f32 down)
{
    return across > 0.0f ? down / across : 1.0f;
}

// The shape's numbers are the quad's HALF extents, not its corners: the retail's shape is a rectangle of the width
// and height the curves give, doubled and stretched by the display's aspect, and the console's VU1 builds the quad
// around the particle's place with the halves. The retail's own culling box says so: it is
// 0.75 * maxSize/10000 to the side, and a puff has to sit inside it, so a puff is half the extent the shape's
// numbers spell out.
constexpr f32 ShapeHalfExtent = 0.5f;
} // namespace Platform::Graphics
