// Maths: CPU implementations of the helpers the PS2 runs on VU0
// (src/platform/ps2/math.cpp). Register flows are noted where retail numerics
// depend on them.
#include "platform/math.h"

#include "game/math.h"

#include <cmath>

// A particle view's state: the frustum's planes in the chunk's space and the camera's place, as VU0 programs 0xCB0,
// 0xA08 and 0xB90 read it from VU0 memory. The SDL3 platform fills these in (culling.cpp); they are weak so the null
// platform's build, which has no culling.cpp, falls back to the origin with nothing culled.
namespace Platform::Graphics
{
const Vector4* HostParticleViewPlanes(s32 view) __attribute__((weak));
const Vector4* HostParticleViewCamera(s32 view) __attribute__((weak));
const Matrix4x4* HostParticleViewMatrix(s32 view) __attribute__((weak));
}

namespace
{
// Rotations closer than this are lerped and renormalised instead of slerped. This is VU0 0x1C0's cutoff, a cosine of
// 0x3F7FBE77 (about 2.6 degrees). The game's own SlerpRotations (rotations.cpp, retail 0x187100) uses a different
// cutoff on purpose.
constexpr f32 SlerpStraightCosine = 0.999f;
// A ray parallel to a triangle's plane within this never hits it
constexpr f32 RayParallelEpsilon = 0x1.0p-30f;
// How far outside its edges a triangle still catches a ray: the microprogram's LOI 0.1, added to each edge plane's
// distance before the sign is read
constexpr f32 RayEdgeMargin = 0.1f;
// How many planes a view's frustum has: the near and the far plane and the four sides, the six TransformPlanes moves
constexpr u32 ViewPlaneCount = 6;

// The game's ArcCosine (src/game/rotations.cpp) exactly: the value's magnitude, its complement's root times the arc cosine of
// the magnitude, and the angle taken off a half turn when the value was negative
f32 ArcCosine(f32 cosine)
{
    f32 value = __builtin_fabsf(cosine);
    f32 complement = 1.0f - value;
    if (complement < 0.0f)
    {
        complement = 0.0f;
    }

    f32 radians = ArcCosineOfPositive(value) * __builtin_sqrtf(complement);
    if (cosine < 0.0f)
    {
        radians = Pi - radians;
    }

    return radians;
}

// The camera's place in a view loaded for the frame's particles, the origin for a view nothing was loaded for (a view
// of the null platform's own build, whose loader keeps none, or an id this frame's particles never loaded)
void ViewCameraOf(s32 view, Vector4* camera)
{
    *camera = {0.0f, 0.0f, 0.0f, 1.0f};
    if (Platform::Graphics::HostParticleViewCamera != nullptr)
    {
        const Vector4* place = Platform::Graphics::HostParticleViewCamera(view);
        if (place != nullptr)
        {
            *camera = *place;
        }
    }
}

// A loaded view's planes (null for a view nothing was loaded for, which culls nothing)
const Vector4* ViewPlanesOf(s32 view)
{
    if (Platform::Graphics::HostParticleViewPlanes == nullptr)
    {
        return nullptr;
    }

    return Platform::Graphics::HostParticleViewPlanes(view);
}

// A loaded view's second matrix (the chunk's toScreen), null for a view nothing was loaded for
const Matrix4x4* ViewMatrixOf(s32 view)
{
    if (Platform::Graphics::HostParticleViewMatrix == nullptr)
    {
        return nullptr;
    }

    return Platform::Graphics::HostParticleViewMatrix(view);
}

// Whether a box of half extents around a place, its axes the first three rows of a gravity matrix, is wholly behind one
// of a view's planes: the box's farthest point along a plane's normal is the place's distance plus the extents weighed
// by the normal through each axis, and behind the plane when even that is negative (the planes point into the view)
bool BoxOutOfView(const Vector4* planes, const Vector4& place, const Matrix4x4* gravity, const f32* extents)
{
    for (u32 plane = 0; plane < ViewPlaneCount; plane++)
    {
        const Vector4& face = planes[plane];
        f32 distance = face.x * place.x + face.y * place.y + face.z * place.z + face.w;
        f32 reach = 0.0f;
        for (u32 axis = 0; axis < 3; axis++)
        {
            const Vector4* row = RowOf(gravity, axis);
            f32 along = face.x * row->x + face.y * row->y + face.z * row->z;
            reach += (along < 0.0f ? -along : along) * extents[axis];
        }

        if (distance + reach < 0.0f)
        {
            return true;
        }
    }

    return false;
}

// The ray kept for the triangle tests (SetRay's), and the triangle being
// tested (StartRayTriangle's vertexes)
Vector4 g_RayStart = {};
Vector4 g_RayEnd = {};
Vector4 g_RayTriangle[3] = {};

// A quaternion's dot product (w included: DotProduct only touches x, y and z)
f32 RotationDot(const Vector4* a, const Vector4* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
}
}

void Platform::Math::SinCos(f32 first, f32 second, f32* out)
{
    SinCosPolynomial(first, second, out);
}

// max.s reads "if fs > ft then fd = fs else fd = ft": the second operand wins a tie (+0.0 over -0.0) and any NaN
// comparison also gives it the second operand.
f32 Platform::Math::Max(f32 first, f32 second)
{
    return first > second ? first : second;
}

// min.s likewise: "if fs < ft then fd = fs else fd = ft"
f32 Platform::Math::Min(f32 first, f32 second)
{
    return first < second ? first : second;
}

void Platform::Math::SlerpRotations(const Vector4* from, const Vector4* to, f32 share, Vector4* out)
{
    if (!(share > 0.0f))
    {
        *out = *from;
        return;
    }

    if (!(share < 1.0f))
    {
        *out = *to;
        return;
    }

    // The shorter way round: the microprogram negates the target when the two are more than half a turn apart.
    f32 cosine = RotationDot(from, to);
    Vector4 target = *to;
    if (cosine < 0.0f)
    {
        cosine = -cosine;
        target.x = -target.x;
        target.y = -target.y;
        target.z = -target.z;
        target.w = -target.w;
    }

    if (!(cosine <= SlerpStraightCosine))
    {
        // Along a straight line: each end's share of one less the other's (0x1C0's vsub of the share from 1, then the two
        // weighted terms added in that order because VU0 rounds each step), made a unit one again
        f32 fromShare = 1.0f - share;
        out->x = from->x * fromShare + target.x * share;
        out->y = from->y * fromShare + target.y * share;
        out->z = from->z * fromShare + target.z * share;
        out->w = from->w * fromShare + target.w * share;
        f32 squared = out->x * out->x + out->y * out->y + out->z * out->z + out->w * out->w;
        f32 inverse = 0.0f;
        if (InverseEpsilon * InverseEpsilon < squared)
        {
            // A square root and a division: GCC makes them one RSQRT.S otherwise, which rounds differently
            f32 root = __builtin_sqrtf(squared);
            OPTIMIZER_BARRIER_F32(root);
            inverse = 1.0f / root;
        }

        out->x = out->x * inverse;
        out->y = out->y * inverse;
        out->z = out->z * inverse;
        out->w = out->w * inverse;
        return;
    }

    // Each weighted by the sine of its share of the angle over the angle's sine (from's by the sine of one less its share), the
    // arguments the microprogram gives its polynomial and the shares taken in that order, because VU0 rounds each step
    f32 angle = ArcCosine(cosine);
    f32 towardSine[4];
    SinCosPolynomial(angle * share, 0.0f, towardSine);
    f32 fromSine[4];
    SinCosPolynomial((1.0f - share) * angle, 0.0f, fromSine);
    f32 sineCos[4];
    SinCosPolynomial(angle, 0.0f, sineCos);
    f32 inverse = 1.0f / sineCos[0];
    f32 toShare = towardSine[0] * inverse;
    f32 fromShare = fromSine[0] * inverse;

    out->x = from->x * fromShare + target.x * toShare;
    out->y = from->y * fromShare + target.y * toShare;
    out->z = from->z * fromShare + target.z * toShare;
    out->w = from->w * fromShare + target.w * toShare;
}

void Platform::Math::EulerRotation(const Vector4* anglesNow, const Vector4* anglesNext, const Vector4* moveNow,
                                   const Vector4* moveNext, Vector4* rotation, Vector4* move)
{
    // The share between this frame's angles and the next's is in the next's w. Each angle the shorter way round: the
    // microprogram takes the difference back inside half a turn before the share is applied, so a frame pair that
    // straddles the ±π wrap doesn't swing the long way round
    f32 share = anglesNext->w;
    auto shortest = [](f32 from, f32 to)
    {
        f32 difference = to - from;
        if (Pi < difference)
        {
            difference = difference - TwoPi;
        }
        else if (difference < -Pi)
        {
            difference = difference + TwoPi;
        }
        else
        {
            return to;
        }

        return from + difference;
    };

    // Each angle as the microprogram blends it (0x818's vaddax of this frame's, vmsubaw of its own share, vmaddw of the
    // next's): the terms in that order because VU0 rounds each step
    f32 x = anglesNow->x - anglesNow->x * share + shortest(anglesNow->x, anglesNext->x) * share;
    f32 y = anglesNow->y - anglesNow->y * share + shortest(anglesNow->y, anglesNext->y) * share;
    f32 z = anglesNow->z - anglesNow->z * share + shortest(anglesNow->z, anglesNext->z) * share;
    // The rotation from the blended angles about x, y and z (turning about x, then y, then z), as GetRotationXYZ works it out
    f32 sinCosXZ[4];
    SinCosPolynomial(x * 0.5f, z * 0.5f, sinCosXZ);
    f32 sinCosY[4];
    SinCosPolynomial(y * 0.5f, y * 0.5f, sinCosY);
    f32 cosXcosZ = sinCosXZ[1] * sinCosXZ[3];
    f32 sinXsinZ = sinCosXZ[0] * sinCosXZ[2];
    f32 sinXcosZ = sinCosXZ[0] * sinCosXZ[3];
    f32 cosXsinZ = sinCosXZ[1] * sinCosXZ[2];
    rotation->x = sinXcosZ * sinCosY[1] - cosXsinZ * sinCosY[0];
    rotation->y = sinXsinZ * sinCosY[1] + cosXcosZ * sinCosY[0];
    rotation->z = cosXsinZ * sinCosY[1] - sinXcosZ * sinCosY[0];
    rotation->w = cosXcosZ * sinCosY[1] + sinXsinZ * sinCosY[0];
    // The translation the same share between the two, its terms in that same order for the same reason: VU0 rounds each step
    move->x = moveNow->x - moveNow->x * share + moveNext->x * share;
    move->y = moveNow->y - moveNow->y * share + moveNext->y * share;
    move->z = moveNow->z - moveNow->z * share + moveNext->z * share;
    move->w = 1.0f;
}

void Platform::Math::TurnRotation(const Vector4* by, Vector4* rotation)
{
    // The additional rotation goes on the left (retail 0xA70; TT-Lab's implementations write additional * rotation).
    // Reversing the order turns each flagged joint's subtree about its own axis.
    MultiplyRotations(rotation, by, rotation);
}

void Platform::Math::Lerp(const Vector4* from, const Vector4* to, f32 share, Vector4* out)
{
    // From + (to - from) share, its w 1 (the macro sequence's vmaddw by the share and vmove.w from vf0). The terms are taken
    // in the microprogram's order - the vaddax of the from, the vmsubaw of the from's own share, then the vmaddw of the to's -
    // because VU0 rounds each step
    out->x = from->x - from->x * share + to->x * share;
    out->y = from->y - from->y * share + to->y * share;
    out->z = from->z - from->z * share + to->z * share;
    out->w = 1.0f;
}

void Platform::Math::JointMatrix(const Vector4* rotation, const Vector4* parentScale, const Vector4* scale,
                                 const Vector4* translation, const Matrix4x4* parent, Matrix4x4* out)
{
    Matrix4x4 local;
    if (rotation != nullptr)
    {
        MatrixFromRotation(&local, rotation);
        // The rotation's own scale along each row (a scale before the matrix), and every column times the inverse of the
        // parent's scale along that column (0x548, 0x668 with a scale; 0x790 in the parent's space)
        f32 inverses[3] = {1.0f, 1.0f, 1.0f};
        if (parentScale != nullptr)
        {
            inverses[0] = 1.0f / parentScale->x;
            inverses[1] = 1.0f / parentScale->y;
            inverses[2] = 1.0f / parentScale->z;
        }

        for (u32 row = 0; row < 3; row++)
        {
            f32 own = scale != nullptr ? (&scale->x)[row] : 1.0f;
            for (u32 column = 0; column < 3; column++)
            {
                local.m[row][column] = local.m[row][column] * own * inverses[column];
            }
        }
    }
    else
    {
        // The identity's rows
        local = {};
        local.m[0][0] = 1.0f;
        local.m[1][1] = 1.0f;
        local.m[2][2] = 1.0f;
    }

    // The translation (none: (0, 0, 0, 1)) as the last row
    if (translation != nullptr)
    {
        *RowOf(&local, PositionRow) = *translation;
    }
    else
    {
        *RowOf(&local, PositionRow) = {0.0f, 0.0f, 0.0f, 1.0f};
    }

    if (parent != nullptr)
    {
        // In the parent's space (0x790: times the parent's)
        VuMultiplyMatrices(&local, parent, out);
        return;
    }

    *out = local;
}

void Platform::Math::MultiplyByParent(const Matrix4x4* matrix, const Matrix4x4* parent, Matrix4x4* out)
{
    // Times the parent's (0x790); out may be either
    VuMultiplyMatrices(matrix, parent, out);
}

f32 Platform::Math::ViewDistance(s32 view, const f32* point)
{
    // Between the point and the camera's place in the view (0xCB0 leaves it in vf30's x)
    Vector4 camera;
    ViewCameraOf(view, &camera);
    f32 x = point[0] - camera.x;
    f32 y = point[1] - camera.y;
    f32 z = point[2] - camera.z;
    return __builtin_sqrtf(x * x + y * y + z * z);
}

bool Platform::Math::ParticleBlockView(s32 view, bool keepsTranslation, const f32* extents, const Matrix4x4* gravity,
                                       const f32* place, f32 scale, Matrix4x4* matrix, f32* distance)
{
    Vector4 camera;
    ViewCameraOf(view, &camera);
    f32 x = place[0] - camera.x;
    f32 y = place[1] - camera.y;
    f32 z = place[2] - camera.z;
    f32 far = __builtin_sqrtf(x * x + y * y + z * z);
    *distance = far;
    // The place pulled towards the camera by the scale
    f32 pulled[3] = {place[0], place[1], place[2]};
    const f32 pull = scale;
    if (0.0f < far)
    {
        f32 along = pull / far;
        pulled[0] = place[0] - x * along;
        pulled[1] = place[1] - y * along;
        pulled[2] = place[2] - z * along;
    }

    // The block's matrix: the emitter's gravity axes as rows 0 to 2 (retail's 0xA08 writes vf29/vf28/vf27 from the
    // gravity the EE loaded into vf3/vf4/vf5), the place its particles are born at in row 3.
    //
    // These rows are the emitter's frame and the quad is built from them, so a flamethrower's puffs lie along the
    // emitter's own up axis. Leaving them the identity dropped every emission that does not transform its own starts
    // (GenParticle_Ranges and the rest write start and velocity straight from the data, with no emitMatrix) onto the
    // world's axes, which stood the flame up instead of pointing it where the weapon does. Only the generators that do
    // apply the emission frame themselves -- GenParticle_Box's BoxStart/BoxVelocity -- would see it twice, and retail
    // applies it there as well
    *RowOf(matrix, 0) = *RowOf(gravity, 0);
    *RowOf(matrix, 1) = *RowOf(gravity, 1);
    *RowOf(matrix, 2) = *RowOf(gravity, 2);
    // Row 3 holds the pulled place, but only for an emission that does not keep its translation. Those emissions have
    // their emitMatrix's translation cleared, so each particle's start is the spread alone and the place must come from
    // this row (retail 0xA08). An emission that keeps its translation (StartEmitterKeepingTranslation) already has the
    // emitter's place in every particle's start, so its row stays empty (retail 0xB90); putting the place there as well
    // would count it twice.
    Vector4 at = keepsTranslation ? Vector4{0.0f, 0.0f, 0.0f, 1.0f} : Vector4{pulled[0], pulled[1], pulled[2], 1.0f};

    *RowOf(matrix, PositionRow) = at;
    // Fold in the view's second matrix (the chunk's toScreen, as retail's UploadParticleView does). The block's matrix
    // takes the particle's units into the chunk's space, and toScreen takes the chunk's into the game's screen
    // coordinates. The row-vector product applies the first matrix first, as every other draw that pairs the two does.
    const Matrix4x4* toScreen = ViewMatrixOf(view);
    if (toScreen != nullptr)
    {
        Matrix4x4 composed;
        VuMultiplyMatrices(matrix, toScreen, &composed);
        *matrix = composed;
    }
    // Cull the block's box, at the emitter's place (before the pull), against the view's planes. A box that only crosses
    // a plane is still drawn, and a view with no planes culls nothing.
    const Vector4* planes = ViewPlanesOf(view);
    if (planes == nullptr)
    {
        return false;
    }

    const Vector4 emitterPlace = {place[0], place[1], place[2], 1.0f};
    return BoxOutOfView(planes, emitterPlace, gravity, extents);
}

f32 Platform::Math::DivideBySquareRoot(f32 value, f32 square)
{
    // VU0's division by a square root: the magnitude's root, and the division by nothing left to the FPU (an infinity or a
    // NaN, as VU0's own is) rather than the host's divide-by-zero
    if (square == 0.0f)
    {
        f32 infinite = value / square;
        OPTIMIZER_BARRIER_F32(infinite);
        return infinite;
    }

    f32 result = static_cast<f32>(static_cast<double>(value) / __builtin_sqrt(static_cast<double>(__builtin_fabsf(square))));
    OPTIMIZER_BARRIER_F32(result);
    return result;
}

void Platform::Math::SetRay(const Vector4* start, const Vector4* end)
{
    g_RayStart = *start;
    g_RayEnd = *end;
}

void Platform::Math::StartRayTriangle(const Vector4* vertices, const Vector4* nearest)
{
    (void)nearest;
    g_RayTriangle[0] = vertices[0];
    g_RayTriangle[1] = vertices[1];
    g_RayTriangle[2] = vertices[2];
}

bool Platform::Math::FinishRayTriangle(Vector4* hit)
{
    // Retail's test (VU0 0xAB8): the ray's ends on opposite sides of the triangle's plane, the crossing nearer than the
    // hit kept so far, and the hit within RayEdgeMargin of all three edges. The margin is retail's LOI 0.1, so a ray up
    // to that far outside a triangle still hits it. Exact barycentric bounds miss wall borders, thin steps and cracks
    // between triangles, which the camera's clearance cast notices.
    const Vector4& a = g_RayTriangle[0];
    const Vector4& b = g_RayTriangle[1];
    const Vector4& c = g_RayTriangle[2];
    Vector4 edge1 = {b.x - a.x, b.y - a.y, b.z - a.z, 0.0f};
    Vector4 edge2 = {c.x - a.x, c.y - a.y, c.z - a.z, 0.0f};
    // The triangle's plane normal, not made unit: the two ends' distances are only used for their signs and their ratio
    Vector4 normal = {edge1.y * edge2.z - edge1.z * edge2.y, edge1.z * edge2.x - edge1.x * edge2.z,
                      edge1.x * edge2.y - edge1.y * edge2.x, 0.0f};
    f32 squared = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
    if (!(RayParallelEpsilon < squared))
    {
        // A triangle with no area (the microprogram's own division by nothing)
        return false;
    }

    f32 startDistance = normal.x * (g_RayStart.x - a.x) + normal.y * (g_RayStart.y - a.y) +
                        normal.z * (g_RayStart.z - a.z);
    f32 endDistance = normal.x * (g_RayEnd.x - a.x) + normal.y * (g_RayEnd.y - a.y) +
                      normal.z * (g_RayEnd.z - a.z);
    // Opposite sides (a zero counts as neither, so an end lying on the plane is not a crossing by itself)
    if ((0.0f <= startDistance) == (0.0f <= endDistance))
    {
        return false;
    }

    f32 total = __builtin_fabsf(startDistance) + __builtin_fabsf(endDistance);
    if (!(0.0f < total))
    {
        return false;
    }

    f32 share = __builtin_fabsf(startDistance) / total;
    // Nearer than the hit kept so far (its w the share of the way), and inside the ray's own segment
    if (!(0.0f <= share) || !(share <= 1.0f) || !(share < hit->w))
    {
        return false;
    }

    // The crossing the microprogram's vaddax/vmsubaw/vmaddw chain gives: the start, less its own share, plus the end's
    // share - the terms in that order because VU0 rounds each step
    Vector4 point = {g_RayStart.x - g_RayStart.x * share + g_RayEnd.x * share,
                     g_RayStart.y - g_RayStart.y * share + g_RayEnd.y * share,
                     g_RayStart.z - g_RayStart.z * share + g_RayEnd.z * share, 1.0f};

    // Each edge's distance from the hit, in world units and positive inside. The edge turned a quarter turn about the
    // plane normal points inward whichever way the triangle is wound, so no facing test is needed.
    f32 inverseLength = 1.0f / __builtin_sqrtf(squared);
    f32 unitX = normal.x * inverseLength;
    f32 unitY = normal.y * inverseLength;
    f32 unitZ = normal.z * inverseLength;
    struct Edge
    {
        const Vector4* from;
        const Vector4* to;
    };
    const Edge edges[3] = {{&a, &b}, {&b, &c}, {&c, &a}};
    for (u32 index = 0; index < 3; index++)
    {
        const Vector4& from = *edges[index].from;
        const Vector4& to = *edges[index].to;
        Vector4 along = {to.x - from.x, to.y - from.y, to.z - from.z, 0.0f};
        // cross(unit normal, along), whose length is the edge's (the two are square to each other)
        f32 inwardX = unitY * along.z - unitZ * along.y;
        f32 inwardY = unitZ * along.x - unitX * along.z;
        f32 inwardZ = unitX * along.y - unitY * along.x;
        f32 offsetX = point.x - from.x;
        f32 offsetY = point.y - from.y;
        f32 offsetZ = point.z - from.z;
        f32 lengthSquared = along.x * along.x + along.y * along.y + along.z * along.z;
        if (!(0.0f < lengthSquared))
        {
            continue;
        }

        f32 distance = (inwardX * offsetX + inwardY * offsetY + inwardZ * offsetZ) / __builtin_sqrtf(lengthSquared);
        // Outside this edge by more than the margin
        if (distance + RayEdgeMargin < 0.0f)
        {
            return false;
        }
    }

    hit->x = point.x;
    hit->y = point.y;
    hit->z = point.z;
    hit->w = share;
    return true;
}
