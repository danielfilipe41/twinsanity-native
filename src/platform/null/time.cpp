// Clock: the host's frames at the game's field rate.
//
// Retail's game clock is the EE's timer 0, read once a frame at the vblank: a frame's frameTime is exactly one field's
// worth of ticks (11520 at 50 Hz, 9600 at 60). A free-running host clock gives a slightly different number every frame,
// and the camera smoothing, animation blending and particle aging that consume frameTime inherit that jitter. The host
// therefore steps the game clock itself: WaitVSync is the field boundary and advances it by one exact step. Within the
// field Ticks keeps counting (the game's per-frame budget checks, GameTimeHasTimeLeft, need time to pass) but never past
// the step, so the next field's difference is still one step and a frame that overruns cannot make the game's clock jump
// or run backwards.
#include "platform/time.h"

#include <chrono>

namespace
{
// The display's frames a second (how often the host presents) and the game's logic steps a second (the disc's field rate:
// 50 on PAL, 60 on NTSC). They are deliberately separate numbers: the picture follows the monitor, the game follows the
// disc, and a 60 Hz display simply repeats the two logic frames in twelve that it has nothing new for
constexpr s32 DefaultDisplayRate = 60;
constexpr s32 DefaultLogicRate = 50;

std::chrono::steady_clock::time_point g_FrameStart;
bool g_Started = false;
// The ticks at the current logic frame's boundary, and the ticks one logic step advances by
u64 g_FrameTicks = 0;
u64 g_Step = Platform::Time::TicksPerSecond / DefaultLogicRate;
s32 g_LogicRate = DefaultLogicRate;
s32 g_DisplayRate = DefaultDisplayRate;
// The display frames owed towards the next logic step, in units of 1/(logic * display) of a second: each display frame
// adds the logic rate and a step is taken when the sum passes the display rate
u32 g_StepDebt = 0;
}

void Platform::Time::Initialise()
{
    g_FrameStart = std::chrono::steady_clock::now();
    g_FrameTicks = 0;
    g_Started = true;
}

u64 Platform::Time::Ticks()
{
    if (!g_Started)
    {
        Initialise();
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_FrameStart);
    u64 within = static_cast<u64>(elapsed.count()) * TicksPerSecond / 1000000000;
    // The field's own step is the most a frame may count: past it the next field boundary is the one that matters (and
    // GameTimeHasTimeLeft's budget must run out rather than wrap into the next frame's time)
    if (g_Step != 0 && within >= g_Step)
    {
        within = g_Step - 1;
    }

    return g_FrameTicks + within;
}

s32 Platform::Time::FrameRate()
{
    return g_LogicRate;
}

s32 Platform::Time::DisplayRate()
{
    return g_DisplayRate;
}

void Platform::Time::SetRates(s32 logicRate, s32 displayRate)
{
    if (logicRate <= 0)
    {
        logicRate = DefaultLogicRate;
    }

    if (displayRate <= 0)
    {
        displayRate = DefaultDisplayRate;
    }

    g_LogicRate = logicRate;
    g_DisplayRate = displayRate;
    // 50 and 60 divide the tick rate exactly, which are the fields the game itself was written for (11520 and 9600 ticks
    // a step); an override is rounded rather than refused
    g_Step = (TicksPerSecond + static_cast<u32>(logicRate) / 2) / static_cast<u32>(logicRate);
    if (g_Step == 0)
    {
        g_Step = 1;
    }

    g_StepDebt = 0;
}

bool Platform::Time::LogicStepDue()
{
    g_StepDebt += static_cast<u32>(g_LogicRate);
    if (g_StepDebt < static_cast<u32>(g_DisplayRate))
    {
        // This display frame has nothing of the game's to show: the loop skips it and the window keeps the last picture
        return false;
    }

    g_StepDebt -= static_cast<u32>(g_DisplayRate);
    return true;
}

void Platform::Time::AdvanceFrame()
{
    if (!g_Started)
    {
        Initialise();
    }

    g_FrameTicks += g_Step;
    g_FrameStart = std::chrono::steady_clock::now();
}
