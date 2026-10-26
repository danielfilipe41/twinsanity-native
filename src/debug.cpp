#include "debug.h"

#include "game/gamecontroller.h"
#include "retail/libc.h"

#include <cstdio>
#include <cstdlib>

extern "C"
{
    volatile u32 g_DebugStep = 0;
    volatile s32 g_DebugValues[DebugValueCount] = {};

    bool g_SoundLog = false;

    __attribute__((section(".data"))) volatile u32 g_DebugFixedTime = 0;
    volatile u32 g_DebugFrame = 0;
    volatile u32 g_DebugFreezeFrame = 0;
    volatile u32 g_DebugStates[DebugStateCount] = {};
    volatile u32 g_DebugRandom[DebugRandomFrames] = {};
    volatile DebugInput g_DebugInputs[DebugInputCount] = {};
}

namespace
{
// The high word of the game controller's 64 bit states (bits 44-49 the state it's in, 50-55 the next)
union StatesHighWord
{
    u32 value;
    struct
    {
        u32 unused0 : 12;
        u32 state : 6;
        u32 next : 6;
        u32 unused24 : 8;
    };
};
CHECK_SIZE(StatesHighWord, 4);

// A state logged is the frame and the state's low byte
constexpr u32 LoggedFrameShift = 8;
constexpr u32 LoggedStateMask = 0xFF;

volatile u32* StatesHigh()
{
    return reinterpret_cast<volatile u32*>(&G_GameController->states) + 1;
}
}

void DebugFrameRendered()
{
    // A crash-log test (TS_CRASH_TEST): the frame hook dereferences a null pointer a few frames in, so a run can show that a
    // crash inside the game's own code is written out with its frames. Debug-only: unset, nothing here runs
    static const bool crashTest = std::getenv("TS_CRASH_TEST") != nullptr;
    if (crashTest && g_DebugFrame == 600)
    {
        *reinterpret_cast<volatile int*>(0) = 1;
    }

    u32 frame = g_DebugFrame + 1;
    g_DebugFrame = frame;
    g_DebugRandom[frame % DebugRandomFrames] = *reinterpret_cast<volatile u32*>(&g_Impure->randomNext);
    if (G_GameController != nullptr)
    {
        StatesHighWord states;
        states.value = *StatesHigh();
        u32 state = states.state;
        static u32 logged;
        if (logged == 0 || (g_DebugStates[logged - 1] & LoggedStateMask) != state)
        {
            if (logged < DebugStateCount)
            {
                g_DebugStates[logged++] = frame << LoggedFrameShift | state;
            }

            // A run driven at the host's own frame rate tells a script where the menus are: TS_STATELOG prints each
            // game controller state transition with the frame it happened on
            static const bool stateLog = std::getenv("TS_STATELOG") != nullptr;
            if (stateLog)
            {
                std::fprintf(stderr, "host state: frame %u -> %u\n", frame, state);
            }
        }
    }

    for (volatile DebugInput& input : g_DebugInputs)
    {
        if (input.kind == DebugInput::State && input.frame == frame && G_GameController != nullptr)
        {
            StatesHighWord states;
            states.value = *StatesHigh();
            states.next = input.value;
            *StatesHigh() = states.value;
        }
        else if (input.kind == DebugInput::Press)
        {
            if (input.frame == frame)
            {
                g_TestPadButtons = input.value;
            }
            else if (input.frame + input.length == frame)
            {
                g_TestPadButtons = 0;
            }
        }
    }

    while (g_DebugFreezeFrame == frame)
    {
    }
}
