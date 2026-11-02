#include "game/interpolation.h"

#include "game/instances.h"
#include "game/place.h"

namespace
{
f32 g_Alpha = 0.0f;
bool g_Redraw = false;
u32 g_Step = 0;

// The step the camera's samples were taken in (a step's own frame samples once: the view may be updated more than once)
u32 g_CameraStep = 0xFFFFFFFF;
const void* g_CameraObject = nullptr;
Matrix4x4 g_CameraPrevious;
Matrix4x4 g_CameraCurrent;
bool g_HasPrevious = false;
bool g_HasCurrent = false;

// The largest move a step eases: past it the thing jumped (a teleport, a camera cut, a respawn), and the picture shows the
// stepped state instead of sweeping through the world between the two places
constexpr f32 MaxStepMove = 4.0f;
// The furthest its forward axis may turn in a step and still be eased (the cosine of the angle): a wider turn is a cut
constexpr f32 MinForwardDot = 0.5f;

bool TooFar(const Matrix4x4* previous, const Matrix4x4* current)
{
    const Vector4* before = RowOf(previous, 3);
    const Vector4* now = RowOf(current, 3);
    f32 x = now->x - before->x;
    f32 y = now->y - before->y;
    f32 z = now->z - before->z;
    if (MaxStepMove * MaxStepMove < x * x + y * y + z * z)
    {
        return true;
    }

    const Vector4* beforeForward = RowOf(previous, 2);
    const Vector4* nowForward = RowOf(current, 2);
    return nowForward->x * beforeForward->x + nowForward->y * beforeForward->y + nowForward->z * beforeForward->z <
           MinForwardDot;
}

void Ease(Matrix4x4* out, const Matrix4x4* previous, const Matrix4x4* current, f32 alpha)
{
    for (u32 row = 0; row < 4; row++)
    {
        for (u32 column = 0; column < 4; column++)
        {
            f32 before = previous->m[row][column];
            out->m[row][column] = before + (current->m[row][column] - before) * alpha;
        }
    }
}
}

void HostFrame::SetAlpha(f32 alpha)
{
    if (alpha < 0.0f)
    {
        alpha = 0.0f;
    }
    else if (1.0f < alpha)
    {
        alpha = 1.0f;
    }

    g_Alpha = alpha;
}

void HostFrame::BeginLogicStep()
{
    g_Step++;
}

void HostFrame::SetRedraw(bool redraw)
{
    g_Redraw = redraw;
}

bool HostFrame::IsRedraw()
{
    return g_Redraw;
}

void HostFrame::SampleCamera(const void* camera, const Matrix4x4* matrix)
{
    if (camera != g_CameraObject)
    {
        g_CameraObject = camera;
        g_HasPrevious = false;
        g_HasCurrent = false;
        g_CameraStep = 0xFFFFFFFF;
    }

    if (g_CameraStep == g_Step)
    {
        // The step's own frame again: the camera stands where it does now, its previous sample unchanged
        g_CameraCurrent = *matrix;
        g_HasCurrent = true;
        return;
    }

    if (g_HasCurrent)
    {
        g_CameraPrevious = g_CameraCurrent;
        g_HasPrevious = true;
    }

    g_CameraCurrent = *matrix;
    g_HasCurrent = true;
    g_CameraStep = g_Step;
}

bool HostFrame::InterpolateCameraMatrix(const void* camera, const Matrix4x4* current, Matrix4x4* out)
{
    // At the interval's start (alpha 0) this is the previous step's camera: the picture eases from there to the state just
    // stepped, reaching it as the next step begins. That costs the picture one step (20 ms) and is what makes the display
    // frames between the steps continuous instead of ahead of the state they can know
    if (camera != g_CameraObject || !g_HasPrevious || !g_HasCurrent)
    {
        return false;
    }

    if (TooFar(&g_CameraPrevious, current))
    {
        return false;
    }

    Ease(out, &g_CameraPrevious, current, g_Alpha);
    return true;
}

bool HostFrame::InterpolateInstanceMatrix(const InstanceContext* instance, Matrix4x4* out)
{
    auto* movement = static_cast<MovementNode*>(GetGameNode(const_cast<NodeList*>(&instance->nodes), NodeMovement));
    if (movement == nullptr || movement->bits.captured == 0)
    {
        return false;
    }

    const Matrix4x4* current = &instance->place->matrix;
    if (TooFar(&movement->previousMatrix, current))
    {
        return false;
    }

    Ease(out, &movement->previousMatrix, current, g_Alpha);
    return true;
}
