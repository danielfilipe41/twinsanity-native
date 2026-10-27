// Controllers: SDL3 gamepads, with the keyboard as a development fallback, presented as
// DualShock 2s. Ports 0 and 1 take slot 0 each (no multitap); a port reads Stable once opened, its report's buttons
// pressed 0, its sticks at 128 in the middle, and pressure sensitive buttons fully pressed when held, except the
// triggers which pass their axes through. The test hook (g_TestPadButtons, as on the PS2) holds pad 0's buttons.
#include "platform/pads.h"

#include "graphics/bgfx_context.h"

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
u8 g_Align[PortCount][6] = {};
bool g_SdlTried = false;
bool g_SdlReady = false;

bool ValidPad(s32 port, s32 slot)
{
    return 0 <= port && port < PortCount && slot == 0;
}

// The run ended: the window's close asked for it (SDL sends SDL_EVENT_QUIT when the window manager's close is used).
// The game's loop has no exit of its own to reach here, so the platform shuts the renderer and SDL down and ends the
// process -- otherwise the game keeps running with no window, and only a signal can stop it
[[noreturn]] void EndTheRun()
{
    BgfxShutdown();
    SDL_Quit();
    std::exit(0);
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
    }

    if (pad == nullptr)
    {
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

bool PadButton(SDL_Gamepad* pad, SDL_GamepadButton button)
{
    return pad != nullptr && SDL_GetGamepadButton(pad, button);
}

bool KeyDown(const bool* keys, s32 keysCount, SDL_Scancode code)
{
    return keys != nullptr && code < keysCount && keys[code];
}

// A stick's report value, 128 in the middle
u8 StickReport(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    if (pad == nullptr)
    {
        return StickMiddle;
    }

    return static_cast<u8>((static_cast<s32>(SDL_GetGamepadAxis(pad, axis)) + 32768) >> 8);
}

// An axis inside this much of zero counts as a stick at rest: an idle stick rests a little off it, and the keyboard's fallback
// is only held back while the gamepad's stick is really somewhere
constexpr Sint16 StickRest = 1024;

bool StickAtRest(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    if (pad == nullptr)
    {
        return true;
    }

    Sint16 value = SDL_GetGamepadAxis(pad, axis);
    return value < StickRest && value > -StickRest;
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
    // The keyboard fallback, only with a video subsystem behind it
    int keysCount = 0;
    const bool* keys = (SDL_WasInit(SDL_INIT_VIDEO) != 0) ? SDL_GetKeyboardState(&keysCount) : nullptr;
    // The held buttons (pressed 1): the gamepad's, the keyboard fallback's, and the test run's on pad 0

    u32 held = 0;
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || KeyDown(keys, keysCount, SDL_SCANCODE_UP))
    {
        held |= BitUp;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || KeyDown(keys, keysCount, SDL_SCANCODE_RIGHT))
    {
        held |= BitRight;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || KeyDown(keys, keysCount, SDL_SCANCODE_DOWN))
    {
        held |= BitDown;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || KeyDown(keys, keysCount, SDL_SCANCODE_LEFT))
    {
        held |= BitLeft;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) || KeyDown(keys, keysCount, SDL_SCANCODE_SPACE))
    {
        held |= BitCross;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || KeyDown(keys, keysCount, SDL_SCANCODE_K))
    {
        held |= BitCircle;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_WEST) || KeyDown(keys, keysCount, SDL_SCANCODE_J))
    {
        held |= BitSquare;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_NORTH) || KeyDown(keys, keysCount, SDL_SCANCODE_L))
    {
        held |= BitTriangle;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || KeyDown(keys, keysCount, SDL_SCANCODE_Q))
    {
        held |= BitL1;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || KeyDown(keys, keysCount, SDL_SCANCODE_E))
    {
        held |= BitR1;
    }
    // Triggers are axes in SDL3, not buttons: the axis value past the press threshold (or the keyboard fallback)
    if ((pad != nullptr && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TriggerPressed) ||
        KeyDown(keys, keysCount, SDL_SCANCODE_1))
    {
        held |= BitL2;
    }
    if ((pad != nullptr && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TriggerPressed) ||
        KeyDown(keys, keysCount, SDL_SCANCODE_3))
    {
        held |= BitR2;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_BACK) || KeyDown(keys, keysCount, SDL_SCANCODE_BACKSPACE))
    {
        held |= BitSelect;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_START) || KeyDown(keys, keysCount, SDL_SCANCODE_RETURN))
    {
        held |= BitStart;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK))
    {
        held |= BitL3;
    }
    if (PadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
    {
        held |= BitR3;
    }
    if (port == 0 && slot == 0)
    {
        held |= g_TestPadButtons;
    }

    u8 leftX = StickReport(pad, SDL_GAMEPAD_AXIS_LEFTX);
    u8 leftY = StickReport(pad, SDL_GAMEPAD_AXIS_LEFTY);
    u8 rightX = StickReport(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    u8 rightY = StickReport(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    // The keyboard's sticks, full deflection while held (the gamepad wins while its stick is really off the middle: an
    // idle stick rests a little off zero, so "exactly zero" kept the keyboard out with a pad plugged in)
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_LEFTX))
    {
        if (KeyDown(keys, keysCount, SDL_SCANCODE_A))
        {
            leftX = 0;
        }
        if (KeyDown(keys, keysCount, SDL_SCANCODE_D))
        {
            leftX = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_LEFTY))
    {
        if (KeyDown(keys, keysCount, SDL_SCANCODE_W))
        {
            leftY = 0;
        }
        if (KeyDown(keys, keysCount, SDL_SCANCODE_S))
        {
            leftY = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_RIGHTX))
    {
        if (KeyDown(keys, keysCount, SDL_SCANCODE_J))
        {
            rightX = 0;
        }
        if (KeyDown(keys, keysCount, SDL_SCANCODE_L))
        {
            rightX = FullPressure;
        }
    }
    if (StickAtRest(pad, SDL_GAMEPAD_AXIS_RIGHTY))
    {
        if (KeyDown(keys, keysCount, SDL_SCANCODE_I))
        {
            rightY = 0;
        }
        if (KeyDown(keys, keysCount, SDL_SCANCODE_K))
        {
            rightY = FullPressure;
        }
    }

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
