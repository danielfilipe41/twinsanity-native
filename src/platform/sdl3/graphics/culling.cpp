// Culling: frustum and chunk-box tests, the maths the PS2 runs on VU0 (src/platform/ps2/renderer/culling.cpp,
// microprograms 0x1C0, 0x2D0, 0x398 and 0x500).
//
// The game reads back whether something is wholly out of the view, whether it is partly out, and its squared
// distance from the camera. CPU culling does not match VU0 precision, so the box tests are deliberately
// conservative: nothing is reported wholly out and every box is reported crossing, leaving clipping and depth to
// bgfx. The distance is still exact, since level-of-detail selection and seen stamps read it.
#include "host_graphics.h"
#include "platform/graphics.h"

#include "game/math.h"
#include "game/particles.h"
#include "game/scenery.h"

namespace
{
// A frustum's planes: the near one, the four sides (right, left, top and bottom) and the far one. The far set's sides are five
// times as far out
constexpr u32 NearPlane = 0;
constexpr u32 FirstSidePlane = 1;
constexpr u32 SidePlaneCount = 4;
constexpr u32 FarPlane = 5;
constexpr u32 FrustumPlaneCount = 6;
constexpr f32 FarSidesScale = 5.0f;
// The programs test four planes at once, a plane a field of VU0's registers: the near and far planes go in the first two
constexpr u32 NearField = 0;
constexpr u32 FarField = 1;
constexpr s32 MostInt = 0x7FFFFFFF;

constexpr Matrix4x4 Identity = {
    {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}};

// Four planes as the programs take them, a plane a field: their normals' absolute values and their normals (a row per axis),
// then their offsets
struct PlaneColumns
{
    Vector4 absoluteNormals[3];
    Vector4 normals[3];
    Vector4 offsets;
};

// A box against four planes, a sign bit a plane: wholly behind it, partly behind it
struct PlaneTest
{
    u32 outside;
    u32 crossing;
};

// The camera's frustum's planes in its space, and the same with the sides five times as far out
Vector4 g_ViewPlanes[FrustumPlaneCount];
Vector4 g_FarViewPlanes[FrustumPlaneCount];
// What the programs find in VU0's memory: the planes in the chunk's space, the view (its matrices to the clip space and to the
// screen, the camera's place) and the link's
PlaneColumns g_SidePlanes;
PlaneColumns g_FarSidePlanes;
PlaneColumns g_NearAndFarPlanes;
Matrix4x4 g_ToClip;
Matrix4x4 g_ToScreen;
Vector4 g_Camera;
Matrix4x4 g_LinkToClip;
Matrix4x4 g_LinkToScreen;
Vector4 g_LinkCamera;
// The views the frame's particles loaded (LoadParticleView: one a chunk, as many as the frame's list holds), as the PS2 holds
// them in VU0's memory: the frustum's planes in the chunk's space and the camera's place in it. Platform::Math's ViewDistance
// and ParticleBlockView (src/platform/null/math.cpp, the CPU side of the VU0 programs 0xCB0, 0xA08 and 0xB90) read them back
// from here
constexpr s32 MostParticleViews = static_cast<s32>(ParticleViews::MostChunks);
struct ParticleViewState
{
    bool loaded;
    Vector4 planes[FrustumPlaneCount];
    Vector4 camera;
    Matrix4x4 toScreen;
};
ParticleViewState g_ParticleViews[MostParticleViews];
// What the last test left in VU0's registers: whether it's out of the view and whether partly (vi01, vi02), the squared distance
// from the camera (vf15's x) and the matrices to the clip space and to the screen (vf07-vf10, vf11-vf14)
u32 g_Outside;
u32 g_Clipped;
s32 g_Distance;
Matrix4x4 g_Clip;
Matrix4x4 g_Screen;

f32& FieldOf(Vector4& vector, u32 field)
{
    return (&vector.x)[field];
}

Vector4 Scaled(const Vector4& vector, f32 scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale, vector.w * scale};
}

Vector4 Sum(const Vector4& a, const Vector4& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

Vector4 Difference(const Vector4& a, const Vector4& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

// A point through a matrix's four rows (its w weighing the fourth), summed as VU0's accumulator does
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

// Three rows weighed by a vector's x, y and z, added up as VU0's accumulator does: (x · first + y · second) + z · third
Vector4 Weighed(const Vector4* rows, const Vector4& by)
{
    return Sum(Sum(Scaled(rows[0], by.x), Scaled(rows[1], by.y)), Scaled(rows[2], by.z));
}

// A row through a matrix (its w weighing the fourth row), and every row of a matrix through another
Vector4 RowThrough(const Vector4& row, const Matrix4x4& matrix)
{
    return Sum(Weighed(RowOf(&matrix, 0), row), Scaled(*RowOf(&matrix, 3), row.w));
}

Matrix4x4 Product(const Matrix4x4& a, const Matrix4x4& b)
{
    Matrix4x4 product;
    for (u32 row = 0; row < 4; row++)
    {
        *RowOf(&product, row) = RowThrough(*RowOf(&a, row), b);
    }

    return product;
}

// A point's clip judgment: which of its x, y and z are past its |w| either way, the six bits the CLIP instruction leaves
u32 Judge(const Vector4& point)
{
    f32 w = __builtin_fabsf(point.w);
    u32 bits = 0;
    bits |= point.x > w ? 1u << 0 : 0u;
    bits |= point.x < -w ? 1u << 1 : 0u;
    bits |= point.y > w ? 1u << 2 : 0u;
    bits |= point.y < -w ? 1u << 3 : 0u;
    bits |= point.z > w ? 1u << 4 : 0u;
    bits |= point.z < -w ? 1u << 5 : 0u;
    return bits;
}

// VU0's FTOI0: towards zero, held at an int's ends past them (where VU0 also puts the infinities and NaNs it doesn't have)
s32 FloatToInt(f32 value)
{
    if (__builtin_isnan(value) || !(__builtin_fabsf(value) < 0x1p31f))
    {
        return __builtin_signbitf(value) != 0 ? -MostInt - 1 : MostInt;
    }

    return static_cast<s32>(value);
}

// A plane put in a field of the columns
void SetColumn(PlaneColumns* columns, const Vector4& plane, u32 field)
{
    const f32* normal = &plane.x;
    for (u32 axis = 0; axis < 3; axis++)
    {
        FieldOf(columns->absoluteNormals[axis], field) = __builtin_fabsf(normal[axis]);
        FieldOf(columns->normals[axis], field) = normal[axis];
    }

    FieldOf(columns->offsets, field) = plane.w;
}

// A frustum's side planes (its second to fifth) as columns
PlaneColumns SideColumns(const Vector4* planes)
{
    PlaneColumns columns;
    for (u32 side = 0; side < SidePlaneCount; side++)
    {
        SetColumn(&columns, planes[FirstSidePlane + side], side);
    }

    return columns;
}

// A frustum's planes taken into a space (the matrix's)
void PlanesIn(const Vector4* frustum, const Matrix4x4* matrix, Vector4* planes)
{
    for (u32 plane = 0; plane < FrustumPlaneCount; plane++)
    {
        planes[plane] = frustum[plane];
    }

    TransformPlanes(planes, matrix);
}

// The frustum of a lens: the near rectangle's half sizes are the near distance times the tangent of half the field of view (and
// the aspect across), times the scale; the planes go through the eye and the near rectangle's corners, the far plane is the near
// one moved the depth along it and turned around
void LensFrustum(f32 near, f32 far, f32 aspect, f32 scale, s32 fieldOfView, Vector4* planes)
{
    s32 half = static_cast<s32>(static_cast<f32>(fieldOfView) * 0.5f);
    f32 height = near * TanOfAngle(&half);
    f32 width = height * aspect * scale;
    height = height * scale;
    Vector4 topRight = {width, height, near, 1.0f};
    Vector4 topLeft = {-width, height, near, 1.0f};
    Vector4 bottomRight = {width, -height, near, 1.0f};
    Vector4 bottomLeft = {-width, -height, near, 1.0f};
    Vector4 eye = {0.0f, 0.0f, 0.0f, 1.0f};
    PlaneFromTriangle(&planes[NearPlane], &topRight, &bottomLeft, &bottomRight);
    Vector4 along = {0.0f, 0.0f, far - near, 1.0f};
    PlaneAlongNormal(&planes[NearPlane], &along, &planes[FarPlane]);
    planes[FarPlane].x = -planes[FarPlane].x;
    planes[FarPlane].y = -planes[FarPlane].y;
    planes[FarPlane].z = -planes[FarPlane].z;
    planes[FarPlane].w = -planes[FarPlane].w;
    PlaneFromTriangle(&planes[FirstSidePlane], &eye, &bottomRight, &topRight);
    PlaneFromTriangle(&planes[FirstSidePlane + 1], &eye, &topLeft, &bottomLeft);
    PlaneFromTriangle(&planes[FirstSidePlane + 2], &eye, &topRight, &topLeft);
    PlaneFromTriangle(&planes[FirstSidePlane + 3], &eye, &bottomLeft, &bottomRight);
}

// A box's middle and half its size from its corners
void BoxMiddle(const Vector4& min, const Vector4& max, Vector4* middle, Vector4* half)
{
    *half = Scaled(Difference(max, min), 0.5f);
    *middle = Sum(Scaled(min, 0.5f), Scaled(max, 0.5f));
}

// The squared distance between the camera (in the link's space) and a model's place: the level of detail's distance, an int
s32 CameraDistance(const Matrix4x4* model)
{
    Vector4 apart = Difference(g_LinkCamera, *RowOf(model, 3));
    return FloatToInt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
}
}

namespace Platform::Graphics
{
void SetViewFrustum(f32 near, f32 far, f32 aspect, const s32* fieldOfView)
{
    // The game passes near = 0.1; culling with 0.05 is more conservative, so nothing near the camera is dropped by a
    // slightly wrong near plane. The depth test and bgfx's clipping decide what is seen anyway.
    constexpr f32 CullNear = 0.05f;
    (void)near;
    LensFrustum(CullNear, far, aspect, 1.0f, *fieldOfView, g_ViewPlanes);
    LensFrustum(CullNear, far, aspect, FarSidesScale, *fieldOfView, g_FarViewPlanes);
}

const Vector4* ViewPlanes()
{
    return g_ViewPlanes;
}

// The view a chunk's particles loaded (LoadParticleView hands the chunk's matrix in): the frustum's planes taken into the
// chunk's space and the camera's place there (the matrix's fourth row, the chunk's space being the one the particles' places
// are in). The index'th view, as the PS2 sends them to VU0's memory as the index'th packet
void HostStoreParticleView(s32 view, const Matrix4x4* matrices)
{
    if (matrices == nullptr || view < 0 || view >= MostParticleViews)
    {
        return;
    }

    ParticleViewState& state = g_ParticleViews[view];
    for (u32 plane = 0; plane < FrustumPlaneCount; plane++)
    {
        state.planes[plane] = g_ViewPlanes[plane];
    }

    TransformPlanes(state.planes, &matrices[0]);
    state.camera = *RowOf(&matrices[0], 3);
    // The second matrix, which retail's UploadParticleView uploads too: the chunk's toScreen, the block's billboard
    // matrix is taken through it so the particles come out in the game's screen coordinates the draw reads
    state.toScreen = matrices[1];
    state.loaded = true;
}

// A loaded view's planes (six, the frustum's) and its camera's place, null for an id nothing was loaded for this frame
const Vector4* HostParticleViewPlanes(s32 view)
{
    if (view < 0 || view >= MostParticleViews || !g_ParticleViews[view].loaded)
    {
        return nullptr;
    }

    return g_ParticleViews[view].planes;
}

const Vector4* HostParticleViewCamera(s32 view)
{
    if (view < 0 || view >= MostParticleViews || !g_ParticleViews[view].loaded)
    {
        return nullptr;
    }

    return &g_ParticleViews[view].camera;
}

// The view's second matrix (the chunk's toScreen), null for an id nothing was loaded for
const Matrix4x4* HostParticleViewMatrix(s32 view)
{
    if (view < 0 || view >= MostParticleViews || !g_ParticleViews[view].loaded)
    {
        return nullptr;
    }

    return &g_ParticleViews[view].toScreen;
}

// The view's matrix to the screen (the frame's view): the particle billboards take their size from it
const Matrix4x4& HostParticleToScreen()
{
    return g_ToScreen;
}

// The projection's aspect: the lens's pixel aspect times the TV's, the one the projection's x is compressed by
f32 HostParticleInverseAspect()
{
    return g_ToClip.m[0][0] != 0.0f ? 1.0f / g_ToClip.m[0][0] : 1.0f;
}

void LoadChunkPlanes(const Matrix4x4* place)
{
    Vector4 planes[FrustumPlaneCount];
    PlanesIn(g_ViewPlanes, place, planes);
    g_SidePlanes = SideColumns(planes);
    g_NearAndFarPlanes = {};
    SetColumn(&g_NearAndFarPlanes, planes[NearPlane], NearField);
    SetColumn(&g_NearAndFarPlanes, planes[FarPlane], FarField);
    PlanesIn(g_FarViewPlanes, place, planes);
    g_FarSidePlanes = SideColumns(planes);
}

// The portal's side planes are in the chunk's space already, and there's no far plane
void LoadPortalPlanes(const Matrix4x4* place, const Vector4* portal)
{
    g_SidePlanes = SideColumns(portal);
    Vector4 planes[FrustumPlaneCount];
    PlanesIn(g_FarViewPlanes, place, planes);
    g_FarSidePlanes = SideColumns(planes);
    Vector4 nearPlane;
    TransformPlaneOf(g_ViewPlanes, NearPlane, place, &nearPlane);
    g_NearAndFarPlanes = {};
    SetColumn(&g_NearAndFarPlanes, nearPlane, NearField);
}

// The link's matrices the identity
void LoadCullingView(const Matrix4x4* toClip, const Matrix4x4* toScreen, const Vector4* camera)
{
    g_ToClip = *toClip;
    g_ToScreen = *toScreen;
    g_Camera = *camera;
    CullLoadLink(&Identity, &Identity);
}

// 0x398: the camera's place through the object's matrix, ((its fourth row + x · the first) + y · the second) + z · the third, and
// the view's matrices through the chunk's
void CullLoadLink(const Matrix4x4* chunk, const Matrix4x4* object)
{
    Vector4 camera = Sum(*RowOf(object, 3), Scaled(*RowOf(object, 0), g_Camera.x));
    camera = Sum(camera, Scaled(*RowOf(object, 1), g_Camera.y));
    g_LinkCamera = Sum(camera, Scaled(*RowOf(object, 2), g_Camera.z));
    g_LinkToClip = Product(*chunk, g_ToClip);
    g_LinkToScreen = Product(*chunk, g_ToScreen);
}

// 0x2D0: the model's matrix to the screen and its distance
void CullLoadModel(const Matrix4x4* model)
{
    g_Screen = Product(*model, g_LinkToScreen);
    g_Distance = CameraDistance(model);
}

// 0x1C0: out of the view when wholly behind a side, the near or the far plane, partly when crossing any
// CPU frustum culling doesn't match the VU0 programs' precision, so it isn't done: nothing is ever reported wholly out, but the
// distance (the level of detail's, an instance's seen stamp) is still worked out from the box's middle
void CullTestBox(const Vector4* corners)
{
    Vector4 middle;
    Vector4 half;
    BoxMiddle(corners[0], corners[1], &middle, &half);
    Vector4 apart = Difference(g_LinkCamera, middle);
    g_Distance = FloatToInt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
    g_Outside = 0;
    g_Clipped = 1;
}

// 0x500: the planes taken into the model's space, with the matrices to the screen and, partly, to the clip space. The distance
// either way; like CullTestBox, nothing is reported wholly out (bgfx's depth test and clipping decide)
void CullTestBoxAt(const Box* box, const Matrix4x4* model)
{
    (void)box;
    g_Distance = CameraDistance(model);
    g_Outside = 0;
    g_Clipped = 1;
    g_Screen = Product(*model, g_LinkToScreen);
    g_Clip = Product(*model, g_LinkToClip);
}

CullOutcome CullResult()
{
    return {g_Outside, g_Clipped, g_Distance};
}

void CullMatrices(Matrix4x4* clipped, Matrix4x4* toScreen)
{
    if (clipped != nullptr)
    {
        *clipped = g_Clip;
    }

    *toScreen = g_Screen;
}

namespace
{
// A box's eight corners clipped through a matrix (the view's, or the view's through a model's), two tests a corner: the
// scaled one takes the matrix's first row as a guard-band scale. Every corner's outside bits are OR'd (the scaled ones in
// the low six, clip flags 0-5), and the bits every corner shares are AND'd (the unscaled ones in bits 6-11). The game's
// ChunkView::BoxVisibility reads flags[1]'s low six as "wholly out" and flags[0]'s low six as "partly out"
void ClipCorners(const Matrix4x4* matrix, const Vector4* scale, const Box* box, u32* flags)
{
    Vector4 corners[8];
    corners[0] = box->min;
    corners[1] = box->max;
    for (u32 axis = 0; axis < 3; axis++)
    {
        corners[2 + axis] = box->min;
        (&corners[2 + axis].x)[axis] = (&box->max.x)[axis];
        corners[5 + axis] = box->max;
        (&corners[5 + axis].x)[axis] = (&box->min.x)[axis];
    }

    u32 any = 0;
    u32 every = 0;
    for (u32 corner = 0; corner < 8; corner++)
    {
        Vector4 clipped = Through(matrix, &corners[corner], 1.0f);
        Vector4 scaled = {clipped.x * scale->x, clipped.y * scale->y, clipped.z * scale->z, clipped.w * scale->w};
        u32 judgment = Judge(clipped) | (Judge(scaled) << 6);
        if (corner == 0)
        {
            any = judgment;
            every = judgment;
        }
        else
        {
            any |= judgment;
            every &= judgment;
        }
    }

    flags[0] = any;
    flags[1] = every;
}
}

void ClipBox(const Matrix4x4* view, const Box* box, u32* flags)
{
    // The view's scale is the quadword after its matrix (the clip vector the game keeps there)
    ClipCorners(view, reinterpret_cast<const Vector4*>(view + 1), box, flags);
}

void ClipBoxAt(const Matrix4x4* view, const Box* box, u32* flags, const Matrix4x4* model)
{
    Matrix4x4 through;
    VuMultiplyMatrices(model, view, &through);
    ClipCorners(&through, reinterpret_cast<const Vector4*>(view + 1), box, flags);
}
}
