// Clock: the game's fixed step and the picture's own rate.
//
// Retail's game clock is the EE's timer 0, read once a frame at the vblank: a frame's frameTime is exactly one field's
// worth of ticks (11520 at 50 Hz, 9600 at 60). A free-running host clock gives a slightly different number every frame,
// and the camera smoothing, animation blending and particle aging that consume frameTime inherit that jitter. The host
// therefore steps the game clock itself: AdvanceFrame moves it on by one exact step, taken when the wall clock says a
// field is due (LogicStepDue), while WaitVSync paces the picture at its own rate. Within the field Ticks keeps counting
// (the game's per-frame budget checks, GameTimeHasTimeLeft, need time to pass) but never past the step, so the next
// field's difference is still one step and a frame that overruns cannot make the game's clock jump or run backwards.
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
// The wall clock's next logic boundary, and whether one has been set yet
std::chrono::steady_clock::time_point g_NextStep;
bool g_HasNextStep = false;
}

void Platform::Time::Initialise()
{
    g_FrameStart = std::chrono::steady_clock::now();
    g_FrameTicks = 0;
    g_HasNextStep = false;
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

    // 0 is unlimited (the picture presents as fast as the frame is drawn), a negative rate takes the default
    if (displayRate < 0)
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

    g_HasNextStep = false;
}

bool Platform::Time::LogicStepDue()
{
    // The step's turn is the wall clock's, not a count of the display's frames: the picture's rate is the player's to set
    // and a machine that cannot hold it would otherwise slow the game down in proportion (the fields would be counted off
    // the presents that did happen). A frame that arrives late still takes one step and starts the next from when it did,
    // so a step is never paid back in a burst
    if (!g_Started)
    {
        Initialise();
    }

    auto now = std::chrono::steady_clock::now();
    if (!g_HasNextStep)
    {
        g_NextStep = now;
        g_HasNextStep = true;
    }

    if (now < g_NextStep)
    {
        return false;
    }

    auto step = std::chrono::nanoseconds(1000000000ll / (g_LogicRate > 0 ? g_LogicRate : DefaultLogicRate));
    g_NextStep += step;
    if (g_NextStep <= now)
    {
        g_NextStep = now + step;
    }

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

f32 Platform::Time::LogicAlpha()
{
    if (!g_Started || g_Step == 0 || g_LogicRate <= 0)
    {
        return 0.0f;
    }

    // The share of a step's time gone since the step's own boundary. A display frame that lands on the step draws with 0
    // (AdvanceFrame just reset the start), and the ones between it ease towards 1
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_FrameStart);
    double stepNanoseconds = static_cast<double>(g_Step) * 1.0e9 / static_cast<double>(TicksPerSecond);
    f32 alpha = static_cast<f32>(static_cast<double>(elapsed.count()) / stepNanoseconds);
    if (alpha < 0.0f)
    {
        alpha = 0.0f;
    }
    else if (1.0f < alpha)
    {
        alpha = 1.0f;
    }

    return alpha;
}
