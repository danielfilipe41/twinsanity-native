// VU0 macro-mode maths on the CPU: the CPU versions of the functions
// src/platform/ps2/matrices.cpp and src/platform/ps2/bounds.cpp implement with VU0
// macro instructions. The null platform only needs the results.
#include <cstdio>
#include <cstdlib>
#include "game/collision.h"
#include "game/hull.h"
#include "game/math.h"
#include "game/physics.h"
#include "platform/math.h"

namespace
{
// A box's corners
constexpr u32 BoxCornerCount = 8;
// What the groups' ranges start from: their least 2^65, their most -2^65
constexpr f32 RangeStart = 0x1.0p+65f;
// The triangle's vertexes less the box hull's
constexpr u32 TriangleVertexCount = 3;
constexpr u32 BoxHullVertexCount = 8;

// The edge tests' second half from the first (VU0's registers on the PS2): each edge's cross product with the ray and the ray's
// start's ways to the two vertexes it's tested against
struct EdgeTests
{
    Vector4 crosses[3];
    Vector4 ways[3][2];
};
EdgeTests g_EdgeTests;

// The triangle's vertexes less the box hull's (VU0's registers on the PS2, grouped four at a time)
Vector4 g_SupportDifferences[TriangleVertexCount][BoxHullVertexCount];

// A row vector through a matrix: the first three rows times its x, y and z, then the fourth times a w, summed in that order
Vector4 Through(const Matrix4x4* matrix, const Vector4* vector, f32 w)
{
    Vector4 through;
    f32* lanes = &through.x;
    for (u32 lane = 0; lane < 4; lane++)
    {
        lanes[lane] = matrix->m[0][lane] * vector->x + matrix->m[1][lane] * vector->y + matrix->m[2][lane] * vector->z +
                      matrix->m[3][lane] * w;
    }

    return through;
}

// VU0's outer product pair (OPMULA, then OPMSUB): a cross b, each part a's product less b's
Vector4 Cross(const Vector4& a, const Vector4& b)
{
    Vector4 cross = {};
    cross.x = a.y * b.z - b.y * a.z;
    cross.y = a.z * b.x - b.z * a.x;
    cross.z = a.x * b.y - b.x * a.y;
    return cross;
}

f32 Dot(const Vector4& a, const Vector4& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector4 Difference(const Vector4& from, const Vector4& to)
{
    Vector4 difference = {};
    difference.x = to.x - from.x;
    difference.y = to.y - from.y;
    difference.z = to.z - from.z;
    return difference;
}

// Four values taken into the least and the most so far, and the least and the most of those four into a range
void TakeIntoRange(Vector4* least, Vector4* most, const Vector4& values)
{
    least->x = Platform::Math::Min(least->x, values.x);
    least->y = Platform::Math::Min(least->y, values.y);
    least->z = Platform::Math::Min(least->z, values.z);
    least->w = Platform::Math::Min(least->w, values.w);
    most->x = Platform::Math::Max(most->x, values.x);
    most->y = Platform::Math::Max(most->y, values.y);
    most->z = Platform::Math::Max(most->z, values.z);
    most->w = Platform::Math::Max(most->w, values.w);
}

void StoreRange(const Vector4& least, const Vector4& most, f32* range)
{
    range[0] = Platform::Math::Min(Platform::Math::Min(least.x, least.y), Platform::Math::Min(least.z, least.w));
    range[1] = Platform::Math::Max(Platform::Math::Max(most.x, most.y), Platform::Math::Max(most.z, most.w));
}

// A group's four points (their xs, ys and zs) along an axis
Vector4 GroupAlong(const Vector4* group, const Vector4* axis)
{
    const f32* xs = &group[0].x;
    const f32* ys = &group[1].x;
    const f32* zs = &group[2].x;
    Vector4 along;
    f32* lanes = &along.x;
    for (u32 lane = 0; lane < 4; lane++)
    {
        lanes[lane] = xs[lane] * axis->x + ys[lane] * axis->y + zs[lane] * axis->z;
    }

    return along;
}
}

extern "C"
{
void InitIdentityMatrix(Matrix4x4* matrix)
{
    // vf0 is (0, 0, 0, 1): the rows the PS2's vmr32 and vaddw make of it
    matrix->m[0][0] = 1.0f;
    matrix->m[0][1] = 0.0f;
    matrix->m[0][2] = 0.0f;
    matrix->m[0][3] = 0.0f;
    matrix->m[1][0] = 0.0f;
    matrix->m[1][1] = 1.0f;
    matrix->m[1][2] = 0.0f;
    matrix->m[1][3] = 0.0f;
    matrix->m[2][0] = 0.0f;
    matrix->m[2][1] = 0.0f;
    matrix->m[2][2] = 1.0f;
    matrix->m[2][3] = 0.0f;
    matrix->m[3][0] = 0.0f;
    matrix->m[3][1] = 0.0f;
    matrix->m[3][2] = 0.0f;
    matrix->m[3][3] = 1.0f;
}

void VuTranspose(const Matrix4x4* matrix, Matrix4x4* out)
{
    // The PS2's pextlw/pextuw/pcpyld/pcpyud word shuffle: every row made a column
    Matrix4x4 transposed;
    for (s32 row = 0; row < 4; row++)
    {
        for (s32 column = 0; column < 4; column++)
        {
            transposed.m[row][column] = matrix->m[column][row];
        }
    }

    *out = transposed;
}

void TransposeQuadwords(void* rows)
{
    // The same shuffle in place
    auto* matrix = static_cast<Matrix4x4*>(rows);
    Matrix4x4 transposed;
    for (s32 row = 0; row < 4; row++)
    {
        for (s32 column = 0; column < 4; column++)
        {
            transposed.m[row][column] = matrix->m[column][row];
        }
    }

    *matrix = transposed;
}

void VuTransposeRotation(const Matrix4x4* matrix, Matrix4x4* out)
{
    // The rotation's rows made its columns, the fourth row vf0's (0, 0, 0, 1)
    Matrix4x4 transposed;
    for (s32 row = 0; row < 3; row++)
    {
        for (s32 column = 0; column < 3; column++)
        {
            transposed.m[row][column] = matrix->m[column][row];
        }

        transposed.m[row][3] = 0.0f;
    }

    transposed.m[3][0] = 0.0f;
    transposed.m[3][1] = 0.0f;
    transposed.m[3][2] = 0.0f;
    transposed.m[3][3] = 1.0f;
    *out = transposed;
}

void VuInvertRigid(Matrix4x4* out, const Matrix4x4* matrix)
{
    // The rotation transposed, the translation turned by it and negated. The position
    // is read first: the PS2's sequence works in registers, and in place is a caller
    Vector4 position = *RowOf(matrix, PositionRow);
    Matrix4x4 inverse;
    for (s32 row = 0; row < 3; row++)
    {
        for (s32 column = 0; column < 3; column++)
        {
            inverse.m[row][column] = matrix->m[column][row];
        }

        inverse.m[row][3] = 0.0f;
    }

    const f32* place = reinterpret_cast<const f32*>(&position);
    for (s32 column = 0; column < 3; column++)
    {
        inverse.m[PositionRow][column] =
            -(place[0] * inverse.m[0][column] + place[1] * inverse.m[1][column] + place[2] * inverse.m[2][column]);
    }

    inverse.m[PositionRow][PositionRow] = 1.0f;
    *out = inverse;
}
void VuInvertRigidInPlace(Matrix4x4* matrix)
{
    VuInvertRigid(matrix, matrix);
}

void MultiplyRotations(Vector4* out, const Vector4* a, const Vector4* b)
{
    // a's w times b's vector plus b's w times a's, plus their cross product (the PS2's
    // vopmula/vopmsub of b by a leaves a's cross b in the vector); the w the product of
    // the ws less the dot product
    f32 x = a->w * b->x + b->w * a->x + (a->y * b->z - a->z * b->y);
    f32 y = a->w * b->y + b->w * a->y + (a->z * b->x - a->x * b->z);
    f32 z = a->w * b->z + b->w * a->z + (a->x * b->y - a->y * b->x);
    f32 w = a->w * b->w - (a->x * b->x + a->y * b->y + a->z * b->z);
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
}

void VuMultiplyMatrices(const Matrix4x4* a, const Matrix4x4* b, Matrix4x4* out)
{
    // Each of a's rows through b's: the PS2's vmulax/vmadday/vmaddaz/vmaddw row by row
    Matrix4x4 product;
    for (s32 row = 0; row < 4; row++)
    {
        for (s32 column = 0; column < 4; column++)
        {
            product.m[row][column] = a->m[row][0] * b->m[0][column] + a->m[row][1] * b->m[1][column] +
                                     a->m[row][2] * b->m[2][column] + a->m[row][3] * b->m[3][column];
        }
    }

    *out = product;
}

void VuTransformPoint(const Matrix4x4* matrix, const Vector4* point, Vector4* out)
{
    // The point's w taken as 1 (the PS2's vmaddw by vf0w): the rows weighted by x, y
    // and z, the fourth row whole. Its w the fourth row's w
    const auto& rows = matrix->m;
    f32 x = point->x * rows[0][0] + point->y * rows[1][0] + point->z * rows[2][0] + rows[PositionRow][0];
    f32 y = point->x * rows[0][1] + point->y * rows[1][1] + point->z * rows[2][1] + rows[PositionRow][1];
    f32 z = point->x * rows[0][2] + point->y * rows[1][2] + point->z * rows[2][2] + rows[PositionRow][2];
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = rows[PositionRow][PositionRow];
}

void VuRotateVector(const Matrix4x4* matrix, const Vector4* vector, Vector4* out)
{
    // Without the translation (the PS2's vmaddz into the vector itself): its w kept
    const auto& rows = matrix->m;
    f32 x = vector->x * rows[0][0] + vector->y * rows[1][0] + vector->z * rows[2][0];
    f32 y = vector->x * rows[0][1] + vector->y * rows[1][1] + vector->z * rows[2][1];
    f32 z = vector->x * rows[0][2] + vector->y * rows[1][2] + vector->z * rows[2][2];
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = vector->w;
}

void VuTransformByRows(const Matrix4x4* rows, const Vector4* point, Vector4* out)
{
    // The point through the four rows, its own w taking the fourth row's share
    const auto& matrices = rows->m;
    f32 x = point->x * matrices[0][0] + point->y * matrices[1][0] + point->z * matrices[2][0] +
            point->w * matrices[PositionRow][0];
    f32 y = point->x * matrices[0][1] + point->y * matrices[1][1] + point->z * matrices[2][1] +
            point->w * matrices[PositionRow][1];
    f32 z = point->x * matrices[0][2] + point->y * matrices[1][2] + point->z * matrices[2][2] +
            point->w * matrices[PositionRow][2];
    f32 w = point->x * matrices[0][3] + point->y * matrices[1][3] + point->z * matrices[2][3] +
            point->w * matrices[PositionRow][PositionRow];
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
}


void MatrixFromRotation(Matrix4x4* matrix, const Vector4* rotation)
{
    // The PS2's sequence scales the rotation by the square root of 2, so the products
    // of its parts come out doubled: the rows' w 0, the position row left as it was
    f32 x = rotation->x;
    f32 y = rotation->y;
    f32 z = rotation->z;
    f32 w = rotation->w;
    matrix->m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    matrix->m[0][1] = 2.0f * (x * y + z * w);
    matrix->m[0][2] = 2.0f * (z * x - y * w);
    matrix->m[0][3] = 0.0f;
    matrix->m[1][0] = 2.0f * (x * y - z * w);
    matrix->m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    matrix->m[1][2] = 2.0f * (y * z + x * w);
    matrix->m[1][3] = 0.0f;
    matrix->m[2][0] = 2.0f * (z * x + y * w);
    matrix->m[2][1] = 2.0f * (y * z - x * w);
    matrix->m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    matrix->m[2][3] = 0.0f;
}

namespace
{
// TS_PLANELOG=<path>: log the collision planes and count the triangles with no area. The file is opened on the first
// plane so an empty log still shows that the path ran.
FILE* g_PlaneLog = nullptr;
u32 g_ZeroPlanes = 0;
u32 g_AllPlanes = 0;
bool g_PlaneLogRead = false;

void OpenPlaneLog()
{
    if (g_PlaneLogRead)
    {
        return;
    }

    g_PlaneLogRead = true;
    const char* spec = std::getenv("TS_PLANELOG");
    if (spec != nullptr && spec[0] != '\0')
    {
        g_PlaneLog = std::fopen(spec, "w");
        if (g_PlaneLog != nullptr)
        {
            std::setvbuf(g_PlaneLog, nullptr, _IONBF, 0);
            std::fprintf(g_PlaneLog, "PLANES opened\n");
        }
    }
}
}

u32 PlaneThroughTriangle(Vector4* plane, const Vector4* first, const Vector4* second, const Vector4* third)
{
    // The cross product of the edges from the first vertex made a unit vector, its w
    // less how far along it the first vertex is
    f32 edgeX = second->x - first->x;
    f32 edgeY = second->y - first->y;
    f32 edgeZ = second->z - first->z;
    f32 otherX = third->x - first->x;
    f32 otherY = third->y - first->y;
    f32 otherZ = third->z - first->z;
    f32 normalX = edgeY * otherZ - edgeZ * otherY;
    f32 normalY = edgeZ * otherX - edgeX * otherZ;
    f32 normalZ = edgeX * otherY - edgeY * otherX;
    // A zero-area triangle gives a zero-length normal. A plain IEEE 1/0 would make a plane of NaNs that compares false
    // against everything; the console's divide returns the largest float instead, leaving a useless but finite plane.
    f32 length = __builtin_sqrtf(normalX * normalX + normalY * normalY + normalZ * normalZ);
    OpenPlaneLog();
    g_AllPlanes++;
    // Periodic tally so a short log is not mistaken for a path that never ran
    if (g_PlaneLog != nullptr && g_AllPlanes % 100000 == 0)
    {
        std::fprintf(g_PlaneLog, "PLANES %u of them, %u with no area\n", g_AllPlanes, g_ZeroPlanes);
    }

    if (length == 0.0f)
    {
        g_ZeroPlanes++;
        if (g_PlaneLog != nullptr && g_ZeroPlanes <= 200)
        {
            std::fprintf(g_PlaneLog, "ZEROPLANE %u %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f\n", g_ZeroPlanes,
                         first->x, first->y, first->z, second->x, second->y, second->z, third->x, third->y,
                         third->z);
        }
    }

    f32 scale = 1.0f / length;
    normalX = normalX * scale;
    normalY = normalY * scale;
    normalZ = normalZ * scale;
    plane->x = normalX;
    plane->y = normalY;
    plane->z = normalZ;
    plane->w = -(normalX * first->x + normalY * first->y + normalZ * first->z);
    return Epsilon < length;
}

void GetBbox(const Vector4* points, s32 count, Box* box, const Matrix4x4* matrix)
{
    // Each point moved by the matrix (its own w taking the fourth row's share, as in
    // VuTransformByRows), the smallest and the largest x, y and z kept; the corners'
    // w the first point's moved w
    Vector4 moved;
    VuTransformByRows(matrix, &points[0], &moved);
    Vector4 min = moved;
    Vector4 max = moved;
    for (s32 index = 1; index < count; index++)
    {
        VuTransformByRows(matrix, &points[index], &moved);
        if (moved.x < min.x)
        {
            min.x = moved.x;
        }

        if (max.x < moved.x)
        {
            max.x = moved.x;
        }

        if (moved.y < min.y)
        {
            min.y = moved.y;
        }

        if (max.y < moved.y)
        {
            max.y = moved.y;
        }

        if (moved.z < min.z)
        {
            min.z = moved.z;
        }

        if (max.z < moved.z)
        {
            max.z = moved.z;
        }
    }

    box->min = min;
    box->max = max;
}

void PointsBox(const Vector4* points, s32 count, Box* box)
{
    // The points' box: the first point in both corners, the rest taken in
    Vector4 min = points[0];
    Vector4 max = points[0];
    for (s32 index = 1; index < count; index++)
    {
        if (points[index].x < min.x)
        {
            min.x = points[index].x;
        }

        if (max.x < points[index].x)
        {
            max.x = points[index].x;
        }

        if (points[index].y < min.y)
        {
            min.y = points[index].y;
        }

        if (max.y < points[index].y)
        {
            max.y = points[index].y;
        }

        if (points[index].z < min.z)
        {
            min.z = points[index].z;
        }

        if (max.z < points[index].z)
        {
            max.z = points[index].z;
        }
    }

    box->min = min;
    box->max = max;
}

void TriangleBounds(Box* box, const Vector4* first, const Vector4* second, const Vector4* third)
{
    // The corners' least and largest x, y and z. The PS2 leaves their w what its
    // registers had; the first corner's w stands in for it deterministically
    box->min.x = first->x < second->x ? (first->x < third->x ? first->x : third->x) : (second->x < third->x ? second->x : third->x);
    box->min.y = first->y < second->y ? (first->y < third->y ? first->y : third->y) : (second->y < third->y ? second->y : third->y);
    box->min.z = first->z < second->z ? (first->z < third->z ? first->z : third->z) : (second->z < third->z ? second->z : third->z);
    box->min.w = first->w;
    box->max.x = first->x < second->x ? (second->x < third->x ? third->x : second->x) : (first->x < third->x ? third->x : first->x);
    box->max.y = first->y < second->y ? (second->y < third->y ? third->y : second->y) : (first->y < third->y ? third->y : first->y);
    box->max.z = first->z < second->z ? (second->z < third->z ? third->z : second->z) : (first->z < third->z ? third->z : first->z);
    box->max.w = first->w;
}

// The ray's ends' sides of the triangle's plane (its normal the edges' cross product, not a unit one), and each edge's cross
// product with the ray and the ways to the vertexes it's tested against kept for the rest
void StartTriangleEdgeTests(const Vector4* start, const Vector4* end, const CollisionHit* triangle, f32* values)
{
    const Vector4& first = triangle->vertices[0];
    const Vector4& second = triangle->vertices[1];
    const Vector4& third = triangle->vertices[2];
    Vector4 ray = Difference(*start, *end);
    Vector4 normal = Cross(Difference(first, second), Difference(first, third));
    g_EdgeTests.crosses[0] = Cross(ray, Difference(first, second));
    g_EdgeTests.crosses[1] = Cross(ray, Difference(second, third));
    g_EdgeTests.crosses[2] = Cross(ray, Difference(third, first));
    g_EdgeTests.ways[0][0] = Difference(*start, first);
    g_EdgeTests.ways[0][1] = Difference(*start, third);
    g_EdgeTests.ways[1][0] = Difference(*start, second);
    g_EdgeTests.ways[1][1] = Difference(*start, first);
    g_EdgeTests.ways[2][0] = Difference(*start, third);
    g_EdgeTests.ways[2][1] = Difference(*start, second);
    values[0] = Dot(Difference(first, *start), normal);
    values[1] = Dot(Difference(first, *end), normal);
}

// Each edge's sides of the ray's two ends: the ways to its vertexes along its cross product
void FinishTriangleEdgeTests(f32* values)
{
    for (u32 edge = 0; edge < 3; edge++)
    {
        values[edge * 2] = Dot(g_EdgeTests.ways[edge][0], g_EdgeTests.crosses[edge]);
        values[edge * 2 + 1] = Dot(g_EdgeTests.ways[edge][1], g_EdgeTests.crosses[edge]);
    }
}

// Counts of 0 run on until the count wraps around, as on the PS2
void MakeVertexDifferences(const VertexDifferences* request)
{
    Vector4* moved = request->moved;
    const Vector4* vertex = request->vertices;
    u32 left = request->count;
    do
    {
        *moved = Through(request->matrix, vertex, vertex->w);
        moved++;
        vertex++;
        left--;
    } while (left != 0);

    Vector4* otherMoved = moved;
    vertex = request->otherVertices;
    left = request->otherCount;
    do
    {
        *moved = Through(request->otherMatrix, vertex, vertex->w);
        moved++;
        vertex++;
        left--;
    } while (left != 0);

    // Their w the first matrix's third row's (VU0's register kept it)
    f32 w = request->matrix->m[2][3];
    Vector4* difference = request->differences;
    const Vector4* first = request->moved;
    left = request->count;
    do
    {
        const Vector4* other = otherMoved;
        u32 others = request->otherCount;
        do
        {
            *difference = {first->x - other->x, first->y - other->y, first->z - other->z, w};
            difference++;
            other++;
            others--;
        } while (others != 0);

        first++;
        left--;
    } while (left != 0);
}

// The points made groups of four (each coordinate's four together), in place
void GroupPoints(Vector4* points, s32 groups)
{
    const Vector4* from = points;
    Vector4* to = points;
    s32 left = groups;
    do
    {
        Vector4 group[4] = {from[0], from[1], from[2], from[3]};
        Vector4 first = {group[0].x, group[1].x, group[2].x, group[3].x};
        Vector4 second = {group[0].y, group[1].y, group[2].y, group[3].y};
        Vector4 third = {group[0].z, group[1].z, group[2].z, group[3].z};
        to[0] = first;
        to[1] = second;
        to[2] = third;
        from += 4;
        to += 3;
        left--;
    } while (left != 0);
}

// The range along an axis of so many groups
void GroupsRange(const Vector4* groups, s32 count, const Vector4* axis, f32* range)
{
    Vector4 least = {RangeStart, RangeStart, RangeStart, RangeStart};
    Vector4 most = {-RangeStart, -RangeStart, -RangeStart, -RangeStart};
    const Vector4* group = groups;
    s32 left = count;
    do
    {
        TakeIntoRange(&least, &most, GroupAlong(group, axis));
        group += 3;
        left--;
    } while (left != 0);

    StoreRange(least, most, range);
}

// The range along an axis of 16 groups (64 points)
void SixteenGroupsRange(const Vector4* groups, const Vector4* axis, f32* range)
{
    constexpr u32 GroupCount = 16;
    Vector4 least = {RangeStart, RangeStart, RangeStart, RangeStart};
    Vector4 most = {-RangeStart, -RangeStart, -RangeStart, -RangeStart};
    for (u32 group = 0; group < GroupCount; group++)
    {
        TakeIntoRange(&least, &most, GroupAlong(&groups[group * 3], axis));
    }

    StoreRange(least, most, range);
}

// The triangle's vertexes less the box hull's, grouped
void LoadTriangleHullSupport(const CollisionHit* triangle, const Vector4* hullVertices)
{
    for (u32 vertex = 0; vertex < TriangleVertexCount; vertex++)
    {
        for (u32 hullVertex = 0; hullVertex < BoxHullVertexCount; hullVertex++)
        {
            g_SupportDifferences[vertex][hullVertex] = Difference(hullVertices[hullVertex], triangle->vertices[vertex]);
        }
    }
}

// The range along an axis of the triangle less the hull (its min and max)
void TriangleHullSupportRange(const Vector4* axis, f32* range)
{
    f32 least = Dot(g_SupportDifferences[0][0], *axis);
    f32 most = least;
    for (const auto& vertex : g_SupportDifferences)
    {
        for (const Vector4& difference : vertex)
        {
            f32 along = Dot(difference, *axis);
            least = Platform::Math::Min(least, along);
            most = Platform::Math::Max(most, along);
        }
    }

    range[0] = least;
    range[1] = most;
}
}
