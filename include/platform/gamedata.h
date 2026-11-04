#pragma once

#include "common.h"

// Finding the disc's files at launch. The game reads them by their paths on the disc, so a run started somewhere else has
// nothing to open: this asks for them once, keeps the answer beside the saves, and asks again when what it kept no longer
// has the files (a moved or deleted copy).
//
// The files are either an extracted folder (the port's usual way to be set up) or a disc image, which is read through its own
// file system: a player who has the .iso or .bin they own needs to extract nothing.
namespace Platform::GameData
{
// Makes the game's files readable, picking them when it has to. `named` is the run's own answer when it has one (the
// --disc option): a folder or a disc image, used without asking and without being kept. `window` is the run's window, which a
// dialog is a child of on the hosts that ask for one (null when there is none yet). False when there is nothing to run with:
// the player cancelled, or what they chose has neither the files nor an image of the disc
bool Locate(const char* named = nullptr, void* window = nullptr);

// The host's own folder picker: a dialog on the SDL3 hosts, nothing on the ones without one, so a host that has no picker
// answers false and the run then comes from the working directory or TWINSANITY_DATA. `path` is what the player chose (a
// filesystem path, or whatever the host calls a location); false when they cancelled
bool PickFolder(const char* defaultLocation, char* path, u32 size, void* window);

// The host's file picker, for naming one file among disc images (a folder of several .iso files): the same rules as
// PickFolder's, `path` the image chosen
bool PickImage(const char* defaultLocation, char* path, u32 size, void* window);
}
