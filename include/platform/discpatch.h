#pragma once

#include "common.h"

#include <string>

// A patch of the disc image: xdelta3's VCDIFF, an IPS-style PPF (1.0, 2.0 or 3.0) or BPS, as the community ships a change of
// the whole disc. The image is the patched one while the game reads it, and no copy of it is made: a patch keeps only the
// ranges it changes, so a 2.5 GB disc costs what its patch says it changes rather than another 2.5 GB, which is what a page or
// a phone could not do. The bytes are the patch's own: they are as if the image had been patched on disk.
//
// The source the patch was made over is the image the game was given, so a patch of another disc (or of a raw dump when the
// image is a plain ISO) is refused with what does not match said in the log. Only a plain ISO image can be patched: a raw dump
// keeps sync headers and ECC in its file, and the patch's offsets would be of the dump's, not of its user bytes.
namespace Platform::DiscPatch
{
// Reads a patch of `image` (both the host's paths). False, with what is wrong said in the log, when the file is not a patch
// this port reads (a VCDIFF with its own code table or secondary compression, a damaged one, one made for another disc) or the
// image is not a plain ISO. A patch already loaded is forgotten first.
bool Load(const char* patch, const char* image);
void Forget();
bool Loaded();

// The patch's kind, for the log ("xdelta3 (VCDIFF)", "PPF 2.0", "BPS")
const char* Label();
// The image's size as the patch leaves it (0 when no patch is loaded)
u64 Size();

// The patched image's bytes at [offset, offset + size): as many as the image has, 0 when it has none there
s32 ReadAt(u64 offset, void* buffer, s32 size);
}
