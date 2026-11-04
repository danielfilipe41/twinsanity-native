#pragma once

#include "common.h"

// Finding the disc's files at launch. The game reads them by their paths on the disc, so a run started somewhere else has
// nothing to open: this asks for the folder once, keeps the answer beside the saves, and asks again when the folder it
// kept no longer has the files (a moved or deleted copy).
namespace Platform::GameData
{
// Makes the game's files readable, picking the folder when it has to. False when there is nothing to run with: the player
// cancelled, or chose a folder that isn't the disc's
bool Locate();

// The host's own folder picker: a dialog on the SDL3 hosts, nothing on the ones without one, so a host that has no picker
// answers false and the run then comes from the working directory or TWINSANITY_DATA. `path` is what the player chose (a
// filesystem path, or whatever the host calls a location); false when they cancelled
bool PickFolder(const char* defaultLocation, char* path, u32 size);
}
