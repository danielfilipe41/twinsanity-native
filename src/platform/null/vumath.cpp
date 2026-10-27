// VU0 macro-mode maths on the CPU: the CPU versions of the functions
// src/platform/ps2/matrices.cpp and src/platform/ps2/bounds.cpp implement with VU0
// macro instructions. The null platform only needs the results.
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
    // is read first: the PS2's sequence works in registers, and in place is a caller.
    // The translation is negated before it is turned (the microprogram's vmulx by the -1
    // it makes of vf0w), then the rows' shares are taken in turn, because VU0 rounds each
    // of those steps
    Vector4 position = *RowOf(matrix, PositionRow);
    const f32* place = reinterpret_cast<const f32*>(&position);
    f32 moveX = place[0] * -1.0f;
    f32 moveY = place[1] * -1.0f;
    f32 moveZ = place[2] * -1.0f;
    Matrix4x4 inverse;
    for (s32 row = 0; row < 3; row++)
    {
        for (s32 column = 0; column < 3; column++)
        {
            inverse.m[row][column] = matrix->m[column][row];
        }

        inverse.m[row][3] = 0.0f;
    }

    for (s32 column = 0; column < 3; column++)
    {
        inverse.m[PositionRow][column] =
            inverse.m[0][column] * moveX + inverse.m[1][column] * moveY + inverse.m[2][column] * moveZ;
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
    // the ws less the dot product. The sums are taken in the microprogram's order - the
    // cross product's first terms, then each scaled vector, then the second cross term -
    // and the w's dot taken off one axis at a time, because VU0 rounds each step
    f32 x = a->y * b->z + a->x * b->w + b->x * a->w - b->y * a->z;
    f32 y = a->z * b->x + a->y * b->w + b->y * a->w - b->z * a->x;
    f32 z = a->x * b->y + a->z * b->w + b->z * a->w - b->x * a->y;
    f32 w = a->w * b->w - a->z * b->z - a->y * b->y - a->x * b->x;
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
    // and z, the fourth row whole; every lane summed in the microprogram's order - the
    // rows by x, y and z then the fourth - because VU0 rounds each step
    const auto& rows = matrix->m;
    f32 x = point->x * rows[0][0] + point->y * rows[1][0] + point->z * rows[2][0] + rows[PositionRow][0];
    f32 y = point->x * rows[0][1] + point->y * rows[1][1] + point->z * rows[2][1] + rows[PositionRow][1];
    f32 z = point->x * rows[0][2] + point->y * rows[1][2] + point->z * rows[2][2] + rows[PositionRow][2];
    f32 w = point->x * rows[0][3] + point->y * rows[1][3] + point->z * rows[2][3] + rows[PositionRow][3];
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
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
    // of its parts come out doubled: the rows' w 0, the position row left as it was.
    // The doubled products are the scaled factors' own products (not 2 * x * y), and the
    // diagonal's terms are taken off one at a time, because VU0 rounds each of those steps
    constexpr f32 SquareRootOf2 = 0x1.6A09E6p+0f;
    f32 x = rotation->x;
    f32 y = rotation->y;
    f32 z = rotation->z;
    f32 doubledX = x * x + x * x;
    f32 doubledY = y * y + y * y;
    f32 doubledZ = z * z + z * z;
    f32 scaledX = x * SquareRootOf2;
    f32 scaledY = y * SquareRootOf2;
    f32 scaledZ = z * SquareRootOf2;
    f32 scaledW = rotation->w * SquareRootOf2;
    f32 xy = scaledX * scaledY;
    f32 yz = scaledY * scaledZ;
    f32 zx = scaledZ * scaledX;
    f32 xw = scaledX * scaledW;
    f32 yw = scaledY * scaledW;
    f32 zw = scaledZ * scaledW;
    f32 lessX = 1.0f - doubledX;
    matrix->m[0][0] = 1.0f - doubledY - doubledZ;
    matrix->m[0][1] = xy + zw;
    matrix->m[0][2] = zx - yw;
    matrix->m[0][3] = 0.0f;
    matrix->m[1][0] = xy - zw;
    matrix->m[1][1] = lessX - doubledZ;
    matrix->m[1][2] = yz + xw;
    matrix->m[1][3] = 0.0f;
    matrix->m[2][0] = zx + yw;
    matrix->m[2][1] = yz - xw;
    matrix->m[2][2] = lessX - doubledY;
    matrix->m[2][3] = 0.0f;
}

u32 PlaneThroughTriangle(Vector4* plane, const Vector4* first, const Vector4* second, const Vector4* third)
{
    // The cross product of the edges from the first vertex made a unit vector, its w
    // less how far along it the first vertex is. The w is nought less the dot product, as
    // the microprogram's vsubx.w takes it (not the dot's sign flipped), and the dot's terms
    // are summed in the microprogram's order, because VU0 rounds each step
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
    f32 scale = 1.0f / length;
    normalX = normalX * scale;
    normalY = normalY * scale;
    normalZ = normalZ * scale;
    plane->x = normalX;
    plane->y = normalY;
    plane->z = normalZ;
    plane->w = 0.0f - (normalX * first->x + normalY * first->y + normalZ * first->z);
    return Epsilon < length;
}

void GetBbox(const Vector4* points, s32 count, Box* box, const Matrix4x4* matrix)
{
    // Each point moved by the matrix (its own w taking the fourth row's share, as in
    // VuTransformByRows), the smallest and the largest x, y and z kept; the corners'
    // w the first point's moved w. Each pair taken as the microprogram's vmini and vmax
    // take them (the second operand's way with a tie)
    Vector4 moved;
    VuTransformByRows(matrix, &points[0], &moved);
    Vector4 min = moved;
    Vector4 max = moved;
    for (s32 index = 1; index < count; index++)
    {
        VuTransformByRows(matrix, &points[index], &moved);
        min.x = Platform::Math::Min(min.x, moved.x);
        max.x = Platform::Math::Max(max.x, moved.x);
        min.y = Platform::Math::Min(min.y, moved.y);
        max.y = Platform::Math::Max(max.y, moved.y);
        min.z = Platform::Math::Min(min.z, moved.z);
        max.z = Platform::Math::Max(max.z, moved.z);
    }

    box->min = min;
    box->max = max;
}

void PointsBox(const Vector4* points, s32 count, Box* box)
{
    // The points' box: the first point in both corners, the rest taken in as the
    // microprogram's vmini and vmax take them
    Vector4 min = points[0];
    Vector4 max = points[0];
    for (s32 index = 1; index < count; index++)
    {
        min.x = Platform::Math::Min(min.x, points[index].x);
        max.x = Platform::Math::Max(max.x, points[index].x);
        min.y = Platform::Math::Min(min.y, points[index].y);
        max.y = Platform::Math::Max(max.y, points[index].y);
        min.z = Platform::Math::Min(min.z, points[index].z);
        max.z = Platform::Math::Max(max.z, points[index].z);
    }

    box->min = min;
    box->max = max;
}

void TriangleBounds(Box* box, const Vector4* first, const Vector4* second, const Vector4* third)
{
    // The corners' least and largest x, y and z, the first two taken in then the third,
    // as the microprogram's vmini and vmax take them. The PS2 leaves their w what its
    // registers had; the first corner's w stands in for it deterministically
    box->min.x = Platform::Math::Min(Platform::Math::Min(first->x, second->x), third->x);
    box->min.y = Platform::Math::Min(Platform::Math::Min(first->y, second->y), third->y);
    box->min.z = Platform::Math::Min(Platform::Math::Min(first->z, second->z), third->z);
    box->min.w = first->w;
    box->max.x = Platform::Math::Max(Platform::Math::Max(first->x, second->x), third->x);
    box->max.y = Platform::Math::Max(Platform::Math::Max(first->y, second->y), third->y);
    box->max.z = Platform::Math::Max(Platform::Math::Max(first->z, second->z), third->z);
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
