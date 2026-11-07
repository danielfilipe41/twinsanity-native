// The PlayStation 2's sound processor, as a device: two cores of 24 voices, each voice an ADPCM loop out of the sound
// processor's memory through a Gaussian interpolator, an envelope and a volume; each core a dry and an effect sum, its own
// reverb network and its own master volume.
//
// Everything here follows the hardware's own arithmetic, checked against PCSX2's SPU2 (which is the reference for what the
// chip does, the chip's documentation alone being incomplete): the envelope's 16 bit level and its cache of the four phases'
// shifts and steps, the volume registers' other-phase encoding, the 15 bit multiplies, the interpolation table and the
// reverb network with its resampler. What the retail sound module asks of it -- which registers, in what order, when -- is
// spusoundmodule.cpp's, not this file's.
//
// Addresses are the hardware's: the sound processor's memory is 2 MB of 16 bit words, and every address here is a word's.
#pragma once

#include "common.h"

#include <vector>

namespace SdlAudio::Spu2
{
constexpr s32 CoreCount = 2;
constexpr s32 CoreVoices = 24;
constexpr s32 VoiceCount = CoreCount * CoreVoices;

// The sound processor's memory, as this host lends it. The chip's is 2 MB (0x100000 words); the host's is four times that,
// because the module here keeps a block for every sound the game's tables still name, while the console never has that many at
// once: its EE side hands the module one moving bank address (ps2/stream.cpp's ReadSoundBank, patched in with MsSetBankAddress)
// and only the music rings advance it, so the PS2's memory is a bank's worth rather than a level's. At the chip's own size a
// level's later banks are refused for want of room and their sounds play silent for the whole level
constexpr u32 MemoryWords = 0x400000;
constexpr u32 MemoryBytes = MemoryWords * 2;

// The envelope's level is 16 bit (the hardware's ENVX), full at 0x7FFF
constexpr s32 FullEnvelope = 0x7FFF;

// The two bytes of a block's first word that are not the shift and filter: which of them are loop points
constexpr u8 BlockLoopEnd = 0x01;
constexpr u8 BlockLoop = 0x02;
constexpr u8 BlockLoopStart = 0x04;

// Where a voice reads: an address of 8 word blocks, the first word of a block the shift, filter and loop flags
constexpr u32 BlockWords = 8;
constexpr u32 BlockBytes = BlockWords * 2;
constexpr u32 BlockAlign = 0xFFFF8;

// A voice's volume register and the slide the module's smooth volume writes. The register's low 15 bits are the magnitude
// and the 16th picks the other phase of the wave, which the 15 bit multiply then reads as a negative.
struct VolumeSlide
{
    union
    {
        u16 value;

        struct
        {
            u16 step : 2;
            u16 shift : 5;
            u16 unused7 : 5;
            u16 phase : 1;
            u16 decrease : 1;
            u16 exponential : 1;
            u16 enable : 1;
        };
    };

    u32 counter = 0;
    s32 level = 0;

    // A plain volume register: the slide off, the level the register's own
    void Set(u16 source);
    // One output frame of the slide, if it is on
    void Step();
};

// A voice's envelope: the two registers, the phase they are in and its level. The phases' shifts, steps and targets are
// worked out once per register write rather than per frame.
struct Envelope
{
    union
    {
        u32 value;

        struct
        {
            u16 attackDecay;
            u16 sustainRelease;
        };

        struct
        {
            u32 sustainLevel : 4;
            u32 decayShift : 4;
            u32 attackStep : 2;
            u32 attackShift : 5;
            u32 attackExponential : 1;
            u32 releaseShift : 5;
            u32 releaseExponential : 1;
            u32 sustainStep : 2;
            u32 sustainShift : 5;
            u32 unused29 : 1;
            u32 sustainDecrease : 1;
            u32 sustainExponential : 1;
        };
    };

    enum Phase : u8
    {
        Stopped,
        Attack,
        Decay,
        Sustain,
        Release,
    };

    struct Cached
    {
        bool decrease = false;
        bool exponential = false;
        u8 shift = 0;
        s8 step = 0;
        s32 target = 0;
    };

    // The four phases after attack (attack, decay, sustain, release), read by phase's own index
    Cached phases[5];
    u32 counter = 0;
    s32 level = 0;
    u8 phase = Stopped;

    void Cache();
    // One output frame. False once the envelope is over (the voice stops there)
    bool Step();
    void KeyOn();
    void KeyOff();
};

// A voice's ADPCM reader: which block it is on, the two samples the filter carries over from the block before, the
// interpolator's history and the pitch counter (the sample address' fraction in the top 12 bits).
struct Voice
{
    VolumeSlide volumeLeft;
    VolumeSlide volumeRight;
    Envelope envelope;

    u16 pitch = 0x3FFF;
    u32 loopStart = 0x2800;
    u32 start = 0x2800;
    // The next half block to decode, in words (its low three bits pick the quarter of the block)
    u32 next = 0x2801;

    s32 previous1 = 0;
    s32 previous2 = 0;

    bool useNoise = false;
    bool pitchModulated = false;
    s8 manualLoopStart = 0;
    u8 blockFlags = 0;

    // The sample address, 12 fraction bits
    s32 sampleAddress = 0;
    // Held: the voice keeps its place and says nothing (the module's pause holds the music where it is)
    bool held = false;
    // Where the sound's data ends, in words (0: whatever the block flags say). The chip only knows a block's flags, and the
    // disc's sounds do not all carry an end flag: it is the module that knows a sound's length, and this is it
    u32 dataEnd = 0;
    // Whether reaching that end loops back to the loop point (the game asked for more than one pass) or stops the voice
    bool dataLoop = false;
    // What the voice last gave the mix, before its envelope: the next voice's pitch modulates by it
    s32 out = 0;

    // The decoded block's 28 samples, and which block they are (NoBlock: none yet)
    static constexpr u32 NoBlock = 0xFFFFFFFF;
    u32 cachedBlock = NoBlock;
    s16 block[28] = {};
    s16 fifo[32] = {};
    u32 fifoWrite = 0;
    u32 fifoRead = 0;

    void Start();
    void Stop();
    bool Active() const { return envelope.phase != Envelope::Stopped; }
};

// A core's reverb registers, as libsd's preset tables write them. The addresses are in the units the registers hold: words,
// relative to the effect area's start.
struct ReverbRegisters
{
    s16 inputLeft = 0;
    s16 inputRight = 0;

    u32 apf1Size = 0;
    u32 apf2Size = 0;
    s16 apf1Volume = 0;
    s16 apf2Volume = 0;

    u32 sameLeftSource = 0;
    u32 sameRightSource = 0;
    u32 differentLeftSource = 0;
    u32 differentRightSource = 0;
    u32 sameLeftTarget = 0;
    u32 sameRightTarget = 0;
    u32 differentLeftTarget = 0;
    u32 differentRightTarget = 0;

    s16 iirVolume = 0;
    s16 wallVolume = 0;

    u32 combSource[4][2] = {};
    s16 combVolume[4] = {};

    u32 apfTarget[2][2] = {};
};

// One core: its 24 voices, its routing gates, its volumes, its reverb work area and the network's own state
struct Core
{
    Voice voices[CoreVoices];

    // A bit per voice: where its output goes (the dry sum's sides, the effect sum's sides)
    u32 dryLeft = 0xFFFFFF;
    u32 dryRight = 0xFFFFFF;
    u32 effectLeft = 0xFFFFFF;
    u32 effectRight = 0xFFFFFF;

    // The volume registers: the core's master, the effect return, what core 0 hands core 1, what the input carries
    VolumeSlide masterLeft;
    VolumeSlide masterRight;
    s32 effectVolumeLeft = 0;
    s32 effectVolumeRight = 0;
    s32 externalVolumeLeft = FullEnvelope;
    s32 externalVolumeRight = FullEnvelope;
    s32 inputVolumeLeft = FullEnvelope;
    s32 inputVolumeRight = FullEnvelope;

    ReverbRegisters reverb;
    bool reverbOn = false;
    u32 effectStart = 0;
    u32 effectEnd = 0;

    u8 noiseClock = 0;
    u32 noiseCounter = 0;
    u32 noiseOutput = 0;

    // The resampler's 64 frame window a side, and where in it the network is
    s16 downsample[2][128] = {};
    s16 upsample[2][128] = {};
    u32 resamplePosition = 0;

    void Reset();
};

class Device
{
public:
    // The sound processor's memory, in words
    s16* Memory() { return m_memory.data(); }
    // Bytes of a bank's samples into memory at a word address (what a DMA write does)
    void WriteMemory(u32 wordAddress, const void* bytes, u32 byteCount);
    void Reset();

    Core& GetCore(s32 core) { return m_cores[core]; }

    // One 48 kHz frame: the voices, both cores' networks, the master volumes, the sum at the top of the range
    void Mix(s32& left, s32& right);

    // The frame counter: the reverb's read position steps with it, and its two sides alternate on its low bit
    u32 Cycles() const { return m_cycles; }

    // One side's worth of the effect network's resampler window (the voice gate of the same name)
    s32 Downsample(Core& core, bool right);
    void Upsample(Core& core, s32& left, s32& right);

    // ---- the registers, as the sound module writes them ----
    // A voice's own: its volume on each side (the hardware's 16 bit register), its pitch, its envelope's two registers, and
    // where it starts. Writing the start of the loop by hand turns off the block flags' own loop point for the next start.
    void SetVoiceVolume(s32 voice, u16 left, u16 right);
    void SetVoicePitch(s32 voice, u16 pitch);
    void SetVoiceEnvelope(s32 voice, u16 attackDecay, u16 sustainRelease);
    void SetVoiceStart(s32 voice, u32 wordAddress);
    void SetVoiceLoopStart(s32 voice, u32 wordAddress);
    void SetVoiceNoise(s32 voice, bool noise);
    // Where the sound the voice plays ends, in words: past it the voice stops, since the data may carry no end flag of its own
    void SetVoiceDataEnd(s32 voice, u32 wordAddress);
    // Whether the voice goes back to its loop point when it reaches that end, rather than stopping there
    void SetVoiceDataLoop(s32 voice, bool loop);
    // Holds a voice where it is, contributing nothing, until it is let go again
    void SetVoiceHeld(s32 voice, bool held);
    bool VoiceHeld(s32 voice) const;
    // Its routing: the dry sum's sides and the effect send's, a bit per voice
    void SetDrySides(s32 core, u32 left, u32 right);
    void SetVoiceEffectSend(s32 core, s32 voice, bool on);
    // The voice's key: on starts it at its start address, off lets its envelope go from where it is
    void KeyOn(s32 voice);
    void KeyOff(s32 voice);
    bool VoiceActive(s32 voice) const;
    s32 VoiceEnvelope(s32 voice) const;
    // Where a voice is reading: the block it is on, so a sound the memory moved under it can be followed
    u32 VoiceNext(s32 voice) const;
    // Where a voice was started, which is inside the block its sound is in
    u32 VoiceStart(s32 voice) const;
    // Where a voice goes back to when its data loops, and where the module told it its data ends (0 for none)
    u32 VoiceLoopStart(s32 voice) const;
    u32 VoiceDataEnd(s32 voice) const;
    // A sound moved in the memory: the voice's start, loop point, end and the block it is reading move with it, its place in the
    // block kept
    void MoveVoice(s32 voice, s32 delta);
    // The release rate a key-off will fade at (the envelope's other bits stay as the play left them)
    void SetVoiceRelease(s32 voice, u32 rate);

    // A core's: its master volume, the effect's return volume, what it takes from core 0, its noise clock, and the effect's
    // register set with the work area it runs in
    void SetMasterVolume(s32 core, u16 left, u16 right);
    void SetEffectVolume(s32 core, u16 left, u16 right);
    void SetExternalVolume(s32 core, u16 left, u16 right);
    void SetNoiseClock(s32 core, u8 clock);
    void SetEffectArea(s32 core, u32 startWord, u32 endWord);
    void SetEffectEnabled(s32 core, bool on);
    void SetEffects(s32 core, const ReverbRegisters& registers);

private:
    s32 MixVoice(s32 core, s32 voice, s32& dryLeft, s32& dryRight, s32& effectLeft, s32& effectRight);
    void DecodeSamples(s32 core, s32 voice);
    void NextData(s32 core, s32 voice);
    void UpdateBlockHeader(s32 core, s32 voice);
    s32 VoiceSamples(s32 core, s32 voice);
    void UpdateNoise(Core& core);
    void MixCore(Core& core, s32 dryLeft, s32 dryRight, s32 effectLeft, s32 effectRight, s32 externalLeft,
                 s32 externalRight, s32& outLeft, s32& outRight);

    Core m_cores[CoreCount];
    // The heap's, not the object's: lent eight times the chip's memory, the buffer is more than a stack frame holds when a device
    // is made locally, which the proofs do
    std::vector<s16> m_memory = std::vector<s16>(MemoryWords, 0);
    u32 m_cycles = 0;
};

// The machine's one sound processor
Device& SoundProcessor();

// The interpolation table's own shape, for the proofs: the Gaussian over the 4 samples a voice's history holds
const s16 (*InterpolationTable())[4];

// A 15 bit multiply of a sample by a volume register's level, the 16 bit result the hardware's
inline s32 ApplyVolume(s32 sample, s32 volume)
{
    return (volume * sample) >> 15;
}

// The mix's top: the hardware's DAC takes what it is given and the range ends there
inline s32 Saturate(s32 sample)
{
    if (sample > 32767)
    {
        return 32767;
    }
    if (sample < -32768)
    {
        return -32768;
    }
    return sample;
}
}
