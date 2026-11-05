// The controller's bindings, from keybinds.txt beside the settings file: which key and which pad button each of the game's inputs
// is on, so a player keeps the layout their hands know. A gamepad is SDL's to map (the system's remapping applies), but which of
// its buttons the game's cross, circle and the rest answer to is this file's too, so a pad can be rearranged the way a keyboard
// can. The options' controller page writes the file back as a row is rebound. A platform that has no rebinding (the console)
// answers no actions, which keeps the page off its menus.
#pragma once

#include "common.h"

namespace Platform::Keybinds
{
// The game's inputs: the pad's buttons, and each stick's four directions (on a keyboard the sticks are full deflection while a
// key is held)
enum class Action : u32
{
    Cross,
    Circle,
    Square,
    Triangle,
    Up,
    Down,
    Left,
    Right,
    L1,
    R1,
    L2,
    R2,
    Select,
    Start,
    L3,
    R3,
    LeftStickUp,
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    RightStickUp,
    RightStickDown,
    RightStickRight,
    RightStickLeft,
    Count,
};

// What an action is bound to: a key (SDL's scancode) or a pad's input (one of PadInput)
enum class Kind : u32
{
    Key,
    Pad,
    Count,
};

// The pad's inputs a binding can name, in SDL's own names for them (the triggers are axes, the shoulders buttons)
enum PadInput : s32
{
    PadNone = 0,
    PadSouth,
    PadEast,
    PadWest,
    PadNorth,
    PadLeftShoulder,
    PadRightShoulder,
    PadLeftTrigger,
    PadRightTrigger,
    PadBack,
    PadStart,
    PadLeftStick,
    PadRightStick,
    PadDpadUp,
    PadDpadDown,
    PadDpadLeft,
    PadDpadRight,
    PadInputCount,
};

// How many actions the platform binds (0: it has no rebinding at all, and the options' controller page is not offered)
u32 Actions();
// Reads the file, once: a run that finds none writes one with the defaults in it. Called by the pad code before its first read
void Load();
// An action's name in the file and on the page ("cross", "left stick up")
const char* ActionName(Action action);

// What an action is bound to, this platform's code for it (SDL's scancode for a key, a PadInput for a pad's input; 0 when the
// action has none of that kind, which leaves it to the other kind alone)
s32 Binding(Action action, Kind kind);
// The binding put in place, written to the file at the next Save
void SetBinding(Action action, Kind kind, s32 input);
// The file's name of an input ("Space", "Left Shift", "south"), "none" for the code 0. A static buffer, good until the next call
const char* InputName(Kind kind, s32 input);
// The file written with the bindings as they are: a player's comments and the lines this build has no action for are kept
void Save();

// The input pressed since the last ClearTaken (the first of them), which the controller page takes as the row's new binding
bool Taken(Kind* kind, s32* input);
void ClearTaken();
// Whether an input ends a capture instead of being bound (escape, the pad's back button): the two the page keeps for itself, so
// they can only be bound from the file
bool IsCancel(Kind kind, s32 input);

// The pad code's report of a press this frame (the keyboard's scancode, a PadInput): the first of a frame is the one the
// controller page binds. Called from the pad read, which is where a press is seen
void NoteKey(s32 scancode);
void NotePadInput(s32 input);
}
