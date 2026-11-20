#pragma once

#include "common.h"

// The retail program's data, for a build that carries none of it.
//
// A normal computer build links the split's data asm (tools/host_data.py's objects) into the executable, so the game's
// tables, strings and vtables are there from the first instruction. A build configured with DATA=runtime links none of
// them: it reads them out of the disc's own executable (SLES_525.68, the European release's) at the first start, into an
// image whose symbols are the game's, and points every pointer the data holds at the port's own code or at the image
// itself (tools/retail_map.py's symbol list). Nothing of the game's data ships with such a build: the player's disc is
// the only copy, and this is what puts it in the program's hands.
namespace Platform::RetailData
{
// Whether this build already has the data baked in (a build from the split), in which case Load does nothing
bool Baked();
// The player's own `SLES_525.68`, named by --exe (or TWINSANITY_EXE): read instead of the disc's own. A disc of another
// region carries another program (SLUS_209.09, SLPM_658.01), while the data this build reads is the European release's, so
// that file is what a North American or Japanese disc is played with. Called before Load; an empty path means the disc's own
void SetExecutable(const char* path);
// Loads the data from the disc's executable into the image, and relocates it. Called once, after the disc's files are
// located and before the game's first instruction; false (with the reason said) when the data is needed and cannot be
// read or is not the release's. `verbose` says what it did even when it worked
bool Load(bool verbose);
// Writes the image out, one "address value" word per line, for the parity check (tools/retail_parity.py): a build from
// the split has no image to write
bool Dump(const char* path);
}
