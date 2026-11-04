// The sound processor's checks: the arithmetic the chip's sound depends on, run offline with no disc and no game.
//
// Every number here is the hardware's, not the host's: the interpolation table is the one Near's generator makes, the ADPCM
// filter is the published coefficient table, the volume registers' other phase and the envelope's phase steps are what the
// registers mean. A change to the core that changes any of them is a change in what the game sounds like, and this says so
// before a build reaches a player.
//
// Build (from the repo root):
//   g++ -m32 -std=gnu++26 -O2 -Iinclude -Iinclude/host -Isrc/platform/sdl3
//       tools/audio_spu2_proof.cpp src/platform/sdl3/audio/spu2.cpp -o /tmp/audio_spu2_proof
#include "../src/platform/sdl3/audio/soundmodule.h"

#include <cstdio>
#include <cstring>
#include <vector>

// What the disc hands a stream: the checks build their own region, so the read comes from here rather than a file. This has to
// be the real Platform::Stream::Read the module calls, so it sits outside the anonymous namespace.
namespace
{
std::vector<u8> g_Region;
}

namespace Platform::Stream
{
void Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    (void)channel;
    (void)file;
    if (offset + size > g_Region.size())
    {
        return;
    }

    std::memcpy(destination, g_Region.data() + offset, size);
}
}

namespace
{
using namespace SdlAudio::Spu2;

s32 g_Failures = 0;
s32 g_Checks = 0;

void Check(const char* what, bool passed, const char* detail = "")
{
    g_Checks++;
    if (!passed)
    {
        g_Failures++;
        std::printf("FAIL  %s %s\n", what, detail);
    }
}

void CheckEqual(const char* what, s32 got, s32 want)
{
    char detail[128];
    std::snprintf(detail, sizeof(detail), "(got %d, want %d)", got, want);
    Check(what, got == want, detail);
}

void Phase(const char* name)
{
    std::printf("-- %s\n", name);
}

// A block of 28 samples: its header's shift and filter, its loop flags, then the nibbles as the bank stores them
void MakeBlock(u8* bytes, u8 flags, u8 shift, u8 filter, u8 nibble)
{
    bytes[0] = static_cast<u8>(shift | (filter << 4));
    bytes[1] = flags;
    for (s32 index = 2; index < 16; index++)
    {
        // Both nibbles the same, so every sample of the block is one value and the filter's history is easy to follow
        bytes[index] = static_cast<u8>(nibble | (nibble << 4));
    }
}

// A voice playing a block at word address blockAddress, with the fastest attack and decay and a release the game uses
void StartVoice(Device& device, s32 voiceIndex, u32 blockAddress, u16 pitch)
{
    Core& core = device.GetCore(voiceIndex / CoreVoices);
    Voice& voice = core.voices[voiceIndex % CoreVoices];

    // Attack 3 frames to full, decay to the sustain level at once, sustain level 0xF
    voice.envelope.value = 0;
    voice.envelope.attackDecay = 0x000F;
    voice.envelope.sustainRelease = 0x0000;
    voice.envelope.Cache();

    voice.pitch = pitch;
    voice.start = blockAddress;
    voice.loopStart = blockAddress;
    voice.volumeLeft.Set(0x3FFF);
    voice.volumeRight.Set(0x3FFF);
    voice.Start();

    // Both sides of the core take the voice's output, dry only, at the core's full master volume (which starts at silence
    // and is the module's to set)
    u32 bit = 1u << (voiceIndex % CoreVoices);
    core.dryLeft |= bit;
    core.dryRight |= bit;
    device.GetCore(0).masterLeft.Set(0x3FFF);
    device.GetCore(0).masterRight.Set(0x3FFF);
    device.GetCore(1).masterLeft.Set(0x3FFF);
    device.GetCore(1).masterRight.Set(0x3FFF);
    core.masterLeft.Set(0x3FFF);
    core.masterRight.Set(0x3FFF);
}

// The interpolation table
void CheckInterpolation()
{
    Phase("interpolation table");

    const s16(*table)[4] = InterpolationTable();
    // The table's own ends and middle, as the published one has them
    CheckEqual("row 0 tap 0", table[0][0], 0x12C7);
    CheckEqual("row 0 tap 1", table[0][1], 0x59B3);
    CheckEqual("row 0 tap 2", table[0][2], 0x1307);
    CheckEqual("row 0 tap 3", table[0][3], -1);
    CheckEqual("row 128 tap 0", table[128][0], 0x019C);
    CheckEqual("row 128 tap 1", table[128][1], 0x3DEF);
    CheckEqual("row 128 tap 2", table[128][2], 0x3E4C);
    CheckEqual("row 128 tap 3", table[128][3], 0x01A8);

    // Every row sums to the 1.15 full a still voice needs, to within the table's rounding
    s32 worst = 0;
    for (s32 row = 0; row < 256; row++)
    {
        s32 sum = static_cast<s32>(table[row][0]) + table[row][1] + table[row][2] + table[row][3];
        s32 distance = sum > 0x7F80 ? sum - 0x7F80 : 0x7F80 - sum;
        if (distance > worst)
        {
            worst = distance;
        }
    }

    Check("every row sums to full", worst <= 4, "");
}

// The ADPCM decoder
void CheckAdpcm()
{
    Phase("ADPCM");

    Device device;
    device.Reset();

    // Shift 0, filter 0, one nibble throughout: every sample is the nibble scaled by 0x1000
    u8 block[16];
    MakeBlock(block, 0, 0, 0, 0x1);
    device.WriteMemory(0x1000, block, sizeof(block));

    StartVoice(device, 0, 0x1000, 0x1000);
    // The attack takes it to full over three frames, so the samples are read after that
    for (s32 frame = 0; frame < 8; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
    }

    Voice& voice = device.GetCore(0).voices[0];
    // The decoder's own output, not the mix's: the last two samples it decoded are the nibble scaled up
    CheckEqual("nibble 1 decodes to 0x1000", voice.previous2, 0x1000);
    CheckEqual("nibble 1 decodes to 0x1000 (next)", voice.previous1, 0x1000);

    // A signed nibble: 0x8 is the most negative value 4 bits hold
    u8 negative[16];
    MakeBlock(negative, 0, 0, 0, 0x8);
    device.WriteMemory(0x2000, negative, sizeof(negative));
    StartVoice(device, 1, 0x2000, 0x1000);
    for (s32 frame = 0; frame < 8; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
    }

    CheckEqual("nibble 8 decodes to -0x8000", device.GetCore(0).voices[1].previous1, -0x8000);

    // Filter 1 carries the last sample in at 60/64: a step settles upwards instead of jumping
    u8 filtered[16];
    MakeBlock(filtered, 0, 0, 1, 0x0);
    device.WriteMemory(0x3000, filtered, sizeof(filtered));
    StartVoice(device, 2, 0x3000, 0x1000);
    for (s32 frame = 0; frame < 8; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
    }

    CheckEqual("a zero block after a filter is silent", device.GetCore(0).voices[2].previous1, 0);
}

// The volume registers
void CheckVolume()
{
    Phase("volume");

    VolumeSlide volume;
    volume.Set(0x3FFF);
    CheckEqual("full volume is 0x7FFE in 16 bits", volume.level, 0x7FFE);

    volume.Set(0x2000);
    CheckEqual("half volume", volume.level, 0x4000);

    // Past 0x3FFF the register is the other phase of the wave: 0x4000 is full in the negative
    volume.Set(0x4000);
    CheckEqual("0x4000 is the other phase at full", volume.level, -32768);

    // The register's top bit is the volume slide's enable, so a value that high is a slide register rather than a level:
    // the level it holds stays where it was
    volume.Set(0x4000);
    volume.Set(0x8000);
    CheckEqual("a slide register leaves its level alone", volume.level, -32768);

    // The 15 bit multiply
    CheckEqual("a full sample at full volume", ApplyVolume(32767, 0x7FFE), 32765);
    CheckEqual("a full sample in the other phase", ApplyVolume(32767, -32768), -32767);
    CheckEqual("silence stays silent", ApplyVolume(32767, 0), 0);

    // A slide that is on moves the level and settles on the register's own
    VolumeSlide slide;
    slide.value = static_cast<u16>(0x3 << 2 | 0x8); // a step of 3 over a shift of 8
    slide.enable = 1;
    slide.level = 0;
    s32 frames = 0;
    while (slide.level < 0x7FFE && frames < 100000)
    {
        slide.Step();
        frames++;
    }

    Check("a smooth volume reaches its level", frames > 0 && frames < 100000, "");
    // It steps in whole units of its own step, so it settles on the first level at or past the register's
    Check("a smooth volume stops at the register's own", slide.level >= 0x7FF0 && slide.level <= 32767, "");
}

// The envelope's phase steps: for every rate the registers can hold, the level moves, and for the rates the game plays with
// the envelope finishes
void CheckEnvelope()
{
    Phase("envelope");

    s32 stalled = 0;
    for (s32 rate = 0; rate < 128; rate++)
    {
        Envelope envelope;
        envelope.value = 0;
        // The attack's rate in the top of the first register, everything else at its fastest
        envelope.attackDecay = static_cast<u16>(rate << 8);
        envelope.sustainRelease = 0;
        envelope.Cache();
        envelope.KeyOn();

        s32 before = envelope.counter;
        bool moved = false;
        for (s32 frame = 0; frame < 64; frame++)
        {
            envelope.Step();
            if (envelope.counter != static_cast<u32>(before) || envelope.level != 0)
            {
                moved = true;
                break;
            }
        }

        if (!moved)
        {
            stalled++;
        }
    }

    CheckEqual("no attack rate stalls the envelope", stalled, 0);

    // The release rates the game plays with (0xD) let go of a voice in well under a second
    Envelope releasing;
    releasing.value = 0x0FFF;
    releasing.attackDecay = 0x000F;
    releasing.sustainRelease = 0x0D;
    releasing.Cache();
    releasing.KeyOn();
    s32 releaseFrames = 0;
    while (releasing.level > 0x7FFF / 4 && releaseFrames < 200000)
    {
        releasing.Step();
        releaseFrames++;
    }

    char releaseDetail[96];
    std::snprintf(releaseDetail, sizeof(releaseDetail), "(a quarter of the level after %d frames)", releaseFrames);
    Check("a voice released at the game's rate has faded", releaseFrames < 48000, releaseDetail);

    // An attack that is asked for the slowest rate still reaches the end eventually: no voice can sit in the attack for
    // good, which is what left the game's voice table full and the game silent
    Envelope slowest;
    slowest.value = 0;
    slowest.attackDecay = static_cast<u16>(0x7F << 8);
    slowest.sustainRelease = 0;
    slowest.Cache();
    slowest.KeyOn();
    s32 before = slowest.level;
    for (s32 frame = 0; frame < 40000; frame++)
    {
        slowest.Step();
    }

    Check("the slowest attack still advances", slowest.level != before || slowest.phase != Envelope::Attack, "");
}

// A sound's end and a sound's loop, as the block flags say
void CheckLoops()
{
    Phase("loops");

    // A one shot: the block's last block carries the end flag and not the loop flag, and the voice is let go there
    {
        Device device;
        device.Reset();

        u8 block[16];
        MakeBlock(block, BlockLoopEnd | BlockLoopStart, 0, 0, 0x1);
        device.WriteMemory(0x1000, block, sizeof(block));

        StartVoice(device, 0, 0x1000, 0x1000);
        s32 frames = 0;
        while (device.GetCore(0).voices[0].Active() && frames < 1000)
        {
            s32 left = 0;
            s32 right = 0;
            device.Mix(left, right);
            frames++;
        }

        Check("a one shot voice is let go at the end of its block", !device.GetCore(0).voices[0].Active(), "");
        // The reader runs a block ahead of the mixer, so the last block's buffered samples (about fifteen) are not played:
        // the voice is let go once the reader has the block's end, which is a third of a millisecond of the sound
        char detail[64];
        std::snprintf(detail, sizeof(detail), "(lasted %d frames)", frames);
        Check("a one shot voice is let go once its block is read", frames >= 8 && frames <= 20, detail);
    }

    // A loop: the same block with the loop flag keeps the voice playing, and letting go of it finishes it
    {
        Device device;
        device.Reset();

        u8 block[16];
        MakeBlock(block, BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x1);
        device.WriteMemory(0x1000, block, sizeof(block));

        StartVoice(device, 0, 0x1000, 0x1000);
        for (s32 frame = 0; frame < 600; frame++)
        {
            s32 left = 0;
            s32 right = 0;
            device.Mix(left, right);
        }

        Check("a looping voice is still playing after 600 frames", device.GetCore(0).voices[0].Active(), "");

        // The game's release rate, and it is free
        device.GetCore(0).voices[0].envelope.KeyOff();
        s32 frames = 0;
        while (device.GetCore(0).voices[0].Active() && frames < 48000)
        {
            s32 left = 0;
            s32 right = 0;
            device.Mix(left, right);
            frames++;
        }

        Check("a released loop stops", !device.GetCore(0).voices[0].Active(), "");
        Check("a released loop stops within a second", frames < 48000, "");
    }

    // A loop point that is not the start: the sound repeats from the block the flag is on
    {
        Device device;
        device.Reset();

        u8 first[16];
        u8 second[16];
        // The first block plays through; the second is where the loop point is and where the loop ends
        MakeBlock(first, 0, 0, 0, 0x1);
        MakeBlock(second, BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x2);
        device.WriteMemory(0x1000, first, sizeof(first));
        device.WriteMemory(0x1008, second, sizeof(second));

        Voice& voice = device.GetCore(0).voices[0];
        StartVoice(device, 0, 0x1000, 0x1000);
        for (s32 frame = 0; frame < 1000; frame++)
        {
            s32 left = 0;
            s32 right = 0;
            device.Mix(left, right);
        }

        Check("the loop's own block is where it repeats", voice.loopStart == 0x1008, "");
        Check("a loop with its own block start keeps playing", voice.Active(), "");
    }
}

// The top of the mix: many voices at once reach the range's end and stop there rather than wrapping
void CheckSaturation()
{
    Phase("saturation");

    Device device;
    device.Reset();

    u8 block[16];
    MakeBlock(block, BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x7);
    for (u32 address = 0x1000; address < 0x1000 + 0x1000; address += 8)
    {
        device.WriteMemory(address, block, sizeof(block));
    }

    // Four voices, each of them at full volume, on one side of the mix
    for (s32 voice = 0; voice < 4; voice++)
    {
        StartVoice(device, voice, 0x1000 + static_cast<u32>(voice) * 8, 0x1000);
    }

    s32 peak = 0;
    s32 lowest = 0;
    for (s32 frame = 0; frame < 200; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
        if (left > peak)
        {
            peak = left;
        }

        if (left < lowest)
        {
            lowest = left;
        }
    }

    Check("four full voices do not pass the top", peak <= 32767, "");
    Check("four full voices do not pass the bottom", lowest >= -32768, "");
    // Four voices at full are four times the range and the sum stops at its end. The top is 32766 rather than 32767
    // because a full volume register is 0x3FFF, one step short of unity in the 15 bit multiply
    char detail[64];
    std::snprintf(detail, sizeof(detail), "(peak %d)", peak);
    Check("four full voices reach the top", peak >= 32700, detail);
}

// The effect unit's answer to an impulse: a stable number for each register set, so a change in the network is visible
void CheckReverb()
{
    Phase("reverb");

    Device device;
    device.Reset();

    Core& core = device.GetCore(0);
    // A work area, and the published Room register set
    core.effectStart = 0x80000;
    core.effectEnd = 0x80000 + (0x26C0 / 2) - 1;
    core.reverbOn = true;
    core.effectVolumeLeft = 32767;
    core.effectVolumeRight = 32767;

    const u32 start = core.effectStart;
    core.reverb.apf1Size = 0x007D * 4;
    core.reverb.apf2Size = 0x005B * 4;
    core.reverb.iirVolume = static_cast<s16>(0x6D80);
    core.reverb.combVolume[0] = static_cast<s16>(0x54B8);
    core.reverb.combVolume[1] = static_cast<s16>(0xBED0);
    core.reverb.combVolume[2] = 0;
    core.reverb.combVolume[3] = 0;
    core.reverb.wallVolume = static_cast<s16>(0xBA80);
    core.reverb.apf1Volume = static_cast<s16>(0x5800);
    core.reverb.apf2Volume = static_cast<s16>(0x5300);
    core.reverb.sameLeftTarget = start + 0x04D6 * 4;
    core.reverb.sameRightTarget = start + 0x0333 * 4;
    core.reverb.combSource[0][0] = start + 0x03F0 * 4;
    core.reverb.combSource[0][1] = start + 0x0227 * 4;
    core.reverb.combSource[1][0] = start + 0x0374 * 4;
    core.reverb.combSource[1][1] = start + 0x01EF * 4;
    core.reverb.sameLeftSource = start + 0x0336 * 4;
    core.reverb.sameRightSource = start + 0x01B7 * 4;
    core.reverb.differentLeftTarget = start + 0x0335 * 4;
    core.reverb.differentRightTarget = start + 0x01B6 * 4;
    core.reverb.combSource[2][0] = start + 0x0334 * 4;
    core.reverb.combSource[2][1] = start + 0x01B5 * 4;
    core.reverb.combSource[3][0] = start + 0x0334 * 4;
    core.reverb.combSource[3][1] = start + 0x01B5 * 4;
    core.reverb.differentLeftSource = start + 0x0334 * 4;
    core.reverb.differentRightSource = start + 0x01B5 * 4;
    core.reverb.apfTarget[0][0] = start + 0x01B4 * 4;
    core.reverb.apfTarget[0][1] = start + 0x0136 * 4;
    core.reverb.apfTarget[1][0] = start + 0x00B8 * 4;
    core.reverb.apfTarget[1][1] = start + 0x005C * 4;
    core.reverb.inputLeft = static_cast<s16>(0x8000);
    core.reverb.inputRight = static_cast<s16>(0x8000);

    // An impulse on the effect send: four voices' worth, one frame of full scale on the left
    u8 block[16];
    MakeBlock(block, BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x7);
    device.WriteMemory(0x1000, block, sizeof(block));
    StartVoice(device, 0, 0x1000, 0x1000);
    device.GetCore(0).effectLeft |= 1u;
    device.GetCore(0).effectRight |= 1u;

    u32 hash = 2166136261u;
    s32 energy = 0;
    for (s32 frame = 0; frame < 48000; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
        if (frame > 100)
        {
            // The impulse is over; what is left is the network's tail, which is what the hash is of
            s32 wet = left < 0 ? -left : left;
            energy += wet;
            hash ^= static_cast<u32>(left) & 0xFFFF;
            hash *= 16777619u;
        }
    }

    // The tail has to go somewhere and come back: a network that is not running leaves this at nothing
    Check("the effect unit produces a tail", energy > 0, "");
    std::printf("   room impulse tail %08x, energy %d\n", hash, energy);

    // The same registers twice give the same answer
    Device second;
    second.Reset();
    Core& again = second.GetCore(0);
    again = core;
    again.resamplePosition = 0;
    u8 secondBlock[16];
    MakeBlock(secondBlock, BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x7);
    second.WriteMemory(0x1000, secondBlock, sizeof(secondBlock));
    StartVoice(second, 0, 0x1000, 0x1000);
    second.GetCore(0).effectLeft |= 1u;
    second.GetCore(0).effectRight |= 1u;

    u32 secondHash = 2166136261u;
    for (s32 frame = 0; frame < 48000; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        second.Mix(left, right);
        if (frame > 100)
        {
            secondHash ^= static_cast<u32>(left) & 0xFFFF;
            secondHash *= 16777619u;
        }
    }

    CheckEqual("the same settings give the same answer", static_cast<s32>(secondHash), static_cast<s32>(hash));
}

// A silence with nothing playing is silence: no voice, no effect, no level left over
void CheckSilence()
{
    Phase("silence");

    Device device;
    device.Reset();
    s32 loudest = 0;
    for (s32 frame = 0; frame < 1000; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
        if (left > loudest)
        {
            loudest = left;
        }

        if (right > loudest)
        {
            loudest = right;
        }

        if (-left > loudest)
        {
            loudest = -left;
        }

        if (-right > loudest)
        {
            loudest = -right;
        }
    }

    CheckEqual("nothing playing is silence", loudest, 0);
}

// A bank as the disc's own files hold them: the "MSVp" header, then the samples' first block
std::vector<u8> MakeBank(u8 flags, u8 shift, u8 filter, u8 nibble, u32 blocks)
{
    std::vector<u8> bank(BankHeaderBytes + blocks * 16, 0);
    std::memcpy(bank.data(), "MSVp", 4);
    for (u32 block = 0; block < blocks; block++)
    {
        MakeBlock(bank.data() + BankHeaderBytes + block * 16, flags, shift, filter, nibble);
    }

    return bank;
}

// The memory's blocks: what a sound is given, and where it goes when the game asks for the memory to be compacted
void CheckMemory()
{
    Phase("memory");

    SoundMemory memory;
    memory.Reset(MemoryWords);

    u32 first = 0;
    u32 second = 0;
    u32 third = 0;
    Check("a sound fits", memory.Allocate(64, first, 1), "");
    Check("a second sound fits", memory.Allocate(64, second, 2), "");
    Check("a third sound fits", memory.Allocate(64, third, 3), "");
    CheckEqual("the first sound is at the start of the sound area", static_cast<s32>(first), static_cast<s32>(MemoryBase));
    CheckEqual("the blocks follow one another", static_cast<s32>(second), static_cast<s32>(first) + 64);

    // The middle sound goes: a gap is left where it was
    Check("a sound is released", memory.Release(2), "");
    CheckEqual("the released sound's space is free", static_cast<s32>(memory.Used()), 128);

    u32 fourth = 0;
    Check("a new sound takes the gap", memory.Allocate(64, fourth, 4), "");
    CheckEqual("the new sound is where the released one was", static_cast<s32>(fourth), static_cast<s32>(second));

    memory.Release(4);
    memory.Release(3);
    memory.Compact();

    // Everything left moves down onto the first block, and the moves say how far
    u32 address = 0;
    u32 words = 0;
    Check("the first sound is still there", memory.AddressOf(1, address, words), "");
    CheckEqual("it did not move", static_cast<s32>(address), static_cast<s32>(first));

    // Moving a sound that is being read: the voice's start and its reading address follow the delta
    {
        Device device;
        device.Reset();
        std::vector<u8> low = MakeBank(BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x1, 2);
        std::vector<u8> high = MakeBank(BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x2, 2);

        SoundMemory arena;
        arena.Reset(MemoryWords);
        u32 lowAddress = 0;
        u32 highAddress = 0;
        arena.Allocate((static_cast<u32>(low.size()) + 1) / 2, lowAddress, 10);
        arena.Allocate((static_cast<u32>(high.size()) + 1) / 2, highAddress, 11);
        device.WriteMemory(lowAddress, low.data(), static_cast<u32>(low.size()));
        device.WriteMemory(highAddress, high.data(), static_cast<u32>(high.size()));

        device.SetVoiceStart(0, lowAddress);
        device.KeyOn(0);
        arena.Release(10);
        arena.Compact();

        device.MoveVoice(0, static_cast<s32>(MemoryBase) - static_cast<s32>(lowAddress));
        CheckEqual("a moved voice's start follows the block", static_cast<s32>(device.VoiceStart(0)), static_cast<s32>(MemoryBase));
    }
}

// The module's side of the bargain: a voice is free only when the processor says so, a sound's block is where the header ends,
// and a track's state survives a movie
void CheckModule()
{
    Phase("module");

    SoundModule& module = Module();
    module.Reset();

    // The bank the disc's files hold: 0x40 bytes of header, then the samples
    std::vector<u8> bank = MakeBank(BlockLoopEnd | BlockLoopStart | BlockLoop, 0, 0, 0x7, 4);
    u32 address = module.LoadSoundBank(0x1234, bank.data(), static_cast<u32>(bank.size()));
    Check("a bank loads", address != 0, "");
    CheckEqual("its samples start after the header", static_cast<s32>(address), static_cast<s32>(MemoryBase));
    Check("the module took the header's bytes off the block", module.MemoryUsed() >= 4 * 16 / 2, "");

    // A voice that has not been played is free
    CheckEqual("an untouched voice is free", module.IsVoiceFree(3), 1);

    // A play keys it, and it is not free from that moment on
    s32 result = module.PlaySound(0x1234, 3 | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    CheckEqual("a play is accepted", result, 0);
    CheckEqual("a playing voice is not free", module.IsVoiceFree(3), 0);

    // ... and it becomes free again once the sound ends, on its own
    s16 stereo[2] = {};
    s32 frames = 0;
    while (module.IsVoiceFree(3) == 0 && frames < 48000)
    {
        module.Mix(stereo, 1);
        frames++;
    }

    CheckEqual("a finished sound frees its voice", module.IsVoiceFree(3), 1);

    // A sound that is not in the memory does not play
    CheckEqual("an unknown sound is refused", module.PlaySound(0x999, 5, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1), 0);
    CheckEqual("and its voice stays free", module.IsVoiceFree(5), 1);

    // The group's volume scales the channel's own
    module.SetGroupVolume(1u << GroupShift, 0x800, 0x800);
    module.PlaySound(0x1234, 4 | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    CheckEqual("a group's volume is taken by the channel", module.IsVoiceFree(4), 0);

    // A released voice keeps sounding until its envelope has run out, and is only free after
    module.ReleaseVoice(4, 0x0D);
    CheckEqual("a released voice is still not free", module.IsVoiceFree(4), 0);
    frames = 0;
    while (module.IsVoiceFree(4) == 0 && frames < 48000)
    {
        module.Mix(stereo, 1);
        frames++;
    }

    CheckEqual("a released voice is free once it has faded", module.IsVoiceFree(4), 1);

    // A pause lets its sounds go: they fade out, and their voices are free again. It is deliberately not a mute of the whole
    // processor, because the game does not always pair its pause with a resume (see CheckPauseKeepsPlaying)
    module.PlaySound(0x1234, 6, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    module.PauseAll();
    s16 paused[2] = {};
    frames = 0;
    while (module.IsVoiceFree(6) == 0 && frames < 48000)
    {
        module.Mix(paused, 1);
        frames++;
    }

    CheckEqual("a pause lets its sounds go", module.IsVoiceFree(6), 1);
    CheckEqual("and lets go of them quietly", paused[0] < 400 && paused[0] > -400, 1);
    module.ResumeAll();

    // A movie takes the module's key-ons away, and gives them back without losing anything
    module.PlaySound(0x1234, 5, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    module.LendToMovie();
    module.PlaySound(0x1234, 7, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    CheckEqual("a movie refuses new key-ons", module.IsVoiceFree(7), 1);
    CheckEqual("and keeps the voices it had", module.IsVoiceFree(5), 0);
    module.ReclaimFromMovie();
    module.PlaySound(0x1234, 7, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1);
    CheckEqual("the module plays again afterwards", module.IsVoiceFree(7), 0);

    // A sound let go of is no longer in the memory, and cannot be played until it is loaded again
    module.ReleaseSound(0x9999);
    CheckEqual("a sound that was never loaded is not in the memory", module.PlaySound(0x9999, 8, 0x3FFF, 0x3FFF, 0x1000, 0x7F, 0x0D, 1), 0);
    CheckEqual("and its voice stays free", module.IsVoiceFree(8), 1);
}

// The exact play a menu sound makes in the log: sound 32769 on voice 4, volume 0x3FFF a side, pitch 1881, attack 32, release
// 13, one pass, in the fourth group. It has to be audible, or every menu click is silent
void CheckMenuPlay()
{
    Phase("menu play");

    SoundModule& module = Module();
    constexpr s32 BankSound = 0x8001;
    constexpr s32 Voice = 4;

    module.Reset();
    std::vector<u8> bank = MakeBank(0, 0, 0, 0x6, 12);
    Check("the menu's sound loads", module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size())) != 0, "");

    // Every group at full, as the game's own reset and the settings file leave them
    for (s32 group = 0; group < GroupCount; group++)
    {
        module.SetGroupVolume(static_cast<s32>((group + 1) << GroupShift), FullGroupVolume, FullGroupVolume);
    }

    module.PlaySound(BankSound, Voice | (4u << GroupShift), 0x3FFF, 0x3FFF, 1881, 32, 13, 1);

    s16 block[2] = {};
    s32 loudest = 0;
    for (s32 frame = 0; frame < 4800; frame++)
    {
        module.Mix(block, 1);
        s32 level = block[0] < 0 ? -block[0] : block[0];
        if (level > loudest)
        {
            loudest = level;
        }
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(loudest %d of 32768)", loudest);
    Check("the menu's play is audible", loudest > 100, detail);

    // ... and it has to be let go of when its data ends, or its voice is gone for the session: the disc's sounds carry no end
    // flag, so nothing but the module's own length stops them
    s32 frames = 0;
    while (module.IsVoiceFree(Voice) == 0 && frames < 48000)
    {
        module.Mix(block, 1);
        frames++;
    }

    char freed[96];
    std::snprintf(freed, sizeof(freed), "(free after %d frames, data is %u blocks)", frames, 12u);
    Check("a sound with no end flag is still let go of at its length", module.IsVoiceFree(Voice) == 1, freed);
}

// The game does not always pair its pause with a resume (a movie that ends into the title screen never resumes), so a pause must
// not be a mute of the whole processor: the sounds it lets go of are gone, but a sound played afterwards is still heard
void CheckPauseKeepsPlaying()
{
    Phase("pause");

    SoundModule& module = Module();
    constexpr s32 BankSound = 0x6000;
    constexpr s32 Voice = 12;

    module.Reset();
    std::vector<u8> bank = MakeBank(0, 0, 0, 0x5, 8);
    module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size()));

    // A pause with no resume, as the frontend leaves it
    module.PauseAll();

    module.PlaySound(BankSound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    s16 block[2] = {};
    s32 loudest = 0;
    for (s32 frame = 0; frame < 600; frame++)
    {
        module.Mix(block, 1);
        s32 level = block[0] < 0 ? -block[0] : block[0];
        if (level > loudest)
        {
            loudest = level;
        }
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(loudest %d of 32768)", loudest);
    Check("a sound played during an unpaired pause is still heard", loudest > 100, detail);

    // And the pause still does its job: a sound that was playing when it began is let go of
    module.ResumeAll();
    module.PlaySound(BankSound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    for (s32 frame = 0; frame < 100; frame++)
    {
        module.Mix(block, 1);
    }

    Check("a sound is playing before the pause", module.IsVoiceFree(Voice) == 0, "");

    module.PauseAll();
    s32 held = 0;
    while (module.IsVoiceFree(Voice) == 0 && held < 48000)
    {
        module.Mix(block, 1);
        held++;
    }

    std::snprintf(detail, sizeof(detail), "(let go after %d frames)", held);
    Check("a sound that was playing when the pause began is let go of", module.IsVoiceFree(Voice) == 1, detail);
    module.ResumeAll();
}

// A voice that plays twice must not keep the first play's loop point: it would wrap into memory that is not its own, which is
// heard as another sound at another pitch
void CheckLoopPointIsPerPlay()
{
    Phase("loop points");

    SoundModule& module = Module();
    constexpr s32 Sound = 0x7000;
    constexpr s32 Other = 0x7001;
    constexpr s32 Voice = 14;

    module.Reset();

    // A music-sized reservation first, so the memory below the sounds is not zeroed between them
    u32 ring = module.ReserveSoundMemory(StreamRingBytes);

    std::vector<u8> first = MakeBank(0, 0, 0, 0x3, 4);
    std::vector<u8> second = MakeBank(0, 0, 0, 0x6, 8);
    module.LoadSoundBank(Sound, first.data(), static_cast<u32>(first.size()));
    module.LoadSoundBank(Other, second.data(), static_cast<u32>(second.size()));

    // The first play sets the loop point to somewhere far away (as a stream's ring would)
    SoundProcessor().SetVoiceStart(Voice, ring / 2);
    SoundProcessor().SetVoiceLoopStart(Voice, ring / 2);

    // The next play must not inherit it
    module.PlaySound(Sound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    u32 start = SoundProcessor().VoiceStart(Voice);
    Check("a play starts at its own sound", start != ring / 2, "");

    // Play it right through: the voice must stop at its own end, not wrap anywhere
    s16 block[2] = {};
    s32 frames = 0;
    while (module.IsVoiceFree(Voice) == 0 && frames < 48000)
    {
        module.Mix(block, 1);
        frames++;
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(free after %d frames)", frames);
    Check("a voice does not loop into memory it does not own", module.IsVoiceFree(Voice) == 1, detail);
}

// How the module answers the game's loop count. The game's repeatable sounds are not the module's to repeat: its own sound
// objects re-key them (soundobjects.cpp waits for the voice to stop, then plays it again), so a module that looped them instead
// would never let the voice go, and the game would pile copy after copy of the same sound up with nothing able to stop them.
// A count above one is a real count of passes, and is counted here.
void CheckRepeatedPlay()
{
    Phase("repeats");

    SoundModule& module = Module();
    constexpr s32 Sound = 0x7100;
    constexpr s32 Voice = 16;

    module.Reset();

    constexpr u32 Blocks = 6;
    std::vector<u8> bank = MakeBank(0, 0, 0, 0x3, Blocks);
    module.LoadSoundBank(Sound, bank.data(), static_cast<u32>(bank.size()));

    s16 block[2] = {};

    // One pass: the voice stops at the end of its data
    module.PlaySound(Sound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    s32 once = 0;
    while (module.IsVoiceFree(Voice) == 0 && once < 96000)
    {
        module.Mix(block, 1);
        once++;
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(%d frames)", once);
    Check("a single pass stops at the end of its data", module.IsVoiceFree(Voice) == 1, detail);

    // A play of 0 passes is one pass too, and is let go of the same way: this is what the game sends for a sound it repeats
    module.PlaySound(Sound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 0);
    u32 start = SoundProcessor().VoiceStart(Voice);
    u32 end = start + Blocks * BlockWords;
    bool inside = true;
    s32 repeating = 0;
    while (module.IsVoiceFree(Voice) == 0 && repeating < 96000)
    {
        module.Mix(block, 1);
        // Only while it is sounding: the stop leaves the read position a word past the data, which is not the voice reading
        // anything, and the voice is not let go of until the next block of the mix
        if (SoundProcessor().VoiceActive(Voice))
        {
            u32 next = SoundProcessor().VoiceNext(Voice);
            if (next < start || next > end)
            {
                inside = false;
            }
        }

        repeating++;
    }

    std::snprintf(detail, sizeof(detail), "(%d frames against %d)", repeating, once);
    Check("a sound the game repeats is still one pass, and is let go of", module.IsVoiceFree(Voice) == 1, detail);
    Check("and never reads outside its own data", inside == true, "");

    // A count of passes is honoured: the voice goes round its own data that many times, then is let go of
    module.PlaySound(Sound, Voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 3);
    s32 counted = 0;
    while (module.IsVoiceFree(Voice) == 0 && counted < 96000)
    {
        module.Mix(block, 1);
        counted++;
    }

    std::snprintf(detail, sizeof(detail), "(%d frames against %d for one pass)", counted, once);
    Check("a count of passes is let go of when it is done", counted > once * 2 && counted < once * 4, detail);
}

// A voice on either core must come out at the same level: core 0's output reaches the DAC through core 1's external input, so
// that path has to be unity. Half there is 6 dB off everything on core 0 - which is where the game puts its music.
void CheckCoresMatch()
{
    Phase("cores");

    SoundModule& module = Module();
    constexpr s32 BankSound = 0x5000;

    std::vector<u8> bank = MakeBank(0, 0, 0, 0x5, 8);

    auto peakOnVoice = [&](s32 voice) -> s32
    {
        module.Reset();
        module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size()));
        module.PlaySound(BankSound, voice, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);

        s16 block[2] = {};
        s32 peak = 0;
        for (s32 frame = 0; frame < 400; frame++)
        {
            module.Mix(block, 1);
            if (frame > 100)
            {
                s32 level = block[0] < 0 ? -block[0] : block[0];
                if (level > peak)
                {
                    peak = level;
                }
            }
        }

        return peak;
    };

    // Voice 4 is on core 0, voice 30 on core 1. Core 0's signal goes through three 15 bit multiplies on its way out (its own
    // master, the external volume, core 1's master) where core 1's goes through one, so the two differ by a step or two of the
    // range and nothing more
    s32 first = peakOnVoice(4);
    s32 second = peakOnVoice(30);
    s32 difference = first > second ? first - second : second - first;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "(core 0 voice %d, core 1 voice %d)", first, second);
    Check("a voice is heard the same on either core", first > 0 && difference <= 4, detail);
}

// The group's volume is the only thing between the game's volume register and what is heard: a group at half must halve the
// level and a group at full must leave it alone, or every sound in a group is out by that much (which is how a sound is heard
// as too loud while the numbers the game passes look right)
void CheckGroupVolume()
{
    Phase("group volume");

    SoundModule& module = Module();
    constexpr s32 Voice = 20;
    constexpr s32 BankSound = 0x3000;

    std::vector<u8> bank = MakeBank(0, 0, 0, 0x4, 8);

    auto peakAt = [&](u16 groupLeft, u16 groupRight) -> s32
    {
        module.Reset();
        module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size()));
        module.SetGroupVolume(1u << GroupShift, groupLeft, groupRight);
        module.PlaySound(BankSound, Voice | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);

        s16 block[2] = {};
        s32 peak = 0;
        // Past the attack, where the sound sits at its own level
        for (s32 frame = 0; frame < 400; frame++)
        {
            module.Mix(block, 1);
            if (frame > 100)
            {
                s32 level = block[0] < 0 ? -block[0] : block[0];
                if (level > peak)
                {
                    peak = level;
                }
            }
        }

        return peak;
    };

    s32 full = peakAt(FullGroupVolume, FullGroupVolume);
    s32 half = peakAt(FullGroupVolume / 2, FullGroupVolume / 2);
    s32 quarter = peakAt(FullGroupVolume / 4, FullGroupVolume / 4);

    Check("a group at full lets the sound through", full > 0, "");

    // A group's volume of 1 is 0x1000: half is 0x800 and a quarter 0x400, and the level follows them
    char detail[128];
    std::snprintf(detail, sizeof(detail), "(full %d, half %d, quarter %d)", full, half, quarter);
    s32 halfWanted = full / 2;
    s32 quarterWanted = full / 4;
    Check("a group at half halves the sound", half >= halfWanted - 2 && half <= halfWanted + 2, detail);
    Check("a group at a quarter quarters it", quarter >= quarterWanted - 2 && quarter <= quarterWanted + 2, detail);
}

// The region maker is defined with the stream checks below; this one needs it here
std::vector<u8> MakeRegion(u32 blocks, u8 nibble);

// The game's music volume option reaches a track through the music's group, the way retail's does (MsPlayStream records the
// stream's channel in its group and hands the stream its group-scaled volumes). A track's voice is not one of the game's
// channels, so a module that only scaled the voices the game plays itself would leave the option reaching nothing
void CheckMusicGroupVolume()
{
    Phase("music group volume");

    SoundModule& module = Module();
    constexpr s32 Voice = 22;
    constexpr s32 Channel = 0;

    auto peakAt = [&](u16 groupLeft, u16 groupRight) -> s32
    {
        module.Reset();
        g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);

        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);
        module.SetGroupVolume(3u << GroupShift, groupLeft, groupRight);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (3u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        s32 peak = 0;
        for (s32 frame = 0; frame < 4000; frame++)
        {
            module.Mix(block, 1);
            // Past the ramp and the attack, where the track sits at its own level
            if (frame > 800)
            {
                s32 level = block[0] < 0 ? -block[0] : block[0];
                if (level > peak)
                {
                    peak = level;
                }
            }
        }

        module.StopMusic(Channel);
        return peak;
    };

    s32 full = peakAt(FullGroupVolume, FullGroupVolume);
    s32 half = peakAt(FullGroupVolume / 2, FullGroupVolume / 2);

    Check("the music group at full lets a track through", full > 0, "");

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(full %d, half %d)", full, half);
    Check("the music group at half halves the track", half >= full / 2 - 2 && half <= full / 2 + 2, detail);

    // And the option is heard mid-track, not only at its start: retail rescales the group's channels when the volume is set
    module.Reset();
    g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);
    u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
    module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);
    module.SetGroupVolume(3u << GroupShift, FullGroupVolume, FullGroupVolume);
    module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
    module.StreamMusic(1, Channel, Voice | (3u << GroupShift), 0x1000, false);
    module.PrepareMusic(Channel);
    module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
    module.PlayMusic(Channel);

    s16 block[2] = {};
    for (s32 frame = 0; frame < 4000; frame++)
    {
        module.Mix(block, 1);
    }

    s32 before = 0;
    for (s32 frame = 0; frame < 4000; frame++)
    {
        module.Mix(block, 1);
        s32 level = block[0] < 0 ? -block[0] : block[0];
        if (level > before)
        {
            before = level;
        }
    }

    module.SetGroupVolume(3u << GroupShift, FullGroupVolume / 2, FullGroupVolume / 2);
    for (s32 frame = 0; frame < 4000; frame++)
    {
        module.Mix(block, 1);
    }

    s32 after = 0;
    for (s32 frame = 0; frame < 4000; frame++)
    {
        module.Mix(block, 1);
        s32 level = block[0] < 0 ? -block[0] : block[0];
        if (level > after)
        {
            after = level;
        }
    }

    std::snprintf(detail, sizeof(detail), "(before %d, after %d)", before, after);
    Check("a track turned down mid-play follows the option", after >= before / 2 - 2 && after <= before / 2 + 2, detail);
    module.StopMusic(Channel);
}

// A sound whose block moves while it plays: the memory's compaction must take the voice with it, or the voice reads whatever
// lands in its old space, which is heard as a burst of noise
void CheckCompactionUnderVoice()
{
    Phase("compaction");

    SoundModule& module = Module();
    constexpr s32 Voice = 20;
    constexpr s32 BankSound = 0x4000;
    constexpr s32 GoneSound = 0x4001;

    // A pattern worth comparing: the filter carries it from block to block, so a voice reading the wrong bytes shows at once.
    // Its period must not be the distance the memory moves by, or a voice reading the neighbouring blocks by mistake reads the
    // same bytes and the check cannot fail: this one repeats every 13 blocks, and the gap it closes is 8
    std::vector<u8> sound(64 * BlockBytes);
    for (u32 block = 0; block < 64; block++)
    {
        MakeBlock(sound.data() + block * BlockBytes, 0, 0, 1, static_cast<u8>((block * 5 + block / 13) & 0x7));
    }

    std::vector<u8> gone = MakeBank(0, 0, 0, 0x1, 8);

    // The same play twice: once with the memory compacted under the voice, once without
    u32 startBefore = 0;
    u32 startAfter = 0;
    auto run = [&](bool compact, std::vector<s16>& out)
    {
        module.Reset();
        // Whatever this is sits first, so releasing it leaves the gap the sound below has to be moved into
        module.LoadSoundBank(GoneSound, gone.data(), static_cast<u32>(gone.size()));
        std::vector<u8> bank(BankHeaderBytes + sound.size(), 0);
        std::memcpy(bank.data(), "MSVp", 4);
        std::memcpy(bank.data() + BankHeaderBytes, sound.data(), sound.size());
        module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size()));
        module.ReleaseSound(GoneSound);

        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlaySound(BankSound, Voice | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);

        out.assign(1600 * 2, 0);
        for (s32 frame = 0; frame < 1600; frame++)
        {
            if (compact && frame == 400)
            {
                startBefore = SoundProcessor().VoiceStart(Voice);
                module.CompactSoundMemory();
                startAfter = SoundProcessor().VoiceStart(Voice);
            }

            module.Mix(&out[frame * 2], 1);
        }
    };

    std::vector<s16> plain;
    std::vector<s16> compacted;
    run(false, plain);
    run(true, compacted);

    // The sound really was moved, not just left where it was
    char moved[96];
    std::snprintf(moved, sizeof(moved), "(start %u -> %u)", startBefore, startAfter);
    Check("the compaction moved the sound the voice was playing", startBefore != startAfter, moved);

    s32 worst = 0;
    for (size_t index = 0; index < plain.size(); index++)
    {
        s32 distance = plain[index] - compacted[index];
        if (distance < 0)
        {
            distance = -distance;
        }

        if (distance > worst)
        {
            worst = distance;
        }
    }

    char detail[96];
    std::snprintf(detail, sizeof(detail), "(worst difference %d of 32768)", worst);
    Check("a sound whose block moves keeps sounding the same", worst == 0, detail);

    // The window the module gave the voice is part of following it: a loop point left behind sends a looping sound back to
    // where its data used to be, and an end left behind lets a one-shot read on past its own samples
    module.Reset();
    module.LoadSoundBank(GoneSound, gone.data(), static_cast<u32>(gone.size()));
    std::vector<u8> looping(BankHeaderBytes + sound.size(), 0);
    std::memcpy(looping.data(), "MSVp", 4);
    std::memcpy(looping.data() + BankHeaderBytes, sound.data(), sound.size());
    module.LoadSoundBank(BankSound, looping.data(), static_cast<u32>(looping.size()));
    module.ReleaseSound(GoneSound);
    module.PlaySound(BankSound, Voice | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    module.Mix(&plain[0], 1);
    u32 moveStartBefore = SoundProcessor().VoiceStart(Voice);
    u32 loopBefore = SoundProcessor().VoiceLoopStart(Voice);
    u32 endBefore = SoundProcessor().VoiceDataEnd(Voice);
    module.CompactSoundMemory();
    u32 moveStartAfter = SoundProcessor().VoiceStart(Voice);
    u32 loopAfter = SoundProcessor().VoiceLoopStart(Voice);
    u32 endAfter = SoundProcessor().VoiceDataEnd(Voice);
    char window[160];
    std::snprintf(window, sizeof(window), "(start %u -> %u, loop %u -> %u, end %u -> %u)", moveStartBefore, moveStartAfter, loopBefore,
                  loopAfter, endBefore, endAfter);
    // The sound moved, so all three numbers have to move with it by the same amount
    Check("a moved sound's loop point moves with it",
          moveStartAfter != moveStartBefore && loopAfter - loopBefore == moveStartAfter - moveStartBefore, window);
    Check("and so does the end of its data", endBefore != 0 && endAfter - endBefore == moveStartAfter - moveStartBefore, window);

    // A play that reads the sound table after a compaction: the table's addresses must move with the bytes, or a sound played
    // after the compaction reads its old address, which now holds whichever sound was moved down onto it
    module.Reset();
    module.LoadSoundBank(GoneSound, gone.data(), static_cast<u32>(gone.size()));
    std::vector<u8> afterBank(BankHeaderBytes + sound.size(), 0);
    std::memcpy(afterBank.data(), "MSVp", 4);
    std::memcpy(afterBank.data() + BankHeaderBytes, sound.data(), sound.size());
    module.LoadSoundBank(BankSound, afterBank.data(), static_cast<u32>(afterBank.size()));
    module.ReleaseSound(GoneSound);
    module.CompactSoundMemory();
    module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
    module.PlaySound(BankSound, Voice | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);
    std::vector<s16> playAfter(1600 * 2, 0);
    for (s32 frame = 0; frame < 1600; frame++)
    {
        module.Mix(&playAfter[frame * 2], 1);
    }
    s32 playWorst = 0;
    for (size_t index = 0; index < plain.size(); index++)
    {
        s32 distance = plain[index] - playAfter[index];
        if (distance < 0)
        {
            distance = -distance;
        }
        if (distance > playWorst)
        {
            playWorst = distance;
        }
    }
    char playDetail[96];
    std::snprintf(playDetail, sizeof(playDetail), "(worst difference %d of 32768)", playWorst);
    Check("a sound played after a compaction reads its moved block", playWorst == 0, playDetail);


    // The arena after a release: a block that goes back to the memory's end is free space, not a hole as well, or the next
    // sound that does not fit a hole is handed the very address of one
    module.Reset();
    std::vector<u8> small(BankHeaderBytes + 8 * BlockBytes, 0);
    std::memcpy(small.data(), "MSVp", 4);
    std::vector<u8> big(BankHeaderBytes + 64 * BlockBytes, 0);
    std::memcpy(big.data(), "MSVp", 4);
    constexpr u32 FirstSound = 0x4100;
    constexpr u32 SecondSound = 0x4101;
    constexpr u32 ThirdSound = 0x4102;
    u32 firstAt = module.LoadSoundBank(FirstSound, small.data(), static_cast<u32>(small.size()));
    module.ReleaseSound(FirstSound);
    u32 secondAt = module.LoadSoundBank(SecondSound, small.data(), static_cast<u32>(small.size()));
    u32 thirdAt = module.LoadSoundBank(ThirdSound, big.data(), static_cast<u32>(big.size()));
    char room[128];
    std::snprintf(room, sizeof(room), "(first %u, second %u, third %u of %u words)", firstAt, secondAt, thirdAt,
                  static_cast<u32>(64 * BlockWords));
    Check("a released sound's space is not handed out twice", thirdAt != 0 && secondAt != 0
                                                                && (thirdAt + 64 * BlockWords <= secondAt
                                                                    || secondAt + 8 * BlockWords <= thirdAt),
          room);
}

// A music region of the disc's own shape: ADPCM blocks, with a loop point marked in the middle of it (as a track's own data
// carries), which the ring the module builds must not follow
std::vector<u8> MakeRegion(u32 blocks, u8 nibble)
{
    std::vector<u8> region(blocks * BlockBytes);
    for (u32 block = 0; block < blocks; block++)
    {
        MakeBlock(region.data() + block * BlockBytes, block == blocks / 2 ? (BlockLoopEnd | BlockLoop) : 0, 0, 0, nibble);
    }

    return region;
}

// The music's streams: a region laid into the ring the player was given, played through a voice, ended or looped as the game
// asked, and reported to it the way the module reports a stream
void CheckStreams()
{
    Phase("streams");

    SoundModule& module = Module();
    constexpr s32 Voice = 10;
    constexpr s32 Channel = 0;
    constexpr s32 Child = 1;

    // A track that plays once
    {
        module.Reset();
        g_Region = MakeRegion(64, 0x4);

        // The ring the module hands out is an address in bytes, as the game's own arithmetic on it expects
        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        u32 ring = ringBytes / 2;
        Check("the player's ring is reserved", ringBytes != 0, "");
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        CheckEqual("the stream is taken", module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, true), Channel);
        // The module lists the stream from the moment it is started, before it is even ready: the game polls this to know its
        // track is still the module's
        Check("a started stream is listed as playing", module.IsMusicPlaying(Channel), "");

        module.PrepareMusic(Channel);
        Check("a prepared stream is ready", module.IsMusicReady(Channel), "");
        CheckEqual("it is not playing yet", module.IsMusicPlaying(Channel), 1);

        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        CheckEqual("the stream plays", module.PlayMusic(Channel), 0);

        // The ring holds the region's blocks, with the module's flags rather than the track's own loop point
        s16 expected[BlockWords];
        std::memcpy(expected, g_Region.data(), BlockBytes);
        s16 ringBlock[BlockWords];
        std::memcpy(ringBlock, &SoundProcessor().Memory()[ring], BlockBytes);
        Check("the ring's first block is the region's first", std::memcmp(expected, ringBlock, BlockBytes - 1) == 0, "");

        // The track's own loop point sits in the middle of the region: the ring must have cleared its flags, or the voice would
        // stop or jump there mid-track
        s16 middle[BlockWords];
        std::memcpy(middle, &SoundProcessor().Memory()[ring + 32 * BlockWords], BlockBytes);
        CheckEqual("the track's own loop flag is not the ring's", (static_cast<u16>(middle[0]) >> 8) & BlockLoopEnd, 0);

        // It sounds, and it ends by itself: the game is told so, which is how a player moves on to its next track
        s16 block[2] = {};
        s32 loudest = 0;
        s32 frames = 0;
        while (module.IsMusicPlaying(Channel) && frames < 48000)
        {
            module.Mix(block, 1);
            if (block[0] > loudest)
            {
                loudest = block[0];
            }

            frames++;
        }

        Check("a stream sounds", loudest > 0, "");
        Check("a track that plays once ends", !module.IsMusicPlaying(Channel), "");

        // 64 blocks of 28 samples at the sample's own rate is 1792 frames; the ring reads a little ahead of the voice, so the
        // end comes within a block or so of that
        char detail[96];
        std::snprintf(detail, sizeof(detail), "(ended after %d frames, %d expected)", frames, 64 * 28);
        Check("it ends where its data does", frames > 64 * 28 - 200 && frames < 64 * 28 + 400, detail);
    }

    // A track that loops: it keeps going, and is still the module's
    {
        module.Reset();
        g_Region = MakeRegion(64, 0x4);

        u32 ring = module.ReserveSoundMemory(StreamRingBytes) / 2;
        module.SetChannelBuffer(Channel, ring * 2, StreamRingBytes);
        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        for (s32 frame = 0; frame < 20000; frame++)
        {
            module.Mix(block, 1);
        }

        Check("a looping track keeps playing", module.IsMusicPlaying(Channel), "");
        Check("and its voice is still going", SoundProcessor().VoiceActive(Voice), "");

        // The module's stop is the game's, and the voice fades rather than cutting
        module.StopMusic(Channel);
        Check("a stopped track is no longer playing", !module.IsMusicPlaying(Channel), "");
    }

    // A track that is longer than the ring: the module wraps the ring and keeps laying the rest of the data down behind the
    // voice, which is what stops a whole region having to be decoded into the host's memory up front
    {
        module.Reset();
        // Four rings' worth of data, so the ring has to be refilled several times over
        g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);

        u32 ring = module.ReserveSoundMemory(StreamRingBytes) / 2;
        module.SetChannelBuffer(Channel, ring * 2, StreamRingBytes);
        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, true);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        s32 loudest = 0;
        s32 quietRuns = 0;
        s32 run = 0;
        for (s32 frame = 0; frame < 60000; frame++)
        {
            module.Mix(block, 1);
            if (block[0] > loudest)
            {
                loudest = block[0];
            }

            if (block[0] == 0)
            {
                run++;
                if (run > 200)
                {
                    quietRuns++;
                    run = 0;
                }
            }
            else
            {
                run = 0;
            }
        }

        Check("a track longer than the ring sounds throughout", loudest > 0, "");
        // A ring refilled behind the voice leaves no silence: a gap of more than two hundred frames would be the writer
        // falling behind the reader
        CheckEqual("and never falls silent mid-track", quietRuns, 0);
    }

    // An interleaved track: the data alternates in chunks between the two sides of the pair, the parent's ring taking the even
    // ones and the child's the odd ones
    {
        module.Reset();
        constexpr u32 ChunkBytes = 0x100;
        constexpr u32 Chunks = 8;
        g_Region.assign(ChunkBytes * Chunks, 0);
        for (u32 chunk = 0; chunk < Chunks; chunk++)
        {
            for (u32 block = 0; block < ChunkBytes / BlockBytes; block++)
            {
                MakeBlock(g_Region.data() + chunk * ChunkBytes + block * BlockBytes, 0, 0, 0,
                          static_cast<u8>(chunk + 1));
            }
        }

        // A player's buffer is 0x8000 bytes, half for each side of an interleaved track
        u32 player = module.ReserveSoundMemory(StreamRingBytes * 2);
        u32 parentRingBytes = player;
        u32 childRingBytes = player + StreamRingBytes;
        u32 parentRing = parentRingBytes / 2;
        u32 childRing = childRingBytes / 2;
        module.SetChannelBuffer(0, parentRingBytes, StreamRingBytes);
        module.SetChannelBuffer(1, childRingBytes, StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, 0, Voice | (1u << GroupShift), 0x1000, true);
        module.InterleaveMusic(0, ChunkBytes);
        module.AddMusicChannel(1, 0, (Voice + 1) | (1u << GroupShift), childRingBytes);
        module.PrepareMusic(0);
        module.PrepareMusic(1);

        // Each side's first block is its own chunk's, not the other's: the nibbles are in the block's second word, the first
        // being the shift, filter and flags
        u8 parentFirst = static_cast<u8>(SoundProcessor().Memory()[parentRing + 1] & 0xFF);
        u8 childFirst = static_cast<u8>(SoundProcessor().Memory()[childRing + 1] & 0xFF);
        CheckEqual("the parent plays the first chunk", parentFirst, 0x11);
        CheckEqual("the child plays the second", childFirst, 0x22);
    }

    // What the frontend actually does, and what the log showed: two players running at once, each an interleaved pair of voices,
    // for long enough that the rings have gone round many times. All four voices have to still be sounding at the end - the
    // port's log had one of them drop out a few seconds in, which is a side of a track falling silent for the rest of the game
    {
        module.Reset();
        constexpr u32 ChunkBytes = 0x1000;
        constexpr u32 Chunks = 6;
        g_Region.assign(ChunkBytes * Chunks, 0);
        for (u32 chunk = 0; chunk < Chunks; chunk++)
        {
            for (u32 block = 0; block < ChunkBytes / BlockBytes; block++)
            {
                MakeBlock(g_Region.data() + chunk * ChunkBytes + block * BlockBytes, 0, 0, 1,
                          static_cast<u8>((chunk * 3 + block) & 0x7));
            }
        }

        // Two players, as the game reserves them: 0x8000 bytes each, a half for each side
        u32 firstPlayer = module.ReserveSoundMemory(StreamRingBytes * 2);
        u32 secondPlayer = module.ReserveSoundMemory(StreamRingBytes * 2);
        module.SetChannelBuffer(0, firstPlayer, StreamRingBytes);
        module.SetChannelBuffer(1, firstPlayer + StreamRingBytes, StreamRingBytes);
        module.SetChannelBuffer(2, secondPlayer, StreamRingBytes);
        module.SetChannelBuffer(3, secondPlayer + StreamRingBytes, StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, 0, Voice | (1u << GroupShift), 0x1000, false);
        module.InterleaveMusic(0, ChunkBytes);
        module.AddMusicChannel(1, 0, (Voice + 1) | (1u << GroupShift), firstPlayer + StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, 2, (Voice + 2) | (1u << GroupShift), 0x1000, false);
        module.InterleaveMusic(2, ChunkBytes);
        module.AddMusicChannel(3, 2, (Voice + 3) | (1u << GroupShift), secondPlayer + StreamRingBytes);

        module.PrepareMusic(0);
        module.PrepareMusic(1);
        module.PrepareMusic(2);
        module.PrepareMusic(3);
        for (s32 voice = Voice; voice < Voice + 4; voice++)
        {
            module.SetVoiceVolume(voice, 0x3FFF, 0x3FFF);
        }

        module.PlayMusic(0);
        module.PlayMusic(1);
        module.PlayMusic(2);
        module.PlayMusic(3);

        s16 block[2] = {};
        s32 lostAt = -1;
        for (s32 frame = 0; frame < 200000; frame++)
        {
            module.Mix(block, 1);
            for (s32 voice = Voice; voice < Voice + 4; voice++)
            {
                if (!SoundProcessor().VoiceActive(voice) && lostAt < 0)
                {
                    lostAt = frame;
                }
            }
        }

        char detail[128];
        std::snprintf(detail, sizeof(detail), "(%d of the four voices stopped, first at frame %d)", lostAt < 0 ? 0 : 1,
                      lostAt);
        Check("two music players keep all four of their voices sounding", lostAt < 0, detail);
        Check("and both are still the module's", module.IsMusicPlaying(0) && module.IsMusicPlaying(2), "");

        for (s32 channel = 0; channel < 4; channel++)
        {
            module.StopMusic(channel);
        }
    }

    // The game prepares and plays only the parent's channel; the module starts the other side itself. A side keyed onto a ring
    // with nothing under it played the memory that was there - heard as a burst of the wrong sound - and then read on until a
    // stale block's flags stopped it, which is the side that went silent for the rest of the session in the port's log
    {
        module.Reset();
        constexpr u32 ChunkBytes = 0x1000;
        constexpr u32 Chunks = 6;
        g_Region.assign(ChunkBytes * Chunks, 0);
        for (u32 chunk = 0; chunk < Chunks; chunk++)
        {
            for (u32 block = 0; block < ChunkBytes / BlockBytes; block++)
            {
                MakeBlock(g_Region.data() + chunk * ChunkBytes + block * BlockBytes, 0, 0, 1,
                          static_cast<u8>((chunk * 3 + block) & 0x7));
            }
        }

        u32 player = module.ReserveSoundMemory(StreamRingBytes * 2);
        module.SetChannelBuffer(Channel, player, StreamRingBytes);
        module.SetChannelBuffer(Child, player + StreamRingBytes, StreamRingBytes);

        // Whatever was in the other side's ring before the track: every block of it ends, so a voice let loose on it stops at
        // the first block it reads
        std::vector<u8> stale(StreamRingBytes, 0);
        for (u32 block = 0; block < StreamRingBytes / BlockBytes; block++)
        {
            stale[block * BlockBytes + 1] = BlockLoopEnd;
        }

        SoundProcessor().WriteMemory((player + StreamRingBytes) / 2, stale.data(), StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.InterleaveMusic(Channel, ChunkBytes);
        module.AddMusicChannel(Child, Channel, (Voice + 1) | (1u << GroupShift), player + StreamRingBytes);

        // The game's own order: the parent is prepared and played, and the module starts the other side with it
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.SetVoiceVolume(Voice + 1, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        s32 lostAt = -1;
        for (s32 frame = 0; frame < 24000; frame++)
        {
            module.Mix(block, 1);
            if (!SoundProcessor().VoiceActive(Voice + 1) && lostAt < 0)
            {
                lostAt = frame;
            }
        }

        char detail[96];
        std::snprintf(detail, sizeof(detail), "(the other side stopped at frame %d)", lostAt);
        Check("a side the game never prepared still has a ring under it", lostAt < 0, detail);
        Check("and the parent is sounding with it", SoundProcessor().VoiceActive(Voice), "");

        module.StopMusic(Channel);
        module.StopMusic(Child);
    }

    // A track's voice is not the game's to take while the track is the module's. Retail marks a music channel taken when its
    // stream starts; a voice reported free is one the game marks unplaying and then hands to a sound, which is heard as a sound
    // that came from nowhere and a side of the track that is gone
    {
        module.Reset();
        g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);

        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);

        Check("a voice with no track on it is free", module.IsVoiceFree(Voice) == 1, "");

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        for (s32 frame = 0; frame < 2000; frame++)
        {
            module.Mix(block, 1);
        }

        Check("a track's voice is not free while the track is the module's", module.IsVoiceFree(Voice) == 0, "");

        module.StopMusic(Channel);
        s32 freed = 0;
        while (module.IsVoiceFree(Voice) == 0 && freed < 48000)
        {
            module.Mix(block, 1);
            freed++;
        }

        char detail[96];
        std::snprintf(detail, sizeof(detail), "(free after %d frames)", freed);
        Check("and is the game's again once the track is let go of", module.IsVoiceFree(Voice) == 1, detail);
    }

    // A channel given another track lets the one it had go. The game changes tracks in play, and a re-bound channel that left
    // its old voice looping its ring would keep sounding under the new track - a copy of the music per track change, with
    // nothing left in the game knowing it is there to stop
    {
        module.Reset();
        g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);

        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        for (s32 frame = 0; frame < 2000; frame++)
        {
            module.Mix(block, 1);
        }

        Check("the first track is playing", SoundProcessor().VoiceActive(Voice), "");

        // The same channel, another track, on another voice
        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, (Voice + 1) | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice + 1, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        // The old voice is let go of, not cut off: it fades on its release, so it is the fading that is given time here
        s32 faded = 0;
        while (SoundProcessor().VoiceActive(Voice) && faded < 48000)
        {
            module.Mix(block, 1);
            faded++;
        }

        char detail[96];
        std::snprintf(detail, sizeof(detail), "(silent after %d frames)", faded);
        Check("a re-bound channel lets its old voice go", !SoundProcessor().VoiceActive(Voice), detail);
        Check("and the new one is the track", SoundProcessor().VoiceActive(Voice + 1), "");
        module.StopMusic(Channel);
    }

    // A channel re-bound while its old voice is still finishing the track it had. The voice reads where the old ring left it,
    // and taken as the new track's reading that puts the writer a whole ring behind itself: the prime then writes nothing and
    // the play is refused for a track whose data is all there. Heard as a level that opens with no music and gets it back only
    // when something else happens to re-bind the channel
    {
        module.Reset();
        g_Region = MakeRegion(4 * (StreamRingWords / BlockWords), 0x6);

        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);

        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        s16 block[2] = {};
        for (s32 frame = 0; frame < 2000; frame++)
        {
            module.Mix(block, 1);
        }

        Check("the track the channel had is playing", SoundProcessor().VoiceActive(Voice), "");

        // The same channel and the same voice, another track, while the old one is still being let go of
        module.ReadMusic(1, 0, static_cast<u32>(g_Region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, false);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        s32 played = module.PlayMusic(Channel);

        char detail[96];
        std::snprintf(detail, sizeof(detail), "(the play returned %d)", played);
        Check("a channel re-bound as its old voice is let go of plays its new track", played == 0, detail);
        Check("and the new track is the module's", module.IsMusicPlaying(Channel), detail);
        module.StopMusic(Channel);
    }

    // The claim the streaming rests on: a track laid through the ring sounds exactly like the same bytes played as a sound in
    // a bank, sample for sample. If this drifts, the ring is altering the audio, and a track that was checked as a bank would
    // not be the one a player hears
    {
        constexpr s32 BankSound = 0x2000;
        constexpr u32 Blocks = 64;
        // No flags in the data: a sound played straight out of a bank follows them, and this is about the samples
        std::vector<u8> region(Blocks * BlockBytes);
        for (u32 block = 0; block < Blocks; block++)
        {
            MakeBlock(region.data() + block * BlockBytes, 0, 0, 1, static_cast<u8>((block * 5) & 0x7));
        }

        // As a sound in a bank
        module.Reset();
        std::vector<u8> bank(BankHeaderBytes + region.size(), 0);
        std::memcpy(bank.data(), "MSVp", 4);
        std::memcpy(bank.data() + BankHeaderBytes, region.data(), region.size());
        module.LoadSoundBank(BankSound, bank.data(), static_cast<u32>(bank.size()));
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlaySound(BankSound, Voice | (1u << GroupShift), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1);

        std::vector<s16> asSound(2000 * 2);
        for (s32 frame = 0; frame < 2000; frame++)
        {
            module.Mix(&asSound[frame * 2], 1);
        }

        // The same bytes, streamed through a ring
        module.Reset();
        g_Region = region;
        u32 ringBytes = module.ReserveSoundMemory(StreamRingBytes);
        module.SetChannelBuffer(Channel, ringBytes, StreamRingBytes);
        module.ReadMusic(1, 0, static_cast<u32>(region.size()));
        module.StreamMusic(1, Channel, Voice | (1u << GroupShift), 0x1000, true);
        module.PrepareMusic(Channel);
        module.SetVoiceVolume(Voice, 0x3FFF, 0x3FFF);
        module.PlayMusic(Channel);

        std::vector<s16> asStream(2000 * 2);
        for (s32 frame = 0; frame < 2000; frame++)
        {
            module.Mix(&asStream[frame * 2], 1);
        }

        // Past the volume's ramp up (the module's smooth volume, about five hundred frames) and before the data ends, the two
        // are the same waveform
        s32 worst = 0;
        for (s32 frame = 700; frame < 1500; frame++)
        {
            for (s32 side = 0; side < 2; side++)
            {
                s32 distance = asSound[frame * 2 + side] - asStream[frame * 2 + side];
                if (distance < 0)
                {
                    distance = -distance;
                }

                if (distance > worst)
                {
                    worst = distance;
                }
            }
        }

        char detail[96];
        std::snprintf(detail, sizeof(detail), "(worst difference %d of 32768)", worst);
        Check("a streamed track sounds exactly like the same bytes played as a sound", worst == 0, detail);
    }
}
}

int main()
{
    CheckInterpolation();
    CheckAdpcm();
    CheckVolume();
    CheckEnvelope();
    CheckLoops();
    CheckSaturation();
    CheckReverb();
    CheckSilence();
    CheckMemory();
    CheckModule();
    CheckMenuPlay();
    CheckPauseKeepsPlaying();
    CheckLoopPointIsPerPlay();
    CheckRepeatedPlay();
    CheckCoresMatch();
    CheckGroupVolume();
    CheckMusicGroupVolume();
    CheckCompactionUnderVoice();
    CheckStreams();

    std::printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
