#include "debug.h"

#include "game/gamecontroller.h"
#include "game/oleg.h"
#include "game/olegpages.h"
#include "game/pads.h"
#include "retail/libc.h"

#include <cstdlib>

extern "C"
{
    volatile u32 g_DebugStep = 0;
    volatile s32 g_DebugValues[DebugValueCount] = {};

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
    u32 frame = g_DebugFrame + 1;
    g_DebugFrame = frame;
    // TEMPORARY: the controller page shown at boot, with the pad's down pressed in bursts so the list scrolls
    if (std::getenv("TWINSANITY_TEST_CONTROLLER") != nullptr && g_OlegGameController != nullptr)
    {
        extern ControllerOptionsPage* g_ControllerOptionsPage;
        static bool opened = false;
        if (!opened && frame > 3 && g_ControllerOptionsPage != nullptr)
        {
            opened = true;
            OLEG& oleg = g_OlegGameController->oleg;
            oleg.optionsMenu.home = g_ControllerOptionsPage;
            oleg.optionsMenu.firstPage = g_ControllerOptionsPage;
            oleg.Show(oleg.screens[OLEG::ScreenOptions], 0, 0);
        }

        if (opened)
        {
            // TEMPORARY: the game's own screens take the menu back, so it is shown again every frame
            OLEG& oleg = g_OlegGameController->oleg;
            oleg.Show(oleg.screens[OLEG::ScreenOptions], 0, 0);
        }

        if (opened && frame > 40 && frame % 6 == 0)
        {
            // TEMPORARY: the selection moved as the pad's down would move it (the pad isn't polled out here)
            static_cast<MenuPage*>(g_ControllerOptionsPage)->SelectNext(0);
        }
    }
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
