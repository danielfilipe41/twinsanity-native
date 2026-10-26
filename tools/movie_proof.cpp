// The movie headless proof: drives VIVENDI.PSS's first 10 frames through the Player API (Open/Step/Draw
// sequence, Draw headless-safe with no BgfxInit) and prints YUV + RGBA checksums. Build: g++ -m32 <host flags>
// tools/movie_proof.cpp src/platform/sdl3/movie/movie.cpp src/platform/sdl3/movie/mpeg2.cpp
//     src/platform/sdl3/movie/predict.cpp src/platform/null/files.cpp -o /tmp/movie_proof (+ bgfx libs for the
//     view-27 Draw submit path's symbols; run from the repo root, no window needed).
#include "../src/platform/sdl3/movie/movie_debug.h"
#include "../src/platform/sdl3/movie/mpeg2.h"

#include "platform/movie.h"

#include <cstdint>
#include <cstdio>


int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "disc/FMV/VIVENDI.PSS";
    alignas(16) static u8 storage[Platform::Movie::PlayerSize];
    auto* player = reinterpret_cast<Platform::Movie::Player*>(storage);
    Platform::Movie::Construct(player);
    // No BeginPresenting: headless has no vsync presenter, WaitFrame stays a no-op
    if (!Platform::Movie::Open(player, "\\FMV\\VIVENDI.PSS;1", 0, 640, nullptr))
    {
        std::printf("proof: Open failed for \\FMV\\VIVENDI.PSS;1 (tried %s)\n", path);
        return 1;
    }

    Platform::Movie::StartSound(player, 1.0f);
    Platform::Movie::Area area = {0, 0, 960, 540};
    u32 decoded = 0;
    // Retail's frame loop shape: Step decodes, QueuePicture presents, Draw draws; WaitFrame paces vsync
    for (u32 frame = 0; frame < 10; frame++)
    {
        if (!Platform::Movie::Step(player))
        {
            break;
        }
        Platform::Movie::WaitFrame(player);
        Platform::Movie::QueuePicture(player);
        Platform::Movie::Draw(player, area, 640, 480, 0);
        u32 rgba = HostMovieDebug_LastChecksum(player);
        u32 yuv = HostMovieDebug_LastYuvChecksum(player);
        std::printf("frame %u: yuv %08x rgba %08x presented %u\n", frame, yuv, rgba,
                    HostMovieDebug_Presented(player));
        if (rgba == 0)
        {
            std::printf("proof: zero checksum at frame %u\n", frame);
            Platform::Movie::Close(player);
            return 1;
        }
        decoded++;
    }
    u32 audio = HostMovieDebug_AudioBytes(player);
    std::printf("decoded %u frames audio %u bytes\n", decoded, audio);
    Platform::Movie::Close(player);
    if (decoded != 10)
    {
        std::printf("proof: want 10 frames\n");
        return 1;
    }
    return 0;
}
