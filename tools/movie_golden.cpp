// Movie golden test: demuxes a real retail PSS and decodes its first frames with the software MPEG-2 core.
// Build: g++ -m32 <host flags> tools/movie_golden.cpp src/platform/sdl3/movie/mpeg2.cpp
//     src/platform/sdl3/movie/predict.cpp -o /tmp/movie_golden
// Run from the repo root: /tmp/movie_golden [pss-path] [frames]
#include "../src/platform/sdl3/movie/mpeg2.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
std::vector<u8> ReadFile(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

u32 Checksum(const std::vector<u8>& bytes)
{
    u32 hash = 2166136261u;
    for (u8 byte : bytes)
    {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "disc/FMV/VIVENDI.PSS";
    u32 want = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 30;
    std::vector<u8> pss = ReadFile(path);
    if (pss.empty())
    {
        return 1;
    }
    std::printf("pss %s: %u bytes\n", path, static_cast<u32>(pss.size()));

    HostMovie::Demux demux(pss.data(), static_cast<u32>(pss.size()));
    HostMovie::Decoder decoder;
    u32 packs = 0;
    u32 videoBytes = 0;
    u32 audioBytes = 0;
    while (!demux.Ended())
    {
        if (!demux.Next(
                [&](const HostMovie::VideoPacket& packet)
                {
                    videoBytes += packet.length;
                    decoder.Feed(packet.data, packet.length);
                },
                [&](const u8* data, u32 length)
                {
                    (void)data;
                    audioBytes += length;
                }))
        {
            break;
        }
        packs++;
    }
    std::printf("packs %u video %u bytes audio %u bytes\n", packs, videoBytes, audioBytes);

    u32 frames = 0;
    HostMovie::YuvFrame frame;
    while (frames < want && decoder.NextFrame(&frame) != 0)
    {
        std::printf("frame %u: %ux%u checksum %08x\n", frames, frame.width, frame.height, Checksum(frame.pixels));
        frames++;
    }
    std::printf("decoded %u frames\n", frames);
    return frames > 0 ? 0 : 1;
}
