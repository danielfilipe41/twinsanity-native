#include "game/followcamera.h"

#include "game/agents.h"
#include "game/characters.h"
#include "game/chunkdata.h"
#include "game/chunkloading.h"
#include "game/clock.h"
#include "game/controls.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/objectnode.h"
#include "game/place.h"
#include "game/player.h"
#include "game/reference.h"
#include "debug.h"

#include <cstdio>
#include <cstdlib>

#include <cstddef>
#include <cstdint>

// A playable character's own nodes: the controls (kind 0xB, the pad read into the character's controls, or the instance's motion
// driving them while it's held) and the follow camera (kind 0x16, the camera's instance it makes in the character's chunk)

extern "C"
{
    extern const GccVTableEntry g_ControlsNodeVTable[] RETAIL(D_002F4D90);
    extern const GccVTableEntry g_FollowNodeVTable[] RETAIL(UnkNode_0x16_Methods);
    // The game's nodes' base (game/agentnodes.cpp)
    extern const GccVTableEntry g_GameNodeBaseVTable[] RETAIL(D_002F54B8);
    // A camera's instance made in a chunk (placed by a matrix when there's one), with its lens
    InstanceContext* MakeCameraInstance(ChunkData* chunk, const Matrix4x4* matrix) RETAIL(FUN_00259ce0);
}

namespace
{
constexpr u32 ControlsType = 0x9006;
constexpr u32 FollowType = 0x9007;

// The instance's move this frame (its object node's vector at 0xC0)
const Vector4* FrameMove(InstanceContext* instance)
{
    return &static_cast<ObjectNodeBase*>(GetGameNode(&instance->nodes, NodeObject))->frameMove;
}

void DestroyReplacement(ControlsNode* node)
{
    ControlsHandler* replacement = node->replacement;
    if (replacement != nullptr)
    {
        CallVirtual<void>(replacement, replacement->vtable, ControlsHandler::DestroySlot, u32{DestroyAndFree});
    }
}

ReferencedObject* FollowedOf(const FollowNode* node)
{
    return node->cameraInstance != nullptr ? node->cameraInstance->object : nullptr;
}

// What it follows released, and let go of when it's still there
void ReleaseFollowed(FollowNode* node)
{
    ReferencedObject* object = FollowedOf(node);
    if (object == nullptr)
    {
        return;
    }

    CallVirtual<u32>(object, object->vtable, ReferencedObject::ReleaseSlot);
    if (FollowedOf(node) != nullptr)
    {
        RemoveReference(&node->cameraInstance);
        node->cameraInstance = nullptr;
    }
}

// Its camera started for its character again, the camera's instance made in a chunk
void RestartCamera(FollowNode* node, ChunkData* chunk)
{
    InstanceContext* camera = node->MakeCamera(chunk);
    if (g_SoundLog)
    {
        std::fprintf(stderr, "sound: camera restarted in chunk %p -> %p (instance %p)\n", static_cast<void*>(chunk),
                     static_cast<void*>(camera),
                     static_cast<void*>(node->cameraInstance != nullptr ? node->cameraInstance->object : nullptr));
    }

    RestartFollowCamera(&node->camera, g_AgentNodesGameController, camera, node->owner);
    RestoreCameraDefaults(node);
    node->unused760 = 0.0f;
}
}

ControlsNode* ConstructControlsNode(void* memory)
{
    auto* node = static_cast<ControlsNode*>(memory);
    GameNode::Construct(node);
    node->pad = nullptr;
    node->vtable = g_ControlsNodeVTable;
    CharacterControls::Construct(&node->handler);
    node->replacement = nullptr;
    node->bits.motionDriven = 0;
    return node;
}

void ReplaceControlsHandler(ControlsNode* controls, void* handler)
{
    DestroyReplacement(controls);
    controls->replacement = static_cast<ControlsHandler*>(handler);
}

void ControlsNode::Destroy(u32 destroyFlags)
{
    vtable = g_ControlsNodeVTable;
    DestroyReplacement(this);
    handler.Destroy(DestroyOnly);
    vtable = g_GameNodeBaseVTable;
    GameNode::Destroy(destroyFlags);
}

u32 ControlsNode::Kind()
{
    return NodeControls;
}

void ControlsNode::Step(TimeClock*, u32)
{
    DestroyReplacement(this);
    replacement = nullptr;
    bits.motionDriven = 0;
    handler.Reset();
}

// While the clock runs: motion driven, the controls are given the instance's move this frame in units a second; else, with a
// pad, the controls in use read it for the instance, and its follow camera reads it
u32 ControlsNode::Update(TimeClock* clock)
{
    // TS_SPEED=1: whether this node's branch is taken at all, written before either is. The chase's runner is the body
    // whose speed is wrong, and a body driven by its motion takes the first branch and one driven by a pad the second;
    // a body with neither is asked for nothing, and then its buttons keep whatever the last writer left in them. Opened
    // and closed per line, because a file held open from a static lands wherever the working directory was at the moment
    // of the first call and is then invisible from the logs' own directory
    if (std::getenv("TS_SPEED") != nullptr)
    {
        static u32 noted = 0;
        if (noted++ < 20000)
        {
            std::FILE* file = std::fopen("nodectl.log", "ab");
            if (file != nullptr)
            {
                std::fprintf(file, "frame %u inst %p running %u motionDriven %u pad %p\n",
                             static_cast<unsigned>(g_RenderedFrames), static_cast<void*>(owner),
                             static_cast<unsigned>(clock->flags.running), bits.motionDriven,
                             static_cast<void*>(pad));
                std::fclose(file);
            }
        }
    }

    if (clock->flags.running != 0)
    {
        if (bits.motionDriven != 0)
        {
            Vector4 velocity = *FrameMove(owner);
            f32 inverse = 1.0f / (static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit);
            // TS_PLACE=1: this body's place is watched, so every write to its position is logged with its caller
            if (std::getenv("TS_PLACE") != nullptr)
            {
                g_PlaceWriteTrace = true;
                WatchPlaceWrites(owner->place);
            }

            // TS_SPEED=1: a motion driven body's speed is read back off its own movement - the place's delta last frame
            // becomes this frame's velocity, and this frame's movement becomes next frame's. Written for every frame
            // that runs, because the shape is the answer: a ramp adds a little each frame until it saturates at the
            // property and stays there for the rest of the level; a step means the place jumped or the clip changed over
            if (std::getenv("TS_SPEED") != nullptr)
            {
                static u32 noted = 0;
                if (noted++ < 24000)
                {
                    f32 length = __builtin_sqrtf(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
                    auto* agentNode = static_cast<AgentNode*>(GetGameNode(&owner->nodes, NodeCharacter));
                    CharacterAgent* agent = agentNode != nullptr ? static_cast<CharacterAgent*>(agentNode->agent) : nullptr;
                    f32 top = agent != nullptr ? agent->WalkTopSpeed() : 0.0f;
                    // Opened and closed per line: a file held open from a static is created wherever the working directory
                    // was at the first call and is then missing from the logs' own directory, which is how two runs went by
                    // with no bodymove.log at all
                    std::FILE* file = std::fopen("bodymove.log", "ab");
                    if (file != nullptr)
                    {
                        std::fprintf(file,
                                     "frame %u inst %p placeaddr %p kind %d driven %u fm %g %g %g len %g inv %g top %g "
                                     "stick %g place %g %g %g\n",
                                     static_cast<unsigned>(g_RenderedFrames), static_cast<void*>(owner),
                                     static_cast<void*>(owner->place),
                                     agent != nullptr ? agent->properties->GetInt(CharacterKindProperty) : -1,
                                     static_cast<unsigned>(agent != nullptr ? agent->Controlled() : 0),
                                     static_cast<double>(velocity.x), static_cast<double>(velocity.y),
                                     static_cast<double>(velocity.z), static_cast<double>(length),
                                     static_cast<double>(inverse), static_cast<double>(top),
                                     static_cast<double>(length * inverse / (top != 0.0f ? top : 1.0f)),
                                     static_cast<double>(owner->place->position.x),
                                     static_cast<double>(owner->place->position.y),
                                     static_cast<double>(owner->place->position.z));
                        std::fclose(file);
                    }
                }
            }

            velocity.x *= inverse;
            velocity.y *= inverse;
            velocity.z *= inverse;
            handler.MoveBy(clock, &velocity, owner);
        }
        else if (pad != nullptr)
        {
            auto* follow = static_cast<FollowNode*>(GetGameNode(&owner->nodes, NodeFollow));
            ControlsHandler* controls = replacement != nullptr ? replacement : &handler;
            controls->FrameVirtual(clock, pad, owner);
            controls->ReadButtons(pad, owner);
            FollowCameraReadPad(&follow->camera, pad);
        }
    }

    return GameNode::Update(clock);
}

u32 ControlsNode::Type()
{
    return ControlsType;
}

FollowNode* ConstructFollowNode(FollowNode* node, ChunkEntry* chunk)
{
    GameNode::Construct(node);
    node->chunk = chunk;
    node->cameraInstance = nullptr;
    node->vtable = g_FollowNodeVTable;
    ConstructFollowCamera(&node->camera);
    node->bits.value = 0;
    return node;
}

void RestoreCameraDefaults(FollowNode* node)
{
    auto* player = static_cast<PlayerNode*>(GetGameNode(&node->owner->nodes, NodeCharacter));
    RestoreFollowCameraDefaults(&node->camera, player->character);
}

InstanceContext* FollowNode::MakeCamera(ChunkData* chunk)
{
    AssignReference(&cameraInstance, MakeCameraInstance(chunk, nullptr));
    // Its clock the chunk's first (unchecked: without a camera, a byte of low memory at 0x153 is written)
    static_cast<InstanceContext*>(FollowedOf(this))->clockIndex = FirstClock;
    return static_cast<InstanceContext*>(FollowedOf(this));
}

void FollowNode::StopFollowing()
{
    UnregisterNode(owner, 0, this);
    ReferencedObject* object = FollowedOf(this);
    if (object != nullptr)
    {
        CallVirtual<u32>(object, object->vtable, ReferencedObject::SleepSlot);
    }
}

void FollowNode::Destroy(u32 destroyFlags)
{
    vtable = g_FollowNodeVTable;
    ReleaseFollowed(this);
    DestroyFollowCamera(&camera, DestroyOnly);
    RemoveReference(&cameraInstance);
    vtable = g_GameNodeBaseVTable;
    GameNode::Destroy(destroyFlags);
}

void FollowNode::SetOwner(InstanceContext* instance)
{
    ChunkDataReference* data = chunk->data;
    ChunkData* chunkData = data != nullptr ? data->chunk : nullptr;
    GameNode::SetOwner(instance);
    RestartCamera(this, chunkData);
}

// Into a chunk the link leads to, once the linked chunk's RM2 is loaded: its camera's instance moved along (not while it stays):
// whether it moved
u32 FollowNode::CanChangeChunk(ChunkData*, ChunkLinkData* link)
{
    if (link->flags.linkedRm2Loaded == 0)
    {
        return 0;
    }

    // The camera's flags (at address 4 without a camera): one that moves between chunks by itself (ShowCamera clears it) doesn't
    // follow the character into another chunk
    std::uintptr_t followed = reinterpret_cast<std::uintptr_t>(FollowedOf(this));
    if (reinterpret_cast<const ReferencedObjectFlags*>(followed + offsetof(ReferencedObject, flags))->movesBetweenChunks)
    {
        return 0;
    }

    return static_cast<InstanceContext*>(FollowedOf(this))->ChangeChunk(link) != nullptr;
}

u32 FollowNode::Kind()
{
    return NodeFollow;
}

void FollowNode::LeftChunk(u32)
{
    ReleaseFollowed(this);
}

void FollowNode::Step(TimeClock* clock, u32)
{
    ChunkData* chunk = owner->chunk;
    if (chunk == nullptr)
    {
        return;
    }

    time = clock->time;
    RestartCamera(this, chunk);
}

// While its instance is the player's (or there's none), its camera's step (the countdown down by the clock's advance, to 0)
// while the clock runs; otherwise it stops following
u32 FollowNode::Update(TimeClock* clock)
{
    GameProgress& progress = g_AgentNodesGameController->progress;
    InstanceContext* player = progress.Instance(progress.play.character);
    if (player != owner && player != nullptr)
    {
        StopFollowing();
        return GameNode::Update(clock);
    }

    auto* playerNode = static_cast<PlayerNode*>(GetGameNode(&owner->nodes, NodeCharacter));
    PlayerCharacter* character = playerNode->character;
    if (clock->flags.running == 0)
    {
        // Its last update came while its clock was stopped
        bits.unused0 = 1;
        return GameNode::Update(clock);
    }

    unused760 -= static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit;
    if (unused760 < 0.0f)
    {
        unused760 = 0.0f;
    }

    // The camera's instance read without checking the reference (it always has one: SetOwner and Step make it)
    StepFollowCamera(&camera, clock, character, static_cast<InstanceContext*>(cameraInstance->object));
    bits.unused0 = 0;
    return GameNode::Update(clock);
}

u32 FollowNode::Type()
{
    return FollowType;
}
