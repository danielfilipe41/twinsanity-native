// The host's side of the sound: the module (soundmodule.cpp) behind Platform::Audio, its mix handed to SDL, and the movie's
// own sound -- which retail plays through libsdr's block transfer, straight past the module -- mixed on top of it.
//
// Every call into the module takes the audio lock, because the game drives the module from its own thread while the device
// pulls frames on SDL's. The lock is held for as long as a mix of one chunk takes, which is the same bargain the module's
// retail counterpart makes with the I/O processor.
#pragma once

#include "common.h"

namespace SdlAudio
{
// The movie's demuxed PCM, 48 kHz stereo: handed over whole when it is opened, started with its volume (0 to 1), and let go
// when the movie is closed. It plays outside the module's voices, as the movie's sound does on the PS2, where it goes through
// libsdr into the processor's input.
void MoviePcmOpen(u32 frames, const s16* stereo);
void MoviePcmStart(f32 volume);
void MoviePcmStop();

// Lock/unlock the audio mutex for manual synchronization (stream.cpp bypasses Platform::Audio and calls module directly)
void LockAudio();
void UnlockAudio();
}
