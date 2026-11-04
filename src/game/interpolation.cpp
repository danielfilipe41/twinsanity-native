#include "game/interpolation.h"

#include "game/instances.h"
#include "game/place.h"

#include <unordered_map>
#include <vector>

namespace
{
f32 g_Alpha = 0.0f;
bool g_Redraw = false;
u32 g_Step = 0;

// What a step's own draw remembered of an instance, and the matrix the step before it did
struct Remembered
{
    Matrix4x4 previous;
    Matrix4x4 current;
    // The step whose draw wrote current
    u32 step;
    bool hasPrevious;
};
std::unordered_map<const InstanceContext*, Remembered> g_Instances;
// A skeleton's joints as a step's own draw made them, and the pose the step before it did
struct RememberedPose
{
    std::vector<Matrix4x4> previous;
    std::vector<Matrix4x4> current;
    // The model the pose is one of (an animator freed and its memory given to another may stand at the same address)
    const void* model;
    u32 step;
    bool hasPrevious;
};
std::unordered_map<const void*, RememberedPose> g_Poses;
// A remembered instance seen no step for this many is dropped (its memory may be another instance's by now)
constexpr u32 ForgottenSteps = 512;

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
// The furthest its forward axis may turn in a step and still be eased (the cosine of 30 degrees): a wider turn is a cut, or
// a character that turned on the spot. Beyond it the matrices are not eased -- a linear ease of two rotation matrices that
// far apart collapses their axes through the interval (the model squashes and springs back)
constexpr f32 MinForwardDot = 0.8660254f;

bool TooFar(const Matrix4x4* previous, const Matrix4x4* current, f32 minForwardDot = MinForwardDot)
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
           minForwardDot;
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

u32 HostFrame::GetCurrentStep()
{
    return g_Step;
}

void HostFrame::BeginLogicStep()
{
    // The sample a step's own draw wrote is the one the display frames after it ease from; an instance that step didn't
    // draw has nothing to ease from, and one not seen for a long time is dropped (instances are freed and their memory
    // handed to another)
    for (auto it = g_Instances.begin(); it != g_Instances.end();)
    {
        Remembered& entry = it->second;
        if (entry.step == g_Step)
        {
            entry.previous = entry.current;
            entry.hasPrevious = true;
        }
        else
        {
            entry.hasPrevious = false;
        }

        if (g_Step - entry.step > ForgottenSteps)
        {
            it = g_Instances.erase(it);
            continue;
        }

        ++it;
    }

    for (auto it = g_Poses.begin(); it != g_Poses.end();)
    {
        RememberedPose& pose = it->second;
        if (pose.step == g_Step && pose.previous.size() == pose.current.size())
        {
            pose.previous = pose.current;
            pose.hasPrevious = true;
        }
        else
        {
            pose.hasPrevious = false;
        }

        if (g_Step - pose.step > ForgottenSteps)
        {
            it = g_Poses.erase(it);
            continue;
        }

        ++it;
    }

    g_Step++;
}

void HostFrame::RememberInstance(const InstanceContext* instance, const Matrix4x4* matrix)
{
    if (g_Redraw)
    {
        return;
    }

    Remembered& entry = g_Instances[instance];
    entry.current = *matrix;
    entry.step = g_Step;
}

bool HostFrame::InterpolateRememberedMatrix(const InstanceContext* instance, Matrix4x4* out)
{
    auto found = g_Instances.find(instance);
    if (found == g_Instances.end() || !found->second.hasPrevious)
    {
        return false;
    }

    const Matrix4x4* current = &instance->place->matrix;
    if (TooFar(&found->second.previous, current))
    {
        return false;
    }

    Ease(out, &found->second.previous, current, g_Alpha);
    return true;
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

bool HostFrame::EaseJoints(const void* joints, const void* model, const Matrix4x4* current, u32 count, Matrix4x4* out)
{
    if (joints == nullptr || current == nullptr || out == nullptr || count == 0)
    {
        return false;
    }

    RememberedPose& pose = g_Poses[joints];
    if (!g_Redraw)
    {
        // The step's own draw: the pose it made is the one the next step's frames ease from. Another skeleton's pose (the
        // animator's memory given to another model, or an OGI that changed its joint count) is nothing to ease from
        if (pose.current.size() != count || pose.model != model)
        {
            pose.current.resize(count);
            pose.previous.resize(count);
            pose.hasPrevious = false;
            pose.model = model;
        }

        for (u32 joint = 0; joint < count; joint++)
        {
            pose.current[joint] = current[joint];
        }

        pose.step = g_Step;
    }

    if (!pose.hasPrevious || pose.previous.size() != count || pose.model != model)
    {
        return false;
    }

    for (u32 joint = 0; joint < count; joint++)
    {
        // A bone that jumped (an animation restarted, an OGI changed) is not swept through the model: it takes this
        // step's matrix. Bones turn a lot between steps, so only a quarter turn (which an animation cannot make in one
        // step) counts as a jump, and the bone's place
        if (TooFar(&pose.previous[joint], &current[joint], 0.0f))
        {
            out[joint] = current[joint];
        }
        else
        {
            Ease(&out[joint], &pose.previous[joint], &current[joint], g_Alpha);
        }
    }

    return true;
}
