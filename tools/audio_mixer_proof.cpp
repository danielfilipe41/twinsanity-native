// The offline audio proof: drives the SPU mixer with real disc data (a voice
// sound out of a bank, and a music track's region) and measures its stereo
// output. Prints RMS, peak, the non-zero fraction, and checks for NaN/Inf, for
// full-scale clipping everywhere and for clicks at the mixer's own frame
// boundaries. Build (from the repo root):
//   g++ -m32 -malign-double -msse2 -mfpmath=sse -O2 -std=gnu++26 -fno-pie -fno-exceptions -fno-rtti
//       -Iinclude -Iinclude/host -Isrc/platform/sdl3 -I/usr/include/SDL3 tools/audio_mixer_proof.cpp
//       src/platform/sdl3/audio/spu_mixer.cpp src/platform/sdl3/audio/adpcm.cpp
//       -L/usr/lib32 -lSDL3 -o /tmp/audio_mixer_proof
// Run it from the repo root (it reads disc/CRASH6/ENGLISH.MB and MUSIC.MB).
#include "game/renderer.h"
#include "game/sound.h"
#include "platform/audio.h"

// The game's own option values, which the host settings file (linked in below, at this proof's build) reads and writes: a
// proof of the mixer alone doesn't link the game, so it defines them here
GroupVolume g_GroupVolumes[VolumeGroupCount] RETAIL(D_003D6F20);
u32 g_MusicStereo RETAIL(D_00309E68);
u8 g_WidescreenTv RETAIL(D_0030AAC4);

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// The mixer's door for a bank read (it calls this to pull a music region; the
// real one reads the disc, this test reads the file it opened).
namespace
{
std::FILE* g_Song = nullptr;
}

namespace Platform::Stream
{
void Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    (void)channel;
    (void)file;
    if (g_Song == nullptr)
    {
        return;
    }

    std::fseek(g_Song, static_cast<long>(offset), SEEK_SET);
    std::fread(destination, 1, size, g_Song);
}
}

namespace SdlAudio
{
struct Voice;
void LockMixer();
void UnlockMixer();
void MixStereo(s16* out, u32 frames);
u8* SpuMemory();
u32 LoadSoundBankSamples(u32 sound, const u8* data, u32 size);
}

namespace
{
constexpr u32 MixRate = 48000;
constexpr u32 MixChannels = 2;

struct Stats
{
    double rms = 0.0;
    s32 peak = 0;
    u32 nonZero = 0;
    u32 frames = 0;
    u32 fullScale = 0;
    u32 bad = 0; // NaN/Inf never reach s16, so this counts |x| == 32768-ish clipping runs
    double maxStep = 0.0;
};

Stats Measure(const std::vector<s16>& pcm)
{
    Stats stats;
    long long sum = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2)
    {
        s32 left = pcm[i];
        s32 right = pcm[i + 1];
        s32 mag = std::abs(left) > std::abs(right) ? std::abs(left) : std::abs(right);
        sum += static_cast<long long>(left) * left + static_cast<long long>(right) * right;
        stats.frames++;
        if (mag > stats.peak)
        {
            stats.peak = mag;
        }

        if (left != 0 || right != 0)
        {
            stats.nonZero++;
        }

        if (std::abs(left) >= 32767 || std::abs(right) >= 32767)
        {
            stats.fullScale++;
        }

        if (i >= 2)
        {
            double step = std::abs(static_cast<double>(left) - pcm[i - 2]);
            if (step > stats.maxStep)
            {
                stats.maxStep = step;
            }
        }
    }

    if (stats.frames != 0)
    {
        stats.rms = std::sqrt(static_cast<double>(sum) / (stats.frames * 2)) / 32768.0;
    }

    return stats;
}

// Reads a little-endian u32 out of a bank header
u32 Word(const std::vector<u8>& data, u32 at)
{
    return static_cast<u32>(data[at]) | (static_cast<u32>(data[at + 1]) << 8) | (static_cast<u32>(data[at + 2]) << 16) |
           (static_cast<u32>(data[at + 3]) << 24);
}

std::vector<u8> ReadFile(const char* path, u32 offset, u32 size)
{
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return {};
    }

    std::fseek(file, static_cast<long>(offset), SEEK_SET);
    std::vector<u8> data(size);
    size_t got = std::fread(data.data(), 1, size, file);
    std::fclose(file);
    data.resize(got);
    return data;
}

// The biggest frame-to-frame step between two frames of a capture: a click
// (a waveform ending mid-cycle, or changing at full level in one frame) shows
// as a step well past what the sound's own content moves by
u32 MaxStep(const std::vector<s16>& pcm, size_t firstFrame, size_t endFrame, size_t* where = nullptr)
{
    u32 max = 0;
    for (size_t frame = firstFrame; frame < endFrame; frame++)
    {
        size_t at = frame * MixChannels;
        if (at + MixChannels >= pcm.size())
        {
            break;
        }

        for (u32 channel = 0; channel < MixChannels; channel++)
        {
            s32 step = std::abs(static_cast<s32>(pcm[at + channel]) - static_cast<s32>(pcm[at - MixChannels + channel]));
            if (step > static_cast<s32>(max))
            {
                max = static_cast<u32>(step);
                if (where != nullptr)
                {
                    *where = frame;
                }
            }
        }
    }

    return max;
}

void Print(const char* what, const Stats& stats)
{
    std::printf("%-16s frames=%u  rms=%.5f (%.1f dBFS)  peak=%d (%.2f)  nonzero=%.1f%%  fullScale=%u maxStep=%.0f\n",
                what, stats.frames, stats.rms, stats.rms > 0.0 ? 20.0 * std::log10(stats.rms) : -999.0, stats.peak,
                stats.peak / 32768.0, 100.0 * stats.nonZero / (stats.frames ? stats.frames : 1), stats.fullScale,
                stats.maxStep);
}

// The mixer's own frame boundaries (MixFrames = 1024): a ring/pointer bug shows
// as a discontinuity every 1024 frames. Measured as the per-window RMS spread
// and the biggest jump straddling a boundary against the signal's normal step.
void BoundaryReport(const char* what, const std::vector<s16>& pcm)
{
    constexpr u32 Window = 1024;
    double minRms = 1e9;
    double maxRms = -1.0;
    u32 windows = 0;
    u32 minIndex = 0;
    double boundaryJump = 0.0;
    for (size_t start = 0; start + Window * MixChannels <= pcm.size(); start += Window * MixChannels)
    {
        long long sum = 0;
        for (size_t i = start; i < start + Window * MixChannels; i++)
        {
            sum += static_cast<long long>(pcm[i]) * pcm[i];
        }

        double rms = std::sqrt(static_cast<double>(sum) / (Window * MixChannels));
        if (rms < minRms) { minRms = rms; minIndex = windows; }
        if (rms > maxRms) maxRms = rms;
        windows++;
        if (start > 0)
        {
            double jump = std::abs(static_cast<double>(pcm[start]) - pcm[start - MixChannels]);
            if (jump > boundaryJump) boundaryJump = jump;
        }
    }

    std::printf("%-16s windows=%u  rms[min=%.0f@%u max=%.0f]  biggest boundary jump=%.0f\n", what, windows, minRms,
                minIndex, maxRms, boundaryJump);
}
}

int main()
{
    // A voice sound out of the English bank: its header's first entry, read
    // through the same LoadSoundBankSamples the stream layer calls.
    std::vector<u8> header = ReadFile("disc/CRASH6/ENGLISH.MH", 0, 2868);
    if (header.size() < 28)
    {
        std::printf("can't read disc/CRASH6/ENGLISH.MH (run from the repo root)\n");
        return 1;
    }

    // The first sound with samples in the bank (its size and offset)
    u32 count = Word(header, 0);
    u32 soundSize = 0;
    u32 soundOffset = 0;
    for (u32 i = 0; i < count; i++)
    {
        u32 at = 8 + i * 20;
        u32 size = Word(header, at + 4);
        if (size != 0)
        {
            soundSize = size;
            soundOffset = Word(header, at + 8);
            std::printf("bank sound %u: size=%u offset=%u\n", i, soundSize, soundOffset);
            break;
        }
    }

    std::vector<u8> sample = ReadFile("disc/CRASH6/ENGLISH.MB", soundOffset, soundSize);
    if (sample.empty())
    {
        std::printf("can't read the sound's samples\n");
        return 1;
    }

    std::printf("--- voice path (PlaySound of a real bank sound) ---\n");
    u32 at = SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
    std::printf("registered at %u\n", at);
    std::printf("SpuMemory %p\n", static_cast<void*>(SdlAudio::SpuMemory()));

    Platform::Audio::Reset();
    // Reload after Reset (Reset clears the table)
    at = SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
    s32 played = Platform::Audio::PlaySound(1, 0 /*voice 0, no group*/, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
    std::printf("PlaySound -> %d (0 ok, -1/-2 rejected)\n", played);
    // FindFreeVoice is the game's; the host's PlaySound returns 0. Check the sound is found.
    std::vector<s16> voicePcm(MixRate * MixChannels / 2); // 250 ms

    // A play runs its sample so many times and no more (PlaySound's count: 0 loops until it's let go). A sound whose data
    // has a loop point played once must still come to its end and give its voice back: one that looped past its count stayed
    // active for good, and the sound processor's 48 voices run out (every later sound is then dropped, with no sound left)
    std::printf("\n--- one-shot with a loop point (its voice must come back) ---\n");
    {
        // A PlayStation ADPCM block is 16 bytes and 28 samples (the mixer's adpcm.h)
        constexpr u32 BlockBytes = 16;
        constexpr u32 Blocks = 4;
        std::vector<u8> looped(Blocks * BlockBytes, 0);
        for (u32 block = 0; block < Blocks; block++)
        {
            looped[block * BlockBytes] = 0;
            // Loud nibbles (a square wave), the loop start on the first block and the end on the last: a whole-region loop
            for (u32 nibble = 0; nibble < (BlockBytes - 2) * 2; nibble++)
            {
                u8 value = 0xF;
                looped[block * BlockBytes + 2 + nibble / 2] |= nibble % 2 == 0 ? value : value << 4;
            }
        }

        // The block flags the decoder's adpcm.h names: 0x1 is the loop end, 0x4 the loop start (the SPU2's own bits)
        looped[1] = 0x4;
        looped[(Blocks - 1) * BlockBytes + 1] = 0x1;
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, looped.data(), static_cast<u32>(looped.size()));
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        // Four blocks are 112 samples: a tenth of a second is several times that
        std::vector<s16> afterLoop(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(afterLoop.data(), MixRate / 10);
        Print("loop point once", Measure(afterLoop));
        // The module lets the voice go at SD_VP_ADSR2's 0xD: 1 << (0x1F - 0xD)
        // a sample, about 170 ms for a full envelope
        std::vector<s16> afterRelease(MixRate / 5 * MixChannels);
        SdlAudio::MixStereo(afterRelease.data(), MixRate / 5);
        std::printf("voice 0 free after one pass and its release: %d (1 = the voice came back)\n",
                    Platform::Audio::IsVoiceFree(0));

        // The voice a finished sound left behind must take another sound: the release a finished sound leaves used to drain the
        // next one away before it made a sample (only the first sound of a session was audible, silence ever after)
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        std::vector<s16> again(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(again.data(), MixRate / 10);
        Print("same voice again", Measure(again));
        std::printf("a sound played on a voice that has finished one is heard: %s\n",
                    Measure(again).nonZero != 0 ? "yes" : "NO (it was drained by the old release)");

        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, looped.data(), static_cast<u32>(looped.size()));
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 0 /* until let go */);
        SdlAudio::MixStereo(afterLoop.data(), MixRate / 10);
        Print("loop point forever", Measure(afterLoop));
        std::printf("voice 0 free while looping until let go: %d (0 = still playing)\n", Platform::Audio::IsVoiceFree(0));
        Platform::Audio::ReleaseVoice(0, 0xD);
        // SD_VP_ADSR2's 0xD release is linear: 1 << (0x1F - 0xD) a sample,
        // about 170 ms for a full envelope
        SdlAudio::MixStereo(afterLoop.data(), MixRate / 10);
        std::printf("voice 0 still released after 100 ms: %d (1 = it is still fading, as the PS2's rate does)\n",
                    Platform::Audio::IsVoiceFree(0) == 0 ? 1 : 0);
        std::vector<s16> released(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(released.data(), MixRate / 10);
        std::printf("voice 0 free 200 ms after let go: %d (1 = the voice came back)\n", Platform::Audio::IsVoiceFree(0));

        // A one-block loop (loop start and loop end on the same block) is
        // still a loop: the SPU2's flags decide, not the block count
        std::vector<u8> single(BlockBytes, 0);
        for (u32 nibble = 0; nibble < (BlockBytes - 2) * 2; nibble++)
        {
            single[2 + nibble / 2] |= nibble % 2 == 0 ? 0xF : 0xF0;
        }
        single[1] = 0x5;
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, single.data(), static_cast<u32>(single.size()));
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 0);
        std::vector<s16> singlePcm(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(singlePcm.data(), MixRate / 10);
        Print("one-block loop", Measure(singlePcm));
        std::printf("one-block loop still playing: %d (0 = looping, not ended at once)\n",
                    Platform::Audio::IsVoiceFree(0));
        Platform::Audio::ReleaseVoice(0, 0xD);
        std::vector<s16> singleRelease(MixRate / 4 * MixChannels);
        SdlAudio::MixStereo(singleRelease.data(), MixRate / 4);
        std::printf("one-block loop voice free after let go: %d (1 = the voice came back)\n",
                    Platform::Audio::IsVoiceFree(0));
    }

    Platform::Audio::Reset();
    at = SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
    Platform::Audio::PlaySound(1, 0 /*voice 0, no group*/, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);

    // Silencing a voice must not click: the module ramps the volume to the one the game sets, where writing it at once ends the
    // waveform mid-cycle. The step between two frames says which (the bank region read here holds a short sound, so it is
    // silenced while it is still sounding)
    std::printf("\n--- silencing a voice mid-sound (the step must stay small) ---\n");
    {
        std::vector<s16> sounding(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(sounding.data(), MixRate / 10);
        Print("voice 100ms", Measure(sounding));

        Platform::Audio::SetVoiceVolume(0, 0, 0);
        std::vector<s16> faded(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(faded.data(), MixRate / 10);
        Print("silenced", Measure(faded));
        // Where the click would be: the seam between the last frame before the volume was set and the first after it. The
        // module's ramp keeps the step small (the sound carries on while it fades); an instant cut puts the sound's own last
        // sample there, which is heard as a click
        std::printf("step across the silence: %d (the sound's own last sample would be about as large)\n",
                    std::abs(static_cast<int>(sounding[sounding.size() - 2]) - static_cast<int>(faded[0])));
    }

    Platform::Audio::PlaySound(1, 0 /*voice 0, no group*/, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
    SdlAudio::MixStereo(voicePcm.data(), MixRate / 2);
    // Measure in 1024-frame windows too, to spot boundary clicks
    Print("voice 250ms", Measure(voicePcm));
    BoundaryReport("voice bounds", voicePcm);

    // The two places a step in the waveform is heard as a pop: the key-on
    // that starts a sound (or takes a voice over), and the moment it is over.
    // Both have to stay small next to what the sound's own frames move by
    std::printf("\n--- key-on, a re-key, and the sound's own end (the steps must stay small) ---\n");
    {
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        std::vector<s16> started(MixRate / 100 * MixChannels);
        SdlAudio::MixStereo(started.data(), MixRate / 100);
        Stats startStats = Measure(started);
        std::printf("key-on from silence: biggest step in its first 10 ms %u (that window's peak %d)\n",
                    MaxStep(started, 1, MixRate / 100), startStats.peak);
        // The key-on's own frames are the ones before the fade brings the
        // sound up; after that the steps are the sound's own content
        size_t where = 0;
        u32 stepNow = MaxStep(started, 1, MixRate / 100, &where);
        size_t at = where * MixChannels;
        std::printf(
            "                     biggest step at frame %zu (%zu us in): %d -> %d, over the key-on's fade "
            "(first 2 ms) %u, after it %u\n",
            where, where * 1000000 / MixRate, static_cast<s32>(started[at - MixChannels]),
            static_cast<s32>(started[at]), MaxStep(started, 1, 96), MaxStep(started, 96, MixRate / 100));
        (void)stepNow;
        std::printf("                     voice free after the 10 ms: %d (0 = still going)\n",
                    Platform::Audio::IsVoiceFree(0));

        // A second sound on the voice while the first is still sounding: the
        // module steals voices, and the new waveform arrives under the old
        // level in one frame unless the key-on fades it in over the top
        std::vector<s16> before(MixRate / 200 * MixChannels);
        SdlAudio::MixStereo(before.data(), MixRate / 200);
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        std::vector<s16> after(MixRate / 50 * MixChannels);
        SdlAudio::MixStereo(after.data(), MixRate / 50);
        s32 seam = std::abs(static_cast<s32>(after[0]) - static_cast<s32>(before[before.size() - MixChannels]));
        std::printf("re-key on a voice still sounding: step at the key-on %d\n", seam);

        // The sound played to its end: the voice fades out rather than stops
        // dead, and the frames around the end stay small. A play that runs out
        // in a couple of milliseconds is where every one-shot's end shows up
        constexpr u32 BlockBytes = 16;
        constexpr u32 Blocks = 4;
        std::vector<u8> shortSound(Blocks * BlockBytes, 0);
        for (u32 block = 0; block < Blocks; block++)
        {
            for (u32 nibble = 0; nibble < (BlockBytes - 2) * 2; nibble++)
            {
                u8 value = 0xF;
                shortSound[block * BlockBytes + 2 + nibble / 2] |= nibble % 2 == 0 ? value : value << 4;
            }
        }

        shortSound[1] = 0x4;
        shortSound[(Blocks - 1) * BlockBytes + 1] = 0x1;
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, shortSound.data(), static_cast<u32>(shortSound.size()));
        Platform::Audio::PlaySound(1, 0, 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        std::vector<s16> tail(MixRate / 4 * MixChannels);
        SdlAudio::MixStereo(tail.data(), MixRate / 4);
        s32 endStep = MaxStep(tail, 1, MixRate / 4);
        std::printf("sound's end: voice free %d after its release, biggest step around it %u\n",
                    Platform::Audio::IsVoiceFree(0), endStep);

        // Every voice at once, full scale: what a busy moment puts together.
        // Frames pinned at full scale are where the mix clips (heard as crackle)
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
        for (s32 voice = 0; voice < Platform::Audio::Voices; voice++)
        {
            Platform::Audio::PlaySound(1, static_cast<u32>(voice), 0x3FFF, 0x3FFF, 0x1000, 0xD, 0xD, 1);
        }

        std::vector<s16> busy(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(busy.data(), MixRate / 10);
        Stats busyStats = Measure(busy);
        Print("48 voices", busyStats);
        std::printf("48 voices at full scale: frames pinned at full scale %u\n", busyStats.fullScale);

        // The same moment with the volumes a busy stretch of play actually
        // sets: voices panned and scaled back, a dozen of them at once
        Platform::Audio::Reset();
        SdlAudio::LoadSoundBankSamples(1, sample.data(), static_cast<u32>(sample.size()));
        for (s32 voice = 0; voice < 12; voice++)
        {
            s16 level = static_cast<s16>(0x3FFF / 3);
            Platform::Audio::PlaySound(1, static_cast<u32>(voice), level, level, 0x1000, 0xD, 0xD, 1);
        }

        SdlAudio::MixStereo(busy.data(), MixRate / 10);
        Stats busyStats2 = Measure(busy);
        Print("12 voices", busyStats2);
        std::printf("12 voices at a third scale: frames pinned at full scale %u\n", busyStats2.fullScale);
    }

    Platform::Audio::Reset();
    std::printf("\n--- music path (a real track region) ---\n");
    std::vector<u8> musicHeader = ReadFile("disc/CRASH6/MUSIC.MH", 0, 2868);
    u32 tracks = Word(musicHeader, 0);
    u32 trackSize = 0;
    u32 trackOffset = 0;
    for (u32 i = 0; i < tracks; i++)
    {
        u32 entry = 8 + i * 20;
        u32 kind = Word(musicHeader, entry);
        u32 size = Word(musicHeader, entry + 4);
        if (kind == 1 && size != 0 && size < 4 * 1024 * 1024)
        {
            trackSize = size;
            trackOffset = Word(musicHeader, entry + 8);
            std::printf("music track %u: size=%u offset=%u\n", i, trackSize, trackOffset);
            break;
        }
    }

    g_Song = std::fopen("disc/CRASH6/MUSIC.MB", "rb");
    if (g_Song != nullptr)
    {
        Platform::Audio::ReadMusic(0, trackOffset, trackSize);
        s32 channel = Platform::Audio::StreamMusic(0, 0, 0 /*voice 0*/, 0x1000, false);
        std::printf("StreamMusic -> %d\n", channel);
        Platform::Audio::PrepareMusic(channel);
        std::printf("IsMusicReady -> %d\n", Platform::Audio::IsMusicReady(channel) ? 1 : 0);
        // The game's PlayMusic sets the player's volumes (SetMusicVolume)
        // before it starts the channel: the voice's registers are what the
        // key-on takes its volume from
        Platform::Audio::SetVoiceVolume(0, 0x3FFF, 0x3FFF);
        Platform::Audio::PlayMusic(channel);
        std::printf("IsMusicPlaying -> %d\n", Platform::Audio::IsMusicPlaying(channel) ? 1 : 0);
        // The music runs on the player's voice, so the game's SetMusicVolume
        // (SetVoiceVolume) and SetMusicPitch (SetVoicePitch) must reach it.
        // This track's first 0.4 s are loud, so keep these checks inside it.
        std::vector<s16> musicStart(MixRate / 10 * MixChannels);
        SdlAudio::MixStereo(musicStart.data(), MixRate / 10);
        Print("music 100ms", Measure(musicStart));

        Platform::Audio::SetVoiceVolume(0, 0, 0);
        std::vector<s16> settle(MixRate / 50 * MixChannels);
        SdlAudio::MixStereo(settle.data(), MixRate / 50); // the volume's ramp
        std::vector<s16> muted(MixRate / 20 * MixChannels);
        SdlAudio::MixStereo(muted.data(), MixRate / 20);
        Stats mutedStats = Measure(muted);
        Print("music muted", mutedStats);
        std::printf("music follows SetVoiceVolume(0, 0): %s\n", mutedStats.rms < 0.001 ? "yes" : "NO");
        std::printf("music voice after the mute: free=%d playing=%d\n", Platform::Audio::IsVoiceFree(0),
                    Platform::Audio::IsMusicPlaying(0) ? 1 : 0);

        Platform::Audio::SetVoiceVolume(0, 0x3FFF, 0x3FFF);
        SdlAudio::MixStereo(settle.data(), MixRate / 50); // the volume's ramp
        std::vector<s16> restored(MixRate / 20 * MixChannels);
        SdlAudio::MixStereo(restored.data(), MixRate / 20);
        Stats restoredStats = Measure(restored);
        Print("music restored", restoredStats);
        std::printf("music comes back with SetVoiceVolume(0, 0x3FFF): %s\n", restoredStats.rms > 0.01 ? "yes" : "NO");

        Platform::Audio::SetVoicePitch(0, 0x0800);
        std::vector<s16> pitched(MixRate / 20 * MixChannels);
        SdlAudio::MixStereo(pitched.data(), MixRate / 20);
        Print("music pitch 0x800", Measure(pitched));
        Platform::Audio::SetVoicePitch(0, 0x1000);

        std::vector<s16> musicPcm(MixRate * MixChannels / 2);
        SdlAudio::MixStereo(musicPcm.data(), MixRate / 2);
        Print("music 250ms", Measure(musicPcm));
        BoundaryReport("music bounds", musicPcm);
        std::fclose(g_Song);
    }
    else
    {
        std::printf("can't open disc/CRASH6/MUSIC.MB\n");
    }

    // Silence with everything stopped: the mixer must answer zeros, not garbage
    Platform::Audio::StopMusic(0);
    Platform::Audio::Reset();
    std::vector<s16> silence(MixRate / 10 * MixChannels);
    SdlAudio::MixStereo(silence.data(), MixRate / 10);
    Print("silence", Measure(silence));

    return 0;
}
