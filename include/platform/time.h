#pragma once

#include "common.h"

// The clock the game's frames go by
namespace Platform::Time
{
// The PS2's timer 0, at the bus clock / 256
constexpr u32 TicksPerSecond = 576000;

// Starts it counting from 0, the first time
void Initialise();
// The ticks since then
u64 Ticks();

// The host's framing (the PS2 side counts its own timer and never calls these). Two rates are kept apart:
//
// - the DISPLAY rate is how often the host presents a frame, paced to the monitor's own rate (60 by default, TS_FPS
//   another). It is the loop's own business and the game's logic never sees it.
// - the LOGIC rate is how often the game's frame runs, and it is the disc's: retail's frame IS the vblank, so the PAL
//   release steps its logic 50 times a second and everything the game counts per frame assumes that step.
//
// WaitVSync paces the display's frames and steps nothing; the game's frame is taken by the loop when LogicStepDue says a
// logic step is owed, and that is where AdvanceFrame moves the clock on by one exact field. So frameTime is one exact
// field whatever the monitor does, and the picture runs at the monitor's rate while the game runs at the disc's
s32 FrameRate();
s32 DisplayRate();
void SetRates(s32 logicRate, s32 displayRate);
// Whether the loop owes the game a logic step: call once a display frame, after WaitVSync. It is true 50 times a second
// at a 60 Hz display, so two display frames in twelve repeat the last one
bool LogicStepDue();
// The logic boundary itself: the game's clock moves on by exactly one field
void AdvanceFrame();
}
