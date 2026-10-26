// Maths: CPU implementations of the helpers the PS2 runs on VU0
// (src/platform/ps2/math.cpp). Register flows are noted where retail numerics
// depend on them.
#include "platform/math.h"

#include "game/math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// The arc cosine of a value from 0 to 1 (radians), as the game's ArcCosine
// works it out (src/game/rotations.cpp)
f32 ArcCosine(f32 cosine)
{
    f32 complement = 1.0f - cosine;
    if (complement < 0.0f)
    {
        complement = 0.0f;
    }

    return ArcCosineOfPositive(cosine) * __builtin_sqrtf(complement);
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
        // Along a straight line, made a unit one again
        out->x = (target.x - from->x) * share + from->x;
        out->y = (target.y - from->y) * share + from->y;
        out->z = (target.z - from->z) * share + from->z;
        out->w = (target.w - from->w) * share + from->w;
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

    // Each weighted by the sine of the other's share of the angle over the angle's sine (from's by sin((share - 1) angle))
    f32 angle = ArcCosine(cosine);
    f32 shares[4];
    SinCosPolynomial(angle * share, angle * share - angle, shares);
    f32 sineCos[4];
    SinCosPolynomial(angle, angle, sineCos);
    f32 inverse = 1.0f / sineCos[0];
    f32 toShare = shares[0] * inverse;
    f32 fromShare = -shares[2] * inverse;

    out->x = from->x * fromShare + target.x * toShare;
    out->y = from->y * fromShare + target.y * toShare;
    out->z = from->z * fromShare + target.z * toShare;
    out->w = from->w * fromShare + target.w * toShare;
}

void Platform::Math::EulerRotation(const Vector4* anglesNow, const Vector4* anglesNext, const Vector4* moveNow,
                                   const Vector4* moveNext, Vector4* rotation, Vector4* move)
{
    // The share between this frame's angles and the next's is in the next's w
    f32 share = anglesNext->w;
    f32 x = (anglesNext->x - anglesNow->x) * share + anglesNow->x;
    f32 y = (anglesNext->y - anglesNow->y) * share + anglesNow->y;
    f32 z = (anglesNext->z - anglesNow->z) * share + anglesNow->z;
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
    // The translation the same share between the two
    move->x = (moveNext->x - moveNow->x) * share + moveNow->x;
    move->y = (moveNext->y - moveNow->y) * share + moveNow->y;
    move->z = (moveNext->z - moveNow->z) * share + moveNow->z;
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
    // From + (to - from) share, its w 1 (the macro sequence's vmaddw by the share and vmove.w from vf0)
    out->x = (to->x - from->x) * share + from->x;
    out->y = (to->y - from->y) * share + from->y;
    out->z = (to->z - from->z) * share + from->z;
    out->w = 1.0f;
}

void Platform::Math::JointMatrix(const Vector4* rotation, const Vector4* parentScale, const Vector4* scale,
                                 const Vector4* translation, const Matrix4x4* parent, Matrix4x4* out)
{
    Matrix4x4 local;
    if (rotation != nullptr)
    {
        MatrixFromRotation(&local, rotation);
        // Divided by the parent's scale and scaled by its own (0x548, 0x668 with a scale): each row by its own factor, a
        // scale before the matrix
        for (u32 row = 0; row < 3; row++)
        {
            f32 factor = 1.0f;
            if (parentScale != nullptr)
            {
                factor /= (&parentScale->x)[row];
            }

            if (scale != nullptr)
            {
                factor *= (&scale->x)[row];
            }

            local.m[row][0] *= factor;
            local.m[row][1] *= factor;
            local.m[row][2] *= factor;
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
    // TS_NOPULL=1: skip the pull and use the emitter's place unchanged.
    static const bool noPull = std::getenv("TS_NOPULL") != nullptr;
    const f32 pull = noPull ? 0.0f : scale;
    if (0.0f < far)
    {
        f32 along = pull / far;
        pulled[0] = place[0] - x * along;
        pulled[1] = place[1] - y * along;
        pulled[2] = place[2] - z * along;
    }

    // TS_DIAG=1: log every block's place, pull, camera and matrix to particleview.log.
    static const bool placeDiag = std::getenv("TS_DIAG") != nullptr;
    static u32 placeLines = 0;
    const bool logPlace = placeDiag && placeLines < 4000;
    if (logPlace)
    {
        placeLines++;
    }

    // The block's matrix: identity in rows 0 to 2, the place the block's particles are born at in row 3.
    //
    // Rows 0 to 2 are the identity, not the gravity's axes. The generators already turn each particle's start and
    // velocity by the emission frame (SetEmitterEmission's emitMatrix), so using the gravity rows here turns the spread
    // and flight twice. Leaving the columns alone also keeps the quads facing the camera: the draw takes the quad's
    // size from the first two columns. The gravity's axes are still used for the culling box below.
    *RowOf(matrix, 0) = {1.0f, 0.0f, 0.0f, 0.0f};
    *RowOf(matrix, 1) = {0.0f, 1.0f, 0.0f, 0.0f};
    *RowOf(matrix, 2) = {0.0f, 0.0f, 1.0f, 0.0f};
    // Row 3 holds the pulled place, but only for an emission that does not keep its translation. Those emissions have
    // their emitMatrix's translation cleared, so each particle's start is the spread alone and the place must come from
    // this row (retail 0xA08). An emission that keeps its translation (StartEmitterKeepingTranslation) already has the
    // emitter's place in every particle's start, so its row stays empty (retail 0xB90); putting the place there as well
    // would count it twice.
    Vector4 at = keepsTranslation ? Vector4{0.0f, 0.0f, 0.0f, 1.0f} : Vector4{pulled[0], pulled[1], pulled[2], 1.0f};

    *RowOf(matrix, PositionRow) = at;
    // The block's translation row before the view's matrix is folded in.
    const Vector4 blockRow = *RowOf(matrix, PositionRow);
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
    if (logPlace)
    {
        // The draw's place, the emitter's place, the pull, the camera and the final row, in one line each draw.
        const Vector4* row = RowOf(matrix, PositionRow);
        std::FILE* file = std::fopen("particleview.log", "ab");
        if (file != nullptr)
        {
            std::fprintf(file,
                         "particleview view %d kept %d place %.2f %.2f %.2f scale %.4f camera %.2f %.2f %.2f far %.2f "
                         "pulled %.2f %.2f %.2f block %.2f %.2f %.2f row %.2f %.2f %.2f w %.3f\n",
                         view, keepsTranslation ? 1 : 0, static_cast<double>(place[0]), static_cast<double>(place[1]),
                         static_cast<double>(place[2]), static_cast<double>(scale), static_cast<double>(camera.x),
                         static_cast<double>(camera.y), static_cast<double>(camera.z), static_cast<double>(far),
                         static_cast<double>(pulled[0]), static_cast<double>(pulled[1]), static_cast<double>(pulled[2]),
                         static_cast<double>(blockRow.x), static_cast<double>(blockRow.y), static_cast<double>(blockRow.z),
                         static_cast<double>(row->x), static_cast<double>(row->y), static_cast<double>(row->z),
                         static_cast<double>(row->w));
            std::fclose(file);
        }
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
    f32 root = std::sqrt(square);
    f32 result = value / root;
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
    Vector4 delta = {g_RayEnd.x - g_RayStart.x, g_RayEnd.y - g_RayStart.y, g_RayEnd.z - g_RayStart.z, 0.0f};
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

    Vector4 point = {g_RayStart.x + delta.x * share, g_RayStart.y + delta.y * share,
                     g_RayStart.z + delta.z * share, 1.0f};

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
