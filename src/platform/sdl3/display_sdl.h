// The SDL3 side of the display's settings: the window's size and mode, vsync, the rate and the antialiasing the picture is
// presented with. platform/display.h holds what the game asks for; entry.cpp opens the window with DisplayWindowWidth/Height
// and DisplayWindowFlags, attaches it with DisplayAttach, and every change from the options menu afterwards goes through
// display.cpp's setters, which apply it at once and record it in the settings file.
#pragma once

#include "common.h"

#include <SDL3/SDL.h>

// The settings file's window size (the design's 960x540 when it has none) and the flags to open the window with
u32 DisplayWindowWidth();
u32 DisplayWindowHeight();
SDL_WindowFlags DisplayWindowFlags();

// The window the display's changes are applied to: entry.cpp's, once bgfx draws into it
void DisplayAttach(SDL_Window* window);

// The window's drawable, compared with the swap chain's once a frame: a window the player resized or the window manager moved
// to a display of another size is followed, the resolution recorded with it. Returns whether it changed
void DisplaySync();
