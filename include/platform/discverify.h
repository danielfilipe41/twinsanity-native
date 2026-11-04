#pragma once

#include "common.h"

// Which retail disc an image is. An image is identified by what is in it, never by its file name: redump.org publishes the
// whole image's sector count, CRC-32 and SHA-1 for each release of the disc, and an image that matches one is that release
// byte for byte.
//
// The files themselves are read by Platform::Files (a folder of extracted files or the image's own volume), so this is only
// ever asked for by the run's own --verify-disc. A copy that has been modified (a translation patch, say) plays as the
// original does, and is reported as a modified copy rather than refused.
namespace Platform::DiscVerify
{
// Reads the whole image once and says which known dump it is. False when the file cannot be read. Prints its finding either
// way: the size, the CRC-32, the SHA-1, and the release they add up to (or that they are no known dump of this disc)
bool Image(const char* path);
}
