// The SDL3 movie player's headless proof hooks: checksums and counters the movie proof test reads through the
// opaque Player. Only the sdl3 platform's movie files include this; the game sees Platform::Movie alone.
#pragma once

#include "common.h"
#include "platform/movie.h"

// FNV-1a over the last queued picture's converted RGBA (0 when no picture queued yet)
u32 HostMovieDebug_LastChecksum(const Platform::Movie::Player* player);
// FNV-1a over the last queued picture's planar YUV (0 when no picture queued yet): matches the software core's
// golden checksums byte for byte (tools/movie_golden.cpp)
u32 HostMovieDebug_LastYuvChecksum(const Platform::Movie::Player* player);
// How many pictures QueuePicture has presented (converted + uploaded)
u32 HostMovieDebug_Presented(const Platform::Movie::Player* player);
// The PCM sub-stream's total demuxed bytes (the audio follow-up's input)
u32 HostMovieDebug_AudioBytes(const Platform::Movie::Player* player);
