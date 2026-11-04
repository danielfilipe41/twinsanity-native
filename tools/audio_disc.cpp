// The disc's own sound, read and decoded by itself: a region of a bank file or of the music file, walked block by block and
// reported. This is the other half of the port's audio checks - those say what the engine does with the bytes, this says what
// the bytes are - so a sound that misbehaves can be looked at before anything else is blamed.
//
// It reports the region's shape (whether it opens with the bank tools' header, how many blocks, whether the block headers are
// plausible), its level and length, where its loop and end flags sit, and its energy over time. With --wav it writes what the
// region sounds like when played straight through, at the pitch the game would give it.
//
// Build and run (from the repo root):
//   ninja audio-disc
//   ./build/host/audio_disc disc/CRASH6/ENGLISH.MB <offset> <size> [--rate 32000] [--wav /tmp/sound.wav]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr int BlockBytes = 16;
constexpr int BlockSamples = 28;
constexpr int BlockWords = 8;
constexpr unsigned HeaderBytes = 0x40;
constexpr int MixRate = 48000;

constexpr unsigned char LoopEnd = 0x01;
constexpr unsigned char LoopRepeat = 0x02;
constexpr unsigned char LoopStart = 0x04;

// The filter's coefficients in 64ths, as the chip uses them
constexpr int FilterCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

void WriteLe16(std::vector<unsigned char>& out, unsigned value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void WriteLe32(std::vector<unsigned char>& out, unsigned value)
{
    WriteLe16(out, value & 0xFFFF);
    WriteLe16(out, (value >> 16) & 0xFFFF);
}

struct Region
{
    std::vector<unsigned char> bytes;
    unsigned skip = 0;
    unsigned blocks = 0;
    unsigned endFlags = 0;
    unsigned repeatFlags = 0;
    unsigned startFlags = 0;
    unsigned firstLoopStart = 0;
    unsigned firstLoopEnd = 0;
    unsigned badHeaders = 0;
    std::vector<int16_t> samples;
    std::vector<double> envelope;
};

// Decodes a region's ADPCM the way the voice does, one block after the other, carrying the filter's history
void Decode(Region& region)
{
    int previous1 = 0;
    int previous2 = 0;
    for (unsigned block = 0; block < region.blocks; block++)
    {
        const unsigned char* at = region.bytes.data() + region.skip + block * BlockBytes;
        if (at[0] == 0xFF && at[1] == 0xFF)
        {
            break; // padding at the end of a bank
        }

        const signed char* bytes = reinterpret_cast<const signed char*>(at);
        int header = (unsigned char)bytes[0] | ((unsigned char)bytes[1] << 8);
        int shift = (header & 0xF) + 16;
        int filter = (header >> 4) & 0x1F;
        int first = filter < 5 ? FilterCoefficients[filter][0] : 0;
        int second = filter < 5 ? FilterCoefficients[filter][1] : 0;

        for (int index = 0; index < BlockSamples; index++)
        {
            unsigned char packed = (unsigned char)bytes[2 + index / 2];
            int nibble = index % 2 == 0 ? packed & 0xF : packed >> 4;
            int sample = ((nibble << 28) >> shift) + ((first * previous1 + second * previous2 + 32) >> 6);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            previous2 = previous1;
            previous1 = sample;
            region.samples.push_back((int16_t)sample);
        }
    }
}

void Report(const Region& region, int rate, int pitch)
{
    std::printf("region: %u bytes, header %u, %u blocks, %u samples\n", (unsigned)region.bytes.size(), region.skip,
                region.blocks, (unsigned)region.samples.size());
    std::printf("  flags: %u end, %u repeat, %u loop start (first loop start at block %u, first end at block %u)\n",
                region.endFlags, region.repeatFlags, region.startFlags, region.firstLoopStart, region.firstLoopEnd);
    if (region.badHeaders != 0)
    {
        std::printf("  WARNING: %u block headers are not a shift of 12 or less and a filter of 4 or less: the data is not where\n"
                    "           this tool thinks the samples start\n",
                    region.badHeaders);
    }

    if (!region.samples.empty())
    {
        double sum = 0.0;
        int peak = 0;
        for (int16_t sample : region.samples)
        {
            sum += (double)sample * sample;
            int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
        }

        double rms = std::sqrt(sum / region.samples.size());
        double played = (double)region.samples.size() * 4096.0 / pitch;
        std::printf("  level: RMS %.0f (%.1f%% of full), peak %d (%.1f%% of full)\n", rms, rms / 327.68, peak,
                    peak / 327.68);
        std::printf("  length: %.3f seconds at %d Hz, played at pitch %d (0x%X) it is %.3f seconds\n",
                    (double)region.samples.size() / rate, rate, pitch, pitch, played / MixRate);

        std::printf("  energy:");
        size_t bin = (size_t)(MixRate * 0.1);
        for (size_t start = 0; start < region.samples.size() && start / bin < 32; start += bin)
        {
            double total = 0.0;
            size_t count = 0;
            for (size_t index = start; index < region.samples.size() && index < start + bin; index++)
            {
                total += (double)region.samples[index] * region.samples[index];
                count++;
            }

            std::printf(" %.0f", count ? std::sqrt(total / count) : 0.0);
        }

        std::printf("\n");
    }
}

bool WriteWav(const char* path, const std::vector<int16_t>& samples, int rate)
{
    std::vector<unsigned char> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    WriteLe32(wav, 36 + (unsigned)samples.size() * 2);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    WriteLe32(wav, 16);
    WriteLe16(wav, 1);   // PCM
    WriteLe16(wav, 1);   // mono
    WriteLe32(wav, rate);
    WriteLe32(wav, rate * 2);
    WriteLe16(wav, 2);
    WriteLe16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    WriteLe32(wav, (unsigned)samples.size() * 2);
    for (int16_t sample : samples)
    {
        WriteLe16(wav, (unsigned)(uint16_t)sample);
    }

    std::FILE* file = std::fopen(path, "wb");
    if (!file)
    {
        std::perror(path);
        return false;
    }

    std::fwrite(wav.data(), 1, wav.size(), file);
    std::fclose(file);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr,
                     "usage: %s <file> <offset> <size> [--rate N] [--pitch N] [--wav out.wav] [--raw]\n"
                     "  a region of the disc's sound: what it is made of, its level, and its length when played\n",
                     argv[0]);
        return 2;
    }

    const char* path = argv[1];
    unsigned long offset = std::strtoul(argv[2], nullptr, 0);
    unsigned long size = std::strtoul(argv[3], nullptr, 0);
    int rate = 32000;
    int pitch = 0x1000;
    const char* wavPath = nullptr;
    bool raw = false;

    for (int index = 4; index < argc; index++)
    {
        std::string option = argv[index];
        if (option == "--rate" && index + 1 < argc)
        {
            rate = std::atoi(argv[++index]);
        }
        else if (option == "--pitch" && index + 1 < argc)
        {
            pitch = std::atoi(argv[++index]);
        }
        else if (option == "--wav" && index + 1 < argc)
        {
            wavPath = argv[++index];
        }
        else if (option == "--raw")
        {
            raw = true;
        }
    }

    std::FILE* file = std::fopen(path, "rb");
    if (!file)
    {
        std::perror(path);
        return 1;
    }

    std::fseek(file, (long)offset, SEEK_SET);
    Region region;
    region.bytes.resize(size);
    size_t got = std::fread(region.bytes.data(), 1, size, file);
    std::fclose(file);
    region.bytes.resize(got);

    // The bank tools' header, unless this is the samples themselves
    if (!raw && region.bytes.size() > HeaderBytes && std::memcmp(region.bytes.data(), "MSVp", 4) == 0)
    {
        region.skip = HeaderBytes;
        std::printf("header: \"MSVp\", %u bytes of it, name \"%.16s\"\n", HeaderBytes,
                    reinterpret_cast<const char*>(region.bytes.data() + 0x20));
    }

    if (region.bytes.size() < region.skip + BlockBytes)
    {
        std::printf("region: too short for a block\n");
        return 1;
    }

    region.blocks = (unsigned)((region.bytes.size() - region.skip) / BlockBytes);
    for (unsigned block = 0; block < region.blocks; block++)
    {
        const unsigned char* at = region.bytes.data() + region.skip + block * BlockBytes;
        unsigned shift = at[0] & 0xF;
        unsigned filter = (at[0] >> 4) & 0x1F;
        if (shift > 12 || filter > 4)
        {
            region.badHeaders++;
        }

        unsigned char flags = at[1];
        if ((flags & LoopEnd) != 0)
        {
            if (region.endFlags == 0)
            {
                region.firstLoopEnd = block;
            }

            region.endFlags++;
        }

        if ((flags & LoopRepeat) != 0)
        {
            region.repeatFlags++;
        }

        if ((flags & LoopStart) != 0)
        {
            if (region.startFlags == 0)
            {
                region.firstLoopStart = block;
            }

            region.startFlags++;
        }
    }

    Decode(region);
    Report(region, rate, pitch);

    if (wavPath != nullptr)
    {
        // What it sounds like played through, at the rate it was made at
        if (WriteWav(wavPath, region.samples, rate))
        {
            std::printf("wrote %s (%u samples at %d Hz)\n", wavPath, (unsigned)region.samples.size(), rate);
        }
    }

    return 0;
}
