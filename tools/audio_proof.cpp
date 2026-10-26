// The reverb headless proof: prints the impulse checksum of every SPU2 reverb
// mode at full depth, delay and feedback (the network's checkable answer).
// Build: g++ -m32 -malign-double -msse2 -mfpmath=sse -O2 -std=gnu++26 -fno-pie
//     -fno-exceptions -fno-rtti -Iinclude -Iinclude/host tools/audio_proof.cpp
//     src/platform/sdl3/audio/spu_mixer.cpp src/platform/sdl3/audio/adpcm.cpp
//     -L/usr/lib32 -lSDL3 -o /tmp/audio_proof (run from the repo root).
#include "../src/platform/sdl3/audio/spu_mixer.h"

#include <cstdint>
#include <cstdio>

namespace
{
// Full knob settings: full input drive, full line lengths, full wall reflection.
constexpr u16 FullDepth = 32767;
constexpr u16 FullDelay = 127;
constexpr u16 FullFeedback = 127;
// The first and last SPU2 effect modes (ReverbMode in include/game/sound.h).
constexpr s32 FirstMode = 1;
constexpr s32 LastMode = 9;

// Sony's effect behind each mode: Studio A/B/C are the Small/Medium/Large rooms.
const char* ModeName(s32 mode)
{
    switch (mode)
    {
        case 1:
            return "Room";
        case 2:
            return "StudioSmall";
        case 3:
            return "StudioMedium";
        case 4:
            return "StudioLarge";
        case 5:
            return "Hall";
        case 6:
            return "SpaceEcho";
        case 7:
            return "ChaosEcho";
        case 8:
            return "Delay";
        case 9:
            return "HalfEcho";
        default:
            return "Unknown";
    }
}
}

int main()
{
    for (s32 mode = FirstMode; mode <= LastMode; mode++)
    {
        u32 checksum = SdlAudio::ReverbImpulseChecksum(mode, FullDepth, FullDepth, FullDelay, FullFeedback);
        std::printf("mode %d %-12s impulse %08x\n", mode, ModeName(mode), checksum);
    }
    return 0;
}
