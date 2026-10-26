// The camera probe (TS_CAMLOG): one line a frame with the camera's own numbers, written from the
// frame start. Kept in its own file so the draw code and the headless draw tests do not pull in
// the game's camera and node code.
#include "draws_host.h"
#include "host_graphics.h"

#include "game/camerarig.h"
#include "game/followcamera.h"
#include "game/instances.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/view.h"

#include <cstdio>
#include <cstdlib>


// The camera's own numbers, once a frame, when TS_CAMLOG names a file (the probe itself, near the end so it can
// read the same shader tables the draws do)
namespace
{
FILE* g_file = nullptr;
bool g_read = false;
u32 g_lastFrame = 0xFFFFFFFF;
u32 g_lines = 0;

// The camera's own place (renderer.h's CameraPlace), the null page the PS2 reads at address 8 stood in for by a null
// place: a frame is only logged with a camera
ObjectPlace* PlaceOf(const RenderView* view)
{
    Reference* reference = view->cameraObject;
    ReferencedObject* camera = reference != nullptr ? reference->object : nullptr;
    return camera != nullptr ? camera->place : nullptr;
}
}

void Platform::Graphics::HostCameraLog(const RenderView* view)
{
    if (!g_read)
    {
        g_read = true;
        const char* spec = std::getenv("TS_CAMLOG");
        if (spec != nullptr && spec[0] != '\0')
        {
            g_file = std::fopen(spec, "w");
            if (g_file != nullptr)
            {
                std::setvbuf(g_file, nullptr, _IONBF, 0);
            }
        }
    }

    if (g_file == nullptr || view == nullptr || g_lines >= 20000)
    {
        return;
    }

    if (g_lastFrame == Platform::Graphics::HostFrameCounter())
    {
        return;
    }

    g_lastFrame = Platform::Graphics::HostFrameCounter();
    g_lines++;

    ObjectPlace* place = PlaceOf(view);
    if (place != nullptr)
    {
        place->SyncPosition();
    }

    const Vector4* at = place != nullptr ? &place->position : nullptr;
    // Which camera is looking, and which place it looks from: a changed pair is the camera being switched, so a jump
    // in the numbers after it is the switch rather than a fault
    const void* cameraObject = view->cameraObject;
    const void* cameraPlace = place;
    s32 lensFov = 0;
    u32 lensChanged = 0xFFFFFFFF;
    u32 hasRig = 0;
    u32 isFollow = 0;
    f32 distance = 0.0f;
    f32 distanceLow = 0.0f;
    f32 distanceHigh = 0.0f;
    f32 distanceGoal = 0.0f;
    f32 distanceShare = 0.0f;
    u64 followBits = 0;
    u32 blockedSince = 0xFFFFFFFF;
    const Vector4* aimsAt = nullptr;
    u32 aimsAtValid = 0;
    FollowCameraPositioner* followPositioner = nullptr;
    f32 pitchNow = 0.0f;
    f32 pitchShare = 0.0f;
    s32 pitchLow = 0;
    s32 pitchHigh = 0;
    s32 pitchGoal = 0;
    const void* triggerNode = nullptr;
    f32 placeX = 0.0f;
    f32 placeY = 0.0f;
    f32 placeZ = 0.0f;
    u32 cutState = 0xFFFFFFFF;
    u32 subtypeType = 0;
    u32 hasSecond = 0;
    if (view->cameraObject != nullptr && view->cameraObject->object != nullptr)
    {
        auto* instance = static_cast<InstanceContext*>(view->cameraObject->object);
        auto* lens = static_cast<CameraLensNode*>(GetGameNode(&instance->nodes, NodeCameraLens));
        if (lens != nullptr)
        {
            lensFov = lens->fov;
            lensChanged = lens->bits.projectionChanged;
            CameraRig* rig = lens->rig;
            if (rig != nullptr && rig->positioner != nullptr)
            {
                hasRig = 1;
                // Only a follow rig's positioner is a FollowCameraPositioner: the vtable says which, so a scripted
                // or keyed camera's bytes are never read as one
                if (rig->positioner->vtable == g_FollowCameraPositionerVTable)
                {
                    auto* follow = static_cast<FollowCameraPositioner*>(rig->positioner);
                    isFollow = 1;
                    followPositioner = follow;
                    distance = follow->distance.current;
                    distanceLow = follow->distance.low;
                    distanceHigh = follow->distance.high;
                    distanceGoal = follow->distance.initial;
                    distanceShare = follow->distance.share;
                    followBits = follow->bits.value;
                    blockedSince = follow->blockedSince;
                    triggerNode = follow->trigger;
                    placeX = follow->triggerPlace.x;
                    placeY = follow->triggerPlace.y;
                    placeZ = follow->triggerPlace.z;
                    cutState = follow->state.cut;
                    // The camera the trigger names, and the subtype that authors the place: the spline's own parameter is
                    // smooth and it is evaluated a handful of times a run, so if the place lurches every few frames it is
                    // another subtype writing it - or none writing it, and a stale place from the last camera kept
                    if (follow->trigger != nullptr && follow->trigger->camera != nullptr)
                    {
                        CameraSubtype* second = follow->trigger->camera->second;
                        if (second != nullptr)
                        {
                            hasSecond = 1;
                            subtypeType = CallVirtual<u32>(second, second->vtable, CameraSubtype::TypeSlot);
                        }
                    }
                    pitchNow = follow->pitch.current;
                    pitchShare = follow->pitch.share;
                    pitchLow = follow->pitch.low;
                    pitchHigh = follow->pitch.high;
                    pitchGoal = follow->pitch.initial;
                    if (follow->target != nullptr)
                    {
                        aimsAt = &follow->target->point;
                        aimsAtValid = 1;
                    }
                }
            }
        }
    }

    // How far the camera really is from what it aims at, against what the blender says it should be: the two disagree
    // if the place the camera is drawn from is not the one the positioner moved
    f32 aimed = -1.0f;
    if (at != nullptr && aimsAt != nullptr)
    {
        const f32 dx = at->x - aimsAt->x;
        const f32 dy = at->y - aimsAt->y;
        const f32 dz = at->z - aimsAt->z;
        aimed = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    }

    // The view check run regardless of the skip bit, which is up in almost every frame: whether the collision calls the
    // camera's own segment blocked, and whether the camera would find a clear place out of it. A "blocked" 1 here where
    // nothing stands in the way is the collision lying to the camera; a 0 where a wall plainly is means the segment
    // being tested is not the one the camera is on
    u32 forcedBlocked = 0xFFFFFFFF;
    u32 clearFound = 0xFFFFFFFF;
    f32 clearAt[3] = {0.0f, 0.0f, 0.0f};
    if (isFollow != 0 && at != nullptr)
    {
        auto* follow = followPositioner;
        Vector4 eye = follow->target != nullptr ? follow->target->objectPosition : *at;
        eye.y = eye.y + 1.0f; // followcamera.cpp's EyeHeight
        forcedBlocked = follow->ViewBlocked(&follow->position, &eye);
        if (forcedBlocked != 0)
        {
            Vector4 clear = {};
            if (follow->FindClearPlace(&follow->position, &eye, &clear, 0) != 0)
            {
                clearFound = 1;
                clearAt[0] = clear.x;
                clearAt[1] = clear.y;
                clearAt[2] = clear.z;
            }
            else
            {
                clearFound = 0;
            }
        }
    }

    std::fprintf(g_file,
                 "CAM %u cam %p place %p at %.3f %.3f %.3f inv %.4f cot %.4f xs %.4f lensfov %d changed %u "
                 "rig %u follow %u dist %.3f [%.2f..%.2f] goal %.3f share %.3f bits %llx blockedSince %u "
                 "aims %u aimed %.3f target %.3f %.3f %.3f blockedNow %u clear %u at %.3f %.3f %.3f "
                 "pitch %.3f pshare %.3f pends %d %d pgoal %d trigger %p place %.3f %.3f %.3f cut %u "
                 "second %u type 0x%X\n",
                 g_lines, cameraObject, cameraPlace, at != nullptr ? at->x : 0.0f, at != nullptr ? at->y : 0.0f,
                 at != nullptr ? at->z : 0.0f, view->inverseAspect, view->projection.m[1][1],
                 view->projection.m[0][0], lensFov, lensChanged, hasRig, isFollow, distance, distanceLow,
                 distanceHigh, distanceGoal, distanceShare, static_cast<unsigned long long>(followBits),
                 blockedSince, aimsAtValid, aimed, aimsAt != nullptr ? aimsAt->x : 0.0f,
                 aimsAt != nullptr ? aimsAt->y : 0.0f, aimsAt != nullptr ? aimsAt->z : 0.0f, forcedBlocked,
                 clearFound, clearAt[0], clearAt[1], clearAt[2], pitchNow, pitchShare, pitchLow, pitchHigh,
                 pitchGoal, triggerNode, placeX, placeY, placeZ, cutState, hasSecond, subtypeType);
}
