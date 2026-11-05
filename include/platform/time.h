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
// - the DISPLAY rate is how often the host presents a frame: the player's setting (settings.txt's `render`, 50 when the
//   file has none, never below the logic rate). It is the loop's own business and the game's logic never sees it.
// - the LOGIC rate is how often the game's frame runs, and it is the disc's: retail's frame IS the vblank, so the PAL
//   release steps its logic 50 times a second and everything the game counts per frame assumes that step.
//
// WaitVSync paces the display's frames and steps nothing; the game's frame is taken by the loop when LogicStepDue says a
// logic step is owed (the wall clock's 50 a second), and that is where AdvanceFrame moves the clock on by one exact field.
// So frameTime is one exact field whatever the display does, and the picture runs at its rate while the game runs at the
// disc's, the frames between two steps drawing the state eased between them
s32 FrameRate();
// The picture's rate, or 0 when it is unlimited: WaitVSync then returns at once and the loop presents as fast as it can draw
// (with vsync on, the display is what limits it)
s32 DisplayRate();
// The logic's step and the picture's rate: a displayRate of 0 is unlimited, a negative one takes the default
void SetRates(s32 logicRate, s32 displayRate);
// Whether the loop owes the game a logic step: call once a display frame, after WaitVSync. The step's turn is the wall
// clock's (50 a second on this PAL disc), whatever the picture's rate is: the display frames that fall between two steps
// draw the state eased between them instead of stepping it
bool LogicStepDue();
// The logic boundary itself: the game's clock moves on by exactly one field
void AdvanceFrame();
// How far the display's clock is into the next logic step, 0 to 1: the draw between two steps interpolates the game's
// state by it (0 at a step's own draw, so the stepped state is what that frame shows)
f32 LogicAlpha();
}
