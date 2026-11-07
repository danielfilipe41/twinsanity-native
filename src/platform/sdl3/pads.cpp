// Controllers: SDL3 gamepads, with the keyboard as a development fallback, presented as
// DualShock 2s. Ports 0 and 1 take slot 0 each (no multitap); a port reads Stable once opened, its report's buttons
// pressed 0, its sticks at 128 in the middle, and pressure sensitive buttons fully pressed when held, except the
// triggers which pass their axes through. The test hook (g_TestPadButtons, as on the PS2) holds pad 0's buttons.
#include "platform/pads.h"

#include "graphics/bgfx_context.h"
#include "platform/keybinds.h"
#include "platform/system.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>

extern "C"
{
    volatile u32 g_TestPadButtons = 0;
}

namespace
{
using namespace Platform::Pads;
// The inputs a key or a pad's button is on, which the file beside the settings file says (platform/keybinds.h)
using namespace Platform::Keybinds;
using Action = Platform::Keybinds::Action;

// The presented pads: ports 0 and 1, slot 0 each
constexpr s32 PortCount = 2;
// The report's length and analog mode byte (7 for analog, 3 for the 6 half-words after the buttons)
constexpr s32 ReportLength = 20;
constexpr u8 AnalogMode = 0x73;
// The sticks' middle and a fully pressed button
constexpr u8 StickMiddle = 0x80;
constexpr u8 FullPressure = 0xFF;
// A trigger counts as pressed past this axis value, and the rumble's length per SetActuatorDirect call
constexpr Sint16 TriggerPressed = 256;
constexpr u32 RumbleMs = 500;
// The held bits (pressed 1, as the game reads them: PadReportButtons) of each input
constexpr u32 BitL2 = 0x1;
constexpr u32 BitR2 = 0x2;
constexpr u32 BitL1 = 0x4;
constexpr u32 BitR1 = 0x8;
constexpr u32 BitTriangle = 0x10;
constexpr u32 BitCircle = 0x20;
constexpr u32 BitCross = 0x40;
constexpr u32 BitSquare = 0x80;
constexpr u32 BitSelect = 0x100;
constexpr u32 BitL3 = 0x200;
constexpr u32 BitR3 = 0x400;
constexpr u32 BitStart = 0x800;
constexpr u32 BitUp = 0x1000;
constexpr u32 BitRight = 0x2000;
constexpr u32 BitDown = 0x4000;
constexpr u32 BitLeft = 0x8000;

// A pressure sensitive button's held bit and report byte
struct PressureBit
{
    u32 bit;
    ReportByte byte;
};

constexpr PressureBit PressureBits[] = {
    {BitL2, PressureL2},         {BitR2, PressureR2},         {BitL1, PressureL1}, {BitR1, PressureR1},
    {BitTriangle, PressureTriangle}, {BitCircle, PressureCircle}, {BitCross, PressureCross}, {BitSquare, PressureSquare},
    {BitUp, PressureUp},         {BitRight, PressureRight},   {BitDown, PressureDown}, {BitLeft, PressureLeft},
};

bool g_Opened[PortCount] = {};
SDL_Gamepad* g_Pads[PortCount] = {};
// How many reads a port with no pad waits before listing the gamepads again. SDL_GetGamepads allocates its array, and a
// session played on the keyboard would otherwise ask (and allocate) every frame for a controller that is not there
constexpr s32 PadRetryReads = 30;
s32 g_PadRetry[PortCount] = {};
u8 g_Align[PortCount][6] = {};
bool g_SdlTried = false;
bool g_SdlReady = false;

bool ValidPad(s32 port, s32 slot)
{
    return 0 <= port && port < PortCount && slot == 0;
}

// The run ended: the window's close asked for it (SDL sends SDL_EVENT_QUIT when the window manager's close is used).
// This runs inside the game's frame, so it only asks for the end (Platform::System::Exit, the same ask the pause menu's close
// makes): the frame finishes and its end shuts the renderer and SDL down and ends the process, instead of tearing them down
// under the frame's own draws -- otherwise the game keeps running with no window, and only a signal can stop it
void EndTheRun()
{
    Platform::System::Exit(0);
}

// The input subsystems, initialized once (the entry already did it; the headless tests did not)
bool PollInputs()
{
    if (!g_SdlTried)
    {
        g_SdlTried = true;
        g_SdlReady = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!g_SdlReady)
        {
            std::fprintf(stderr, "sdl3: SDL_INIT_GAMEPAD failed: %s\n", SDL_GetError());
        }
    }

    if (g_SdlReady)
    {
        // The window's events: the close is the SDL_QUIT event (the window managers' close button and the window
        // backend's own close both arrive as it), the reason the run ends here
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0)
        {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID != 0))
            {
                EndTheRun();
            }
        }

        SDL_PumpEvents();
        SDL_UpdateGamepads();
    }

    return g_SdlReady;
}

// The port's gamepad, opening the port's controller (ports read the gamepads in order) when missing
SDL_Gamepad* ConnectedPad(s32 port)
{
    SDL_Gamepad* pad = g_Pads[port];
    if (pad != nullptr && !SDL_GamepadConnected(pad))
    {
        SDL_CloseGamepad(pad);
        g_Pads[port] = nullptr;
        pad = nullptr;
        // A pad went away: the next read looks for the one after it straight away
        g_PadRetry[port] = 0;
    }

    if (pad == nullptr)
    {
        if (g_PadRetry[port] > 0)
        {
            g_PadRetry[port]--;
            return nullptr;
        }

        g_PadRetry[port] = PadRetryReads;
        s32 count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids != nullptr)
        {
            if (port < count)
            {
                pad = SDL_OpenGamepad(ids[port]);
                if (pad != nullptr)
                {
                    g_Pads[port] = pad;
                }
            }

            SDL_free(ids);
        }
    }

    return pad;
}

// Whether a keyboard input is held. A binding of none is SDL_SCANCODE_UNKNOWN (0), which the state array reports as not held, so an
// action with no key is a pad's alone
bool KeyDown(const bool* keys, s32 keysCount, SDL_Scancode code)
{
    return keys != nullptr && code < keysCount && keys[code];
}

// The sticks' dead zone, a percentage of the axis' half range: read from the pads' settings by the first stick, then as the
// options screen sets it (0xFFFFFFFF until it's read)
constexpr u32 DeadZoneUnread = 0xFFFFFFFF;
u32 g_DeadZonePercent = DeadZoneUnread;

// An axis inside this much of zero counts as a stick at rest: an idle stick rests a little off it, and the keyboard's fallback
// is only held back while the gamepad's stick is really somewhere
constexpr Sint16 StickRest = 1024;

// A stick's report value, 128 in the middle. Inside the dead zone it is the middle, and outside it the axis is moved out to the
// zone's edge and scaled over the range left, so the stick's whole travel is still used
u8 StickReport(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    if (pad == nullptr)
    {
        return StickMiddle;
    }

    const s32 value = Platform::Pads::DeadZonedAxis(SDL_GetGamepadAxis(pad, axis), Platform::Pads::DeadZonePercent());
    return static_cast<u8>((value + 32768) >> 8);
}

bool StickAtRest(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    if (pad == nullptr)
    {
        return true;
    }

    // A stick in its dead zone is at rest too: the keyboard's fallback is the one that moves it then
    const s32 zone = Platform::Pads::DeadZoneTicks(Platform::Pads::DeadZonePercent());
    const Sint16 rest = zone > StickRest ? static_cast<Sint16>(zone) : StickRest;
    Sint16 value = SDL_GetGamepadAxis(pad, axis);
    return value < rest && value > -rest;
}

// A trigger's pressure, 0 without a gamepad
u8 TriggerPressure(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    if (pad == nullptr)
    {
        return 0;
    }

    Sint16 value = SDL_GetGamepadAxis(pad, axis);
    if (value <= 0)
    {
        return 0;
    }

    return static_cast<u8>((static_cast<u32>(value) * FullPressure + 16383u) / 32767u);
}

// The buttons the file binds: the action, and the bit of the report it presses
struct ButtonBinding
{
    Action action;
    u32 bit;
};

constexpr ButtonBinding ButtonBindings[] = {
    {Action::L2, BitL2},         {Action::R2, BitR2},         {Action::L1, BitL1},         {Action::R1, BitR1},
    {Action::Triangle, BitTriangle}, {Action::Circle, BitCircle}, {Action::Cross, BitCross}, {Action::Square, BitSquare},
    {Action::Select, BitSelect},  {Action::L3, BitL3},         {Action::R3, BitR3},         {Action::Start, BitStart},
    {Action::Up, BitUp},          {Action::Right, BitRight},   {Action::Down, BitDown},     {Action::Left, BitLeft},
};

// Whether a pad's input is pressed: a button, a stick's click, or a trigger's axis past the press threshold
bool PadInputDown(SDL_Gamepad* pad, s32 input)
{
    if (pad == nullptr)
    {
        return false;
    }

    switch (input)
    {
    case PadSouth:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    case PadEast:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    case PadWest:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    case PadNorth:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    case PadLeftShoulder:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    case PadRightShoulder:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    case PadLeftTrigger:
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TriggerPressed;
    case PadRightTrigger:
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TriggerPressed;
    case PadBack:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    case PadStart:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    case PadLeftStick:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    case PadRightStick:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    case PadDpadUp:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    case PadDpadDown:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    case PadDpadLeft:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    case PadDpadRight:
        return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    }

    return false;
}

// The keys pressed this frame and not the last, and the same of a pad's buttons, reported to the options' controller page: SDL
// says what is held, not what was just pressed, so the press is the first frame a hold is seen
void NotePressed(SDL_Gamepad* pad, const bool* keys, s32 keysCount)
{
    static bool previousKeys[SDL_SCANCODE_COUNT];
    static u8 previousPads[PadInputCount];

    if (keys != nullptr)
    {
        const s32 count = keysCount < SDL_SCANCODE_COUNT ? keysCount : SDL_SCANCODE_COUNT;
        for (s32 code = 1; code < count; code++)
        {
            if (keys[code] && !previousKeys[code])
            {
                Platform::Keybinds::NoteKey(code);
            }
            previousKeys[code] = keys[code];
        }
    }

    for (s32 input = 1; input < PadInputCount; input++)
    {
        const u8 down = PadInputDown(pad, input) ? 1 : 0;
        if (down != 0 && previousPads[input] == 0)
        {
            Platform::Keybinds::NotePadInput(input);
        }
        previousPads[input] = down;
    }
}
}

s32 Platform::Pads::Initialise()
{
    PollInputs();
    return 0;
}

s32 Platform::Pads::Open(s32 port, s32 slot, void* buffer)
{
    (void)buffer;
    if (!ValidPad(port, slot))
    {
        return 0;
    }

    g_Opened[port] = true;
    return 1;
}

s32 Platform::Pads::Close(s32 port, s32 slot)
{
    if (!ValidPad(port, slot))
    {
        return 0;
    }

    if (g_Pads[port] != nullptr)
    {
        SDL_CloseGamepad(g_Pads[port]);
        g_Pads[port] = nullptr;
    }

    g_Opened[port] = false;
    return 1;
}

s32 Platform::Pads::Read(s32 port, s32 slot, u8* data)
{
    if (!ValidPad(port, slot) || !g_Opened[port] || data == nullptr)
    {
        return 0;
    }
    SDL_Gamepad* pad = PollInputs() ? ConnectedPad(port) : nullptr;
    // The keyboard's layout, from the file beside the settings file (read once, and written with the defaults when there is none,
    // so the first run leaves a file to edit)
    Platform::Keybinds::Load();
    // The keyboard fallback, only with a video subsystem behind it
    int keysCount = 0;
    const bool* keys = (SDL_WasInit(SDL_INIT_VIDEO) != 0) ? SDL_GetKeyboardState(&keysCount) : nullptr;
    // The held buttons (pressed 1): what the file binds each action to, a key or a pad's input, and the test run's on pad 0
    auto bound = [&](Action action, Kind kind)
    {
        const s32 input = Platform::Keybinds::Binding(action, kind);
        return kind == Kind::Pad ? PadInputDown(pad, input)
                                 : KeyDown(keys, keysCount, static_cast<SDL_Scancode>(input));
    };
    auto pressed = [&](Action action) { return bound(action, Kind::Key) || bound(action, Kind::Pad); };

    u32 held = 0;
    for (const ButtonBinding& binding : ButtonBindings)
    {
        if (pressed(binding.action))
        {
            held |= binding.bit;
        }
    }
    if (port == 0 && slot == 0)
    {
        held |= g_TestPadButtons;
    }

    u8 leftX = StickReport(pad, SDL_GAMEPAD_AXIS_LEFTX);
    u8 leftY = StickReport(pad, SDL_GAMEPAD_AXIS_LEFTY);
    u8 rightX = StickReport(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    u8 rightY = StickReport(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    // The bound keys and buttons' sticks, full deflection while held (the gamepad wins while its stick is really off the middle: an
    // idle stick rests a little off zero, so "exactly zero" kept the keyboard out with a pad plugged in)
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_LEFTX))
    {
        if (pressed(Action::LeftStickLeft))
        {
            leftX = 0;
        }
        if (pressed(Action::LeftStickRight))
        {
            leftX = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_LEFTY))
    {
        if (pressed(Action::LeftStickUp))
        {
            leftY = 0;
        }
        if (pressed(Action::LeftStickDown))
        {
            leftY = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_RIGHTX))
    {
        if (pressed(Action::RightStickLeft))
        {
            rightX = 0;
        }
        if (pressed(Action::RightStickRight))
        {
            rightX = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_RIGHTY))
    {
        if (pressed(Action::RightStickUp))
        {
            rightY = 0;
        }
        if (pressed(Action::RightStickDown))
        {
            rightY = FullPressure;
        }
    }

    // What was pressed this frame, for the options' controller page to bind as a row is rebound
    NotePressed(pad, keys, keysCount);

    u8 pressureL2 = TriggerPressure(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    u8 pressureR2 = TriggerPressure(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    // The report: the mode, the button word pressed 0 (high byte first), the sticks, and the pressures
    data[0] = 0;
    data[ReportMode] = AnalogMode;
    data[ReportButtonsHigh] = static_cast<u8>(~(held >> 8));
    data[ReportButtonsLow] = static_cast<u8>(~held);
    data[StickRightX] = rightX;
    data[StickRightY] = rightY;
    data[StickLeftX] = leftX;
    data[StickLeftY] = leftY;
    for (u32 i = 0; i < sizeof(PressureBits) / sizeof(PressureBits[0]); i++)
    {
        if ((held & PressureBits[i].bit) == 0)
        {
            data[PressureBits[i].byte] = 0;
        }
        else if (PressureBits[i].bit == BitL2 && pressureL2 > 0)
        {
            data[PressureBits[i].byte] = pressureL2;
        }
        else if (PressureBits[i].bit == BitR2 && pressureR2 > 0)
        {
            data[PressureBits[i].byte] = pressureR2;
        }
        else
        {
            data[PressureBits[i].byte] = FullPressure;
        }
    }

    return ReportLength;
}

Platform::Pads::State Platform::Pads::GetState(s32 port, s32 slot)
{
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return StateDisconnected;
    }

    return StateStable;
}

Platform::Pads::RequestState Platform::Pads::GetRequestState(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return RequestComplete;
}

s32 Platform::Pads::InfoMode(s32 port, s32 slot, ModeInfo info, s32 index)
{
    (void)index;
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return 0;
    }

    switch (info)
    {
    case InfoCurrentId:
    case InfoCurrentExtendedId:
        return ModeIdAnalog;
    case InfoCurrentIndex:
        return 0;
    case InfoModeTable:
        // The mode table's length (the game only checks it is not 0)
        return 2;
    }

    return 0;
}

s32 Platform::Pads::SetMainMode(s32 port, s32 slot, MainMode mode, ModeLock lock)
{
    (void)mode;
    (void)lock;
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return 0;
    }

    return 1;
}

s32 Platform::Pads::InfoActuator(s32 port, s32 slot, s32 actuator, s32 term)
{
    (void)term;
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return 0;
    }

    // The two vibration motors, or one motor's description
    constexpr s32 ActuatorCount = 2;
    if (actuator == CountActuators)
    {
        return ActuatorCount;
    }

    return actuator == 0 || actuator == 1 ? 1 : 0;
}

s32 Platform::Pads::SetActuatorAlign(s32 port, s32 slot, const u8 align[6])
{
    if (!ValidPad(port, slot) || !g_Opened[port] || align == nullptr)
    {
        return 0;
    }

    for (u32 i = 0; i < sizeof(g_Align[port]); i++)
    {
        g_Align[port][i] = align[i];
    }

    return 1;
}

s32 Platform::Pads::SetActuatorDirect(s32 port, s32 slot, const u8 values[6])
{
    if (!ValidPad(port, slot) || !g_Opened[port] || values == nullptr)
    {
        return 0;
    }

    // The small motor on or off, the big one by strength
    constexpr u32 SmallMotor = 0;
    constexpr u32 BigMotor = 1;
    if (g_Pads[port] != nullptr)
    {
        u16 low = static_cast<u16>(values[BigMotor] * 257);
        u16 high = values[SmallMotor] != 0 ? 0xFFFF : 0;
        SDL_RumbleGamepad(g_Pads[port], low, high, RumbleMs);
    }

    return 1;
}

s32 Platform::Pads::InfoPressureMode(s32 port, s32 slot)
{
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return 0;
    }

    return 1;
}

s32 Platform::Pads::EnterPressureMode(s32 port, s32 slot)
{
    if (!ValidPad(port, slot) || !g_Opened[port])
    {
        return 0;
    }

    return 1;
}

bool Platform::Pads::DeadZoneAvailable()
{
    // The host's pads are SDL's, and their sticks' dead zone is this port's
    return true;
}

u32 Platform::Pads::DeadZonePercent()
{
    // Read from the settings file by the first stick that asks (the file is the settings' own, so its parsing stays there)
    if (g_DeadZonePercent == DeadZoneUnread)
    {
        g_DeadZonePercent = HostSettingsDeadZone();
    }

    return g_DeadZonePercent;
}

void Platform::Pads::SetDeadZonePercent(u32 percent)
{
    g_DeadZonePercent = percent > MaxDeadZone ? MaxDeadZone : percent;
    HostSettingsSetDeadZone(g_DeadZonePercent);
}
