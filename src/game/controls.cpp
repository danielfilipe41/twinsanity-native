#include "game/clock.h"
#include "game/controls.h"

#include "game/characters.h"

#include "game/agents.h"
#include "game/chunkdata.h"
#include "game/followcamera.h"
#include "game/instances.h"
#include "game/math.h"
#include "game/memory.h"
#include "game/pads.h"
#include "game/place.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

EABI_EXPORT(FUN_00162da8, &ControlsHandler::Move);

namespace
{
constexpr u32 AxisCount = 2;
constexpr u32 ActionCount = 7;

// A stick axis bound as retail binds it — so it gets retail's scale of 1/(1-deadZone), which is what lets a full push travel
// past 1 (1.43), where the stick gates (the crawl's 1.2, the spin's 1.4) live — but with only a sliver of the dead zone left.
// The pad's resting read is not the game's own middle: the report is a whole byte (128 at rest, and the host writes 128 for a
// centred axis and with no pad at all) while the game's middle is 127.5, so a resting axis reads about 0.004 raw and 0.006
// once the scale is on it. Retail's 0.3 dead zone swallows that, so retail's resting stick is exactly zero; with none the
// reading survives, and the character's own guard (CharacterControls::Frame's IsNone, epsilon 5e-5) then sees a live stick on
// a stick nobody is touching and turns the character to a fixed world direction wherever it stands. A sliver of dead zone is
// enough to zero that and small enough that no travel is lost: a full push still reaches 1.4, past both gates
constexpr f32 StickRestDeadZone = 0.02f;
void AddStickAxis(ButtonBindings& bindings, u32 axis, u32 padAxis)
{
    bindings.AddAxis(ButtonBindings::AxisDeadZone, axis, padAxis);
    bindings.axes[axis].deadZone = StickRestDeadZone;
}

CharacterAgent* CharacterOf(InstanceContext* instance)
{
    return static_cast<CharacterAgent*>(static_cast<AgentNode*>(GetGameNode(&instance->nodes, NodeCharacter))->agent);
}

// Whether the follow camera's rig isn't the lens's (the pad then drives nothing): its bits read as retail does, through a null
// follow node too (the word at 0x30 then)
bool RigAway(InstanceContext* instance)
{
    void* follow = GetGameNode(&instance->nodes, NodeFollow);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(follow) + offsetof(FollowNode, camera);
    return RetailRead<FollowCameraBits>(address + offsetof(FollowCamera, bits)).rigAway != 0;
}

// An instance's place as the retail code reads it, also when there's no instance (the word at address 8 then)
ObjectPlace* RetailPlaceOf(const InstanceContext* instance)
{
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(instance) + offsetof(ReferencedObject, place);
    return RetailRead<ObjectPlace*>(address);
}

void ClearMove(CharacterAgent* character)
{
    Vector4 none = g_DefaultBox.min;
    none.w = 1.0f;
    character->moveInput = none;
}

bool IsNone(const Vector4* vector)
{
    return __builtin_fabsf(vector->x) <= Epsilon && __builtin_fabsf(vector->y) <= Epsilon &&
           __builtin_fabsf(vector->z) <= Epsilon;
}

Vector4 Cross(const Vector4* a, const Vector4* b)
{
    return {a->y * b->z - a->z * b->y, a->z * b->x - a->x * b->z, a->x * b->y - a->y * b->x, 1.0f};
}

// A rotation's inverse (InvertRotation inline)
void Invert(Vector4* rotation)
{
    f32 inverse = InverseLength4(0.0f, InverseEpsilon, rotation);
    rotation->w = rotation->w * inverse;
    rotation->x = rotation->x * -inverse;
    rotation->y = rotation->y * -inverse;
    rotation->z = rotation->z * -inverse;
}

// The rotation of the frame of a direction (made unit) about an up axis: its rows the side (up x direction), the up and the
// direction
void FrameRotation(Vector4* rotation, const Vector4* direction, const Vector4* up)
{
    f32 inverse = InverseLength(direction, LengthEpsilon);
    Vector4 unit = {direction->x * inverse, direction->y * inverse, direction->z * inverse, 1.0f};
    Matrix4x4 frame;
    *RowOf(&frame, 0) = Cross(up, &unit);
    *RowOf(&frame, 1) = *up;
    *RowOf(&frame, 2) = unit;
    GetRotationVec(rotation, &frame);
}
}

ControlsHandler* ControlsHandler::Construct(ControlsHandler* handler, u32 axisCount, u32 actionCount)
{
    handler->vtable = g_ControlsHandlerVTable;
    ButtonBindings::Construct(&handler->bindings, axisCount, actionCount);
    handler->bindings.AddButton(ActionCross, PadCross);
    handler->bindings.AddButton(ActionSquare, PadSquare);
    handler->bindings.AddButton(ActionCircle, PadCircle);
    handler->bindings.AddButton(ActionL, PadL1);
    handler->bindings.AddButton(ActionL, PadL2);
    handler->bindings.AddButton(ActionR, PadR1);
    handler->bindings.AddButton(ActionR, PadR2);
    handler->bindings.AddButton(ActionL2, PadL2);
    handler->bindings.AddButton(ActionStart, PadStart);
    return handler;
}

void ControlsHandler::Destroy(u32 destroyFlags)
{
    vtable = g_ControlsHandlerVTable;
    bindings.Destroy(DestroyOnly);
    if ((destroyFlags & 1) != 0)
    {
        MemoryDeallocate2_(this);
    }
}

void ControlsHandler::StickInWorld(ObjectPlace* place, const Matrix4x4* view, const Vector4* stick, Vector4* out)
{
    f32 length = __builtin_sqrtf(stick->x * stick->x + stick->y * stick->y + stick->z * stick->z);
    // Retail compares the view's up with the character's against 0.707 either way and does nothing with it
    Vector4 up = *RowOf(&place->matrix, 1);
    Vector4 world;
    VuRotateVector(view, stick, &world);
    f32 inverse = 1.0f / Kept(length);
    world.x = world.x * inverse;
    world.z = world.z * inverse;
    world.y = world.y * inverse;
    Vector4 side = Cross(&up, &world);
    Vector4 level = Cross(&side, &up);
    out->w = 1.0f;
    out->x = level.x * length;
    out->z = level.z * length;
    out->y = level.y * length;
}

void ControlsHandler::TurnToward(const Vector4* direction, const Vector4* up, const Vector4* forward, Vector4* turn)
{
    Vector4 toward;
    Vector4 from;
    FrameRotation(&toward, direction, up);
    FrameRotation(&from, forward, up);
    *turn = toward;
    Invert(&from);
    MultiplyRotations(turn, turn, &from);
}

void TraceButtonMove(const CharacterAgent* character, const char* what, const void* caller)
{
    static u32 noted = 0;
    if (std::getenv("TS_SPEED") == nullptr || 40000 <= noted++)
    {
        return;
    }

    std::FILE* file = std::fopen("buttonwrite.log", "ab");
    if (file == nullptr)
    {
        return;
    }

    std::fprintf(file,
                 "frame %u inst %p what %s caller %p moveX %g moveZ %g locked %08X\n",
                 static_cast<unsigned>(g_RenderedFrames), static_cast<const void*>(character->instance), what, caller,
                 static_cast<double>(character->buttons.moveX), static_cast<double>(character->buttons.moveZ),
                 static_cast<unsigned>(character->buttons.locked.value));
    std::fclose(file);
}

void ControlsHandler::GiveTurn(Vector4* turn, const Vector4* move, AgentNode* node)
{
    f32 moveX = move->x;
    f32 moveZ = move->z;
    f32 length = Kept(__builtin_sqrtf(moveX * moveX + move->y * move->y + moveZ * moveZ));
    CharacterButtons& buttons = static_cast<CharacterAgent*>(node->agent)->buttons;
    Vector4 axis;
    s32 angle;
    AxisAngleOfRotation(turn, &axis, &angle, 0);
    angle = WrapAngle(angle);
    // The turn as a share of half a turn, signed by the way its axis points
    f32 share = static_cast<f32>(angle) * AngleToRadians * (axis.y < 0.0f ? -InversePi : InversePi);
    // The move made no longer than a whole one (retail's own: the two axes divided by the move's length keeps a digital
    // pad's diagonal, both axes at 1, from running at the root of two's speed)
    if (1.0f < length)
    {
        moveZ = moveZ / length;
        moveX = moveX / length;
    }

    // TS_SPEED=1: the only writer of the two fields the walk reads as its stick. If this never runs for a body during a
    // chase, that body's stick is whatever the last caller left there - a stale value from an earlier level or menu -
    // and the walk goes by it every frame
    if (std::getenv("TS_SPEED") != nullptr)
    {
        static u32 noted = 0;
        if (noted++ < 600)
        {
            s32 kind = static_cast<CharacterAgent*>(node->agent)->properties->GetInt(CharacterKindProperty);
            std::FILE* file = std::fopen("stickwrite.log", "ab");
            if (file != nullptr)
            {
                std::fprintf(file, "GiveTurn kind %d moveX %g moveZ %g length %g caller %p\n", static_cast<int>(kind),
                             static_cast<double>(moveX), static_cast<double>(moveZ), static_cast<double>(length),
                             __builtin_return_address(0));
                std::fclose(file);
            }
        }
    }

    buttons.turn = buttons.locked.turn != 0 ? 0.0f : share;
    // The move fields take the pair made no longer than a whole one above, not the axes as they came in (asm
    // FUN_00162c40: f24 = f21 and f23 = f22 first, then div.s f23,f22,f20 and div.s f24,f21,f20 when the length is past
    // 1, and those are what 0x162D68/0x162D7C store). It matters: the walk rebuilds its stick from these two and every
    // speed it asks for is that stick times a property, so raw axes make any diagonal (and any analog push past the
    // circle) faster than the property says - a digital diagonal by the root of two, a stick held 20 degrees off by
    // about a sixth. This port kept the raw pair and every character ran fast by exactly the excess
    buttons.moveZ = buttons.locked.moveZ != 0 ? 0.0f : moveZ;
    buttons.moveX = buttons.locked.moveX != 0 ? 0.0f : moveX;
    TraceButtonMove(static_cast<CharacterAgent*>(node->agent), "GiveTurn", __builtin_return_address(0));
}

void ControlsHandler::Move(f32 length, InstanceContext* instance, const Vector4* direction)
{
    // TS_SPEED=1: who asks a body's controls for a move, and how hard. A body with no pad and no motion driving it should
    // not be asked at all, and yet one is - the chase's runner sits at exactly stick 1.0 for a whole run, which is a
    // scripted full-strength ask rather than anything a stick produces. The caller's return address names it; resolve it
    // with addr2line against build/host/sdl3/game-with-data (and symbol_addrs.txt for the retail names)
    if (std::getenv("TS_SPEED") != nullptr)
    {
        static u32 noted = 0;
        if (noted++ < 400)
        {
            auto* controls = static_cast<ControlsNode*>(GetGameNode(&instance->nodes, NodeControls));
            auto* node = static_cast<AgentNode*>(GetGameNode(&instance->nodes, NodeCharacter));
            s32 kind = node != nullptr ? static_cast<CharacterAgent*>(node->agent)->properties->GetInt(CharacterKindProperty)
                                       : -1;
            std::FILE* file = std::fopen("movewrite.log", "ab");
            if (file != nullptr)
            {
                std::fprintf(file,
                             "frame %u Move length %g kind %d pad %p motionDriven %u caller %p\n",
                             static_cast<unsigned>(g_RenderedFrames), static_cast<double>(length),
                             static_cast<int>(kind),
                             static_cast<void*>(controls != nullptr ? controls->pad : nullptr),
                             controls != nullptr ? controls->bits.motionDriven : 0xFFFFFFFFu,
                             __builtin_return_address(0));
                std::fclose(file);
            }
        }
    }

    ObjectPlace* place = instance->place;
    RotateAndTranslate(place);
    auto* node = static_cast<AgentNode*>(GetGameNode(&instance->nodes, NodeCharacter));
    auto* character = static_cast<CharacterAgent*>(node->agent);
    Vector4 turn;
    TurnToward(direction, RowOf(&place->matrix, 1), RowOf(&place->matrix, 2), &turn);
    Vector4 ahead = {0.0f, 0.0f, length, 1.0f};
    Vector4 move;
    // Retail bug: this first move is overwritten (the call only normalizes the turn)
    RotateByQuaternion(&turn, &ahead, &move, 0);
    Vector4 rotation;
    GetRotationVec(&rotation, &place->matrix);
    MultiplyRotations(&rotation, &rotation, &turn);
    RotateByQuaternion(&rotation, &ahead, &move, 0);
    GiveTurn(&turn, &move, node);
    character->moveInput = move;
}

void ControlsHandler::MoveBy(TimeClock*, const Vector4* velocity, InstanceContext* instance)
{
    if (IsNone(velocity))
    {
        ClearMove(CharacterOf(instance));
        return;
    }

    CharacterAgent* character = CharacterOf(instance);
    Vector4 direction = *velocity;
    f32 share = 1.0f / character->WalkTopSpeed();
    direction.x = direction.x * share;
    direction.y = direction.y * share;
    direction.z = direction.z * share;
    // TS_STICK2D=1: the length taken from the two flat axes alone. Retail squares all three (asm 0x1630D8: mul.s f20,f2,f2
    // / mul.s f4,f0,f0 / mul.s f1,f3,f3, add.s twice, then the root), so this is not what the game does - it is a test of
    // whether a single frame's vertical snap, from a bump the runner climbs over, is what his motion driven speed reads as a
    // run. If the chase settles at his walk with this set, the snap is the whole defect and the fix is upstream of here
    static const bool flatOnly = std::getenv("TS_STICK2D") != nullptr;
    f32 length = flatOnly
                     ? __builtin_sqrtf(direction.x * direction.x + direction.z * direction.z)
                     : __builtin_sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    f32 inverse = InverseLength(&direction, LengthEpsilon);
    direction.x = direction.x * inverse;
    direction.y = direction.y * inverse;
    direction.z = direction.z * inverse;
    Move(length, instance, &direction);
}

void ControlsHandler::ReadButtons(GamePad* pad, InstanceContext* instance)
{
    if (RigAway(instance))
    {
        return;
    }

    CharacterButtons& buttons = CharacterOf(instance)->buttons;
    f32 cross = bindings.Pressure(pad, ActionCross);
    f32 circle = bindings.Pressure(pad, ActionCircle);
    f32 square = bindings.Pressure(pad, ActionSquare);
    f32 left = bindings.Pressure(pad, ActionL);
    f32 right = bindings.Pressure(pad, ActionR);
    buttons.cross = buttons.locked.cross != 0 ? 0.0f : cross;
    buttons.square = buttons.locked.square != 0 ? 0.0f : square;
    buttons.circle = buttons.locked.circle != 0 ? 0.0f : circle;
    buttons.shoulders = buttons.locked.shoulders != 0 ? 0.0f : right - left;
}

CharacterControls* CharacterControls::Construct(CharacterControls* handler)
{
    ControlsHandler::Construct(handler, AxisCount, ActionCount);
    handler->vtable = g_CharacterControlsVTable;
    // The d-pad's axes are read first and the stick's after: a d-pad press is a whole axis (and nothing when it isn't pressed),
    // and reading it first keeps a resting stick's reading (never exactly zero) from hiding the d-pad behind it. The sticks have
    // no dead zone of their own but do keep retail's scale (see AddStickAxis), so a full push reaches retail's own travel
    handler->bindings.AddAxis(ButtonBindings::AxisDeadZone, AxisX, PadAxisDirectionX);
    AddStickAxis(handler->bindings, AxisX, PadAxisLeftX);
    handler->bindings.AddAxis(ButtonBindings::AxisDeadZone, AxisY, PadAxisDirectionY);
    AddStickAxis(handler->bindings, AxisY, PadAxisLeftY);
    handler->Reset();
    return handler;
}

void CharacterControls::Destroy(u32 destroyFlags)
{
    vtable = g_CharacterControlsVTable;
    ControlsHandler::Destroy(destroyFlags);
}

void CharacterControls::Reset()
{
}

void CharacterControls::Frame(TimeClock*, GamePad* pad, InstanceContext* instance)
{
    if (RigAway(instance))
    {
        return;
    }

    Vector4 stick = {bindings.AxisValue(pad, AxisX), 0.0f, bindings.AxisValue(pad, AxisY), 1.0f};
    if (IsNone(&stick))
    {
        ClearMove(CharacterOf(instance));
        return;
    }

    ObjectPlace* place = instance->place;
    const Matrix4x4* view = &instance->chunk->matrix;
    RotateAndTranslate(place);
    f32 length = __builtin_sqrtf(stick.x * stick.x + stick.y * stick.y + stick.z * stick.z);
    Vector4 world;
    StickInWorld(place, view, &stick, &world);
    Move(length, instance, &world);
}

VehicleControls* VehicleControls::Construct(VehicleControls* handler, u32 stickIsCross)
{
    ControlsHandler::Construct(handler, AxisCount, ActionCount);
    handler->stickIsCross = stickIsCross;
    handler->vtable = g_VehicleControlsVTable;
    // The d-pad's axes are read first and the stick's after: a d-pad press is a whole axis (and nothing when it isn't pressed),
    // and reading it first keeps a resting stick's reading (never exactly zero) from hiding the d-pad behind it. The sticks have
    // no dead zone of their own but do keep retail's scale (see AddStickAxis), so a full push reaches retail's own travel
    handler->bindings.AddAxis(ButtonBindings::AxisDeadZone, AxisX, PadAxisDirectionX);
    AddStickAxis(handler->bindings, AxisX, PadAxisLeftX);
    handler->bindings.AddAxis(ButtonBindings::AxisDeadZone, AxisY, PadAxisDirectionY);
    AddStickAxis(handler->bindings, AxisY, PadAxisLeftY);
    handler->Reset();
    return handler;
}

void VehicleControls::Destroy(u32 destroyFlags)
{
    vtable = g_VehicleControlsVTable;
    ControlsHandler::Destroy(destroyFlags);
}

void VehicleControls::Reset()
{
}

void VehicleControls::Frame(TimeClock*, GamePad* pad, InstanceContext* instance)
{
    f32 x = bindings.AxisValue(pad, AxisX);
    f32 y = bindings.AxisValue(pad, AxisY);
    CharacterButtons& buttons = CharacterOf(instance)->buttons;
    f32 cross = bindings.Pressure(pad, ActionCross);
    f32 circle = bindings.Pressure(pad, ActionCircle);
    f32 square = bindings.Pressure(pad, ActionSquare);
    if (stickIsCross != 0)
    {
        // Retail bug: the stick's y as cross (the hoverboard's) is written over by ReadButtons, which runs right after
        buttons.cross = buttons.locked.cross != 0 ? 0.0f : y;
        y = 0.0f;
    }
    else
    {
        buttons.cross = buttons.locked.cross != 0 ? 0.0f : cross;
    }

    buttons.square = buttons.locked.square != 0 ? 0.0f : square;
    buttons.circle = buttons.locked.circle != 0 ? 0.0f : circle;
    buttons.locked.cross = 0;
    buttons.locked.square = 0;
    buttons.locked.circle = 0;
    if (x == 0.0f && y == 0.0f)
    {
        // The move left as it was
        return;
    }

    auto* follow = static_cast<FollowNode*>(GetGameNode(&instance->nodes, NodeFollow));
    InstanceContext* camera =
        follow->cameraInstance != nullptr ? static_cast<InstanceContext*>(follow->cameraInstance->object) : nullptr;
    ObjectPlace* cameraPlace = RetailPlaceOf(camera);
    ObjectPlace* place = instance->place;
    RotateAndTranslate(cameraPlace);
    RotateAndTranslate(place);
    // Retail builds a frame from the camera's up and the character's forward that nothing uses: only its calls are left
    Vector4 forward = *RowOf(&place->matrix, 2);
    Matrix4x4 transposed;
    TransposeMatrix(&cameraPlace->matrix, 3, &transposed);
    VuRotateVector(&transposed, &forward, &forward);

    // The stick along the camera's level side and ahead
    Vector4 side = *RowOf(&cameraPlace->matrix, 0);
    Vector4 ahead = *RowOf(&cameraPlace->matrix, 2);
    side.y = 0.0f;
    ahead.y = 0.0f;
    f32 inverse = InverseLength(&side, LengthEpsilon);
    side.x = side.x * inverse;
    side.y = side.y * inverse;
    side.z = side.z * inverse;
    inverse = InverseLength(&ahead, LengthEpsilon);
    ahead.x = ahead.x * inverse;
    ahead.y = ahead.y * inverse;
    ahead.z = ahead.z * inverse;
    f32 moveX = side.x * x + ahead.x * y;
    f32 moveZ = side.z * x + ahead.z * y;
    f32 length = __builtin_sqrtf(moveX * moveX + moveZ * moveZ);
    // As the character's: the stick along both axes at once made a whole move's worth, not more (retail's own)
    if (1.0f < length)
    {
        f32 scale = 1.0f / Kept(length);
        moveZ = moveZ * scale;
        moveX = moveX * scale;
    }

    buttons.moveX = buttons.locked.moveX != 0 ? 0.0f : moveX;
    buttons.moveZ = buttons.locked.moveZ != 0 ? 0.0f : moveZ;
    TraceButtonMove(CharacterOf(instance), "VehicleControls", __builtin_return_address(0));
}

void StickControls::Destroy(u32 destroyFlags)
{
    vtable = g_ControlsHandlerVTable;
    bindings.Destroy(DestroyOnly);
    if ((destroyFlags & 1) != 0)
    {
        MemoryDeallocate2_(this);
    }
}

void StickControls::Reset()
{
}

void StickControls::Frame(TimeClock*, GamePad* pad, InstanceContext* instance)
{
    CharacterButtons& buttons = CharacterOf(instance)->buttons;
    f32 x = bindings.AxisValue(pad, AxisX);
    f32 y = bindings.AxisValue(pad, AxisY);
    // Retail (FUN_00165010): axis 0 goes to turn (0xF4), axis 1 to moveZ (0xF8), each zeroed when its lock bit is set
    // (bits 0 and 1 of the word at 0xF0). It writes no moveX at all, so this matches it exactly
    buttons.turn = buttons.locked.turn != 0 ? 0.0f : x;
    buttons.moveZ = buttons.locked.moveZ != 0 ? 0.0f : y;
    TraceButtonMove(CharacterOf(instance), "StickControls", __builtin_return_address(0));
}
