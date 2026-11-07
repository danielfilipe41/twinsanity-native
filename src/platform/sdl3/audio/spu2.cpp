// The sound processor's arithmetic. Every step here is the chip's own, taken from what the hardware does and checked against
// PCSX2's SPU2 (the only complete reference for it): the envelope's cached phase shifts, the 39 tap resampler around the
// reverb network, the ADPCM filter's history across blocks.
#include "spu2.h"

#include <cmath>

namespace
{
using namespace SdlAudio::Spu2;

constexpr double Pi = 3.14159265358979323846;

// The SPU's interpolation table: a Gaussian over the four samples a voice's history holds, as Near's generator makes it
// (PCSX2's interpolate_table.h carries the same generator and the table it produces). Built once, so the table is the
// published one rather than a transcription of it.
struct InterpolationTableBuilder
{
    s16 values[256][4] = {};

    InterpolationTableBuilder()
    {
        double window[512] = {};
        for (u32 n = 0; n < 512; n++)
        {
            double k = 0.5 + n;
            double s = std::sin(Pi * k * 2.048 / 1024);
            double t = (std::cos(Pi * k * 2.000 / 1023) - 1.0) * 0.50;
            double u = (std::cos(Pi * k * 4.000 / 1023) - 1.0) * 0.08;
            window[511 - n] = s * (t + u + 1.0) / k;
        }

        // Scaled so the whole window sums to the 1.15 the multiplies want, then each phase's four taps made to sum to the
        // same, so a voice that holds still neither swells nor fades
        double sum = 0.0;
        for (double value : window)
        {
            sum += value;
        }
        double scale = 0x7F80 * 128 / sum;
        for (double& value : window)
        {
            value *= scale;
        }

        constexpr double Full = 0x7F80;
        for (u32 phase = 0; phase < 256; phase++)
        {
            double phaseSum = window[phase] + window[phase + 256] + window[511 - phase] + window[255 - phase];
            double diff = (phaseSum - Full) / 4;
            s16* taps = values[255 - phase];
            taps[0] = Round(window[phase] - diff);
            taps[1] = Round(window[phase + 256] - diff);
            taps[2] = Round(window[511 - phase] - diff);
            taps[3] = Round(window[255 - phase] - diff);
        }
    }

    static s16 Round(double value)
    {
        double rounded = std::round(value);
        if (rounded > 32767.0)
        {
            return 32767;
        }
        if (rounded < -32768.0)
        {
            return -32768;
        }
        return static_cast<s16>(rounded);
    }
};

const InterpolationTableBuilder g_InterpolationTable;

// The ADPCM filter's coefficients in 64ths, per block header's filter number
constexpr s32 FilterCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

// Decodes a block's 28 samples after the two the voice already carries, and brings those up to date
void DecodeBlock(const s16* block, s16* out, s32& previous1, s32& previous2)
{
    const s32 header = block[0];
    const s32 shift = (header & 0xF) + 16;
    const s32 filter = (header >> 4) & 0xF;
    const s32 first = filter < 5 ? FilterCoefficients[filter][0] : 0;
    const s32 second = filter < 5 ? FilterCoefficients[filter][1] : 0;

    const s8* bytes = reinterpret_cast<const s8*>(&block[1]);
    const s8* end = &bytes[13];
    for (; bytes <= end; ++bytes)
    {
        s32 packed = (static_cast<s32>(*bytes) << 28) & 0xF0000000;
        s32 sample = (packed >> shift) + ((first * previous1 + second * previous2 + 32) >> 6);
        *(out++) = static_cast<s16>(Saturate(sample));

        packed = (static_cast<s32>(*bytes) << 24) & 0xF0000000;
        s32 next = (packed >> shift) + ((first * sample + second * previous1 + 32) >> 6);
        *(out++) = static_cast<s16>(Saturate(next));

        previous2 = sample;
        previous1 = next;
    }
}

// The effect's resampler: 39 taps down and up around the network, which runs at a quarter of the output rate a side
constexpr u32 ResampleTaps = 39;
constexpr s32 DownCoefficients[ResampleTaps] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0,
                                                10246, 16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35,
                                                0, -10, 0, 2, 0, -1};

// The noise generator's two tables: where the next bit comes from, and how the clock frays
constexpr u8 NoiseAdd[64] = {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                             0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};
constexpr u16 NoiseFrequencyAdd[5] = {0, 84, 140, 180, 210};

// Core 0's output is the external input of core 1, scaled by core 1's external volume. It reaches core 1's effect sum, and
// -- this is the one place the host's sum is arranged rather than read off the hardware -- its dry sum as well: the voices
// a game puts on core 0 have to be heard, and the volume register that scales them is called the external input volume.
constexpr bool ExternalInputToDry = true;

s32 Clamp16(s32 sample)
{
    return Saturate(sample);
}
}

const s16 (*SdlAudio::Spu2::InterpolationTable())[4]
{
    return g_InterpolationTable.values;
}

void SdlAudio::Spu2::VolumeSlide::Set(u16 source)
{
    value = source;
    if (!enable)
    {
        // The register's volume as the multiply takes it: the 15 bit magnitude in 16 bits, so 0x4000 is the other phase at
        // full and 0x8000 wraps to silence
        level = static_cast<s16>(static_cast<u16>(source << 1));
    }
}

void SdlAudio::Spu2::VolumeSlide::Step()
{
    if (!enable)
    {
        return;
    }

    s32 stepSize = 7 - static_cast<s32>(step);
    if (decrease)
    {
        stepSize = ~stepSize;
    }

    u32 counterStep = static_cast<u32>(shift) > 11 ? 0x8000u >> (shift - 11) : 0x8000u;
    s32 levelStep = static_cast<s32>(stepSize) * (static_cast<u32>(shift) < 11 ? (1 << (11 - shift)) : 1);

    if (exponential)
    {
        if (!decrease && level > 0x6000)
        {
            counterStep >>= 2;
        }

        if (decrease)
        {
            levelStep = static_cast<s16>((levelStep * level) >> 15);
        }
    }

    // The counter stands still only when every bit of the rate is set: the slide is then over rather than never ending
    if (step != 3 || shift != 0x1F)
    {
        counterStep = counterStep < 1 ? 1 : counterStep;
    }

    counter += counterStep;

    // A negative phase counts its level up to -0x8000 and down towards 0, unless the exponential decrease is what moves it
    if (!(exponential && decrease))
    {
        levelStep = phase ? -levelStep : levelStep;
    }

    if (counter >= 0x8000)
    {
        counter = 0;
        if (!decrease)
        {
            level = Clamp16(level + levelStep);
        }
        else
        {
            s32 low = phase ? -32768 : 0;
            s32 high = phase ? 0 : 32767;
            if (exponential)
            {
                low = 0;
                high = 32767;
            }

            s32 moved = level + levelStep;
            level = moved < low ? low : (moved > high ? high : moved);
        }
    }
}

void SdlAudio::Spu2::Envelope::Cache()
{
    // Attack rises to full, decay falls to the sustain level, sustain moves either way until the voice is let go, release
    // falls to nothing. The steps are the rate field's own: the top two bits are which of four steps, the rest how far
    phases[Attack].decrease = false;
    phases[Attack].exponential = attackExponential != 0;
    phases[Attack].shift = static_cast<u8>(attackShift);
    phases[Attack].step = static_cast<s8>(7 - static_cast<s32>(attackStep));
    phases[Attack].target = FullEnvelope;

    phases[Decay].decrease = true;
    phases[Decay].exponential = true;
    phases[Decay].shift = static_cast<u8>(decayShift);
    phases[Decay].step = -8;
    phases[Decay].target = (static_cast<s32>(sustainLevel) + 1) << 11;

    phases[Sustain].decrease = sustainDecrease != 0;
    phases[Sustain].exponential = sustainExponential != 0;
    phases[Sustain].shift = static_cast<u8>(sustainShift);
    phases[Sustain].step = static_cast<s8>(7 - static_cast<s32>(sustainStep));
    phases[Sustain].target = 0;
    if (phases[Sustain].decrease)
    {
        phases[Sustain].step = static_cast<s8>(~static_cast<s32>(phases[Sustain].step));
    }

    phases[Release].decrease = true;
    phases[Release].exponential = releaseExponential != 0;
    phases[Release].shift = static_cast<u8>(releaseShift);
    phases[Release].step = -8;
    phases[Release].target = 0;
}

bool SdlAudio::Spu2::Envelope::Step()
{
    const Cached& current = phases[phase];
    if (phase == Stopped)
    {
        level = 0;
        return false;
    }

    // The rate field picks how often the level moves against how far it moves: every step of the shift doubles one and
    // halves the other, so the rate as a whole is four steps per doubling
    u32 counterStep = static_cast<u32>(current.shift) > 11 ? 0x8000u >> (current.shift - 11) : 0x8000u;
    s32 levelStep = static_cast<s32>(current.step) * (static_cast<u32>(current.shift) < 11 ? (1 << (11 - current.shift)) : 1);

    if (current.exponential)
    {
        // Rising exponentially is slow at first and quick near the top; falling exponentially covers less and less ground
        if (!current.decrease && level > 0x6000)
        {
            counterStep >>= 2;
        }

        if (current.decrease)
        {
            levelStep = static_cast<s16>((levelStep * level) >> 15);
        }
    }

    // A rate of nothing would hold the voice for good, so the counter always takes at least one step
    counterStep = counterStep < 1 ? 1 : counterStep;
    counter += counterStep;

    if (counter >= 0x8000)
    {
        counter = 0;
        s32 moved = level + levelStep;
        level = moved < 0 ? 0 : (moved > FullEnvelope ? FullEnvelope : moved);
    }

    // Sustain has no target: it holds until the voice is let go or runs into silence
    if (phase == Sustain)
    {
        return level != 0;
    }

    if ((!current.decrease && level >= current.target) || (current.decrease && level <= current.target))
    {
        phase++;
    }

    if (phase > Release)
    {
        phase = Stopped;
        level = 0;
        return false;
    }

    return true;
}

void SdlAudio::Spu2::Envelope::KeyOn()
{
    phase = Attack;
    counter = 0;
    level = 0;
}

void SdlAudio::Spu2::Envelope::KeyOff()
{
    if (phase != Stopped)
    {
        phase = Release;
        counter = 0;
    }
}

void SdlAudio::Spu2::Voice::Start()
{
    // The start is already on a block: every write of it goes through SetVoiceStart, which masks it with BlockAlign.
    // (The old round-to-the-next-block branch here could never run, and added the mask itself rather than a block size)
    envelope.KeyOn();
    manualLoopStart = 0;
    sampleAddress = 0;
    blockFlags = 0;
    next = start | 1;
    // The loop point is this play's own start until the sound's data names another: a voice that kept the last play's (a music
    // ring, say) would wrap into memory that is not its own, and be heard as another sound at another pitch
    loopStart = start;
    previous1 = 0;
    previous2 = 0;
    cachedBlock = NoBlock;
    fifoRead = 0;
    fifoWrite = 0;
    out = 0;
    held = false;
    dataLoop = false;
}

void SdlAudio::Spu2::Voice::Stop()
{
    envelope.level = 0;
    envelope.phase = Envelope::Stopped;
}

void SdlAudio::Spu2::Core::Reset()
{
    *this = Core();
}

void SdlAudio::Spu2::Device::Reset()
{
    for (Core& core : m_cores)
    {
        core.Reset();
        for (Voice& voice : core.voices)
        {
            voice = Voice();
        }
    }

    for (s32 word = 0; word < static_cast<s32>(MemoryWords); word++)
    {
        m_memory[word] = 0;
    }

    m_cycles = 0;
}

void SdlAudio::Spu2::Device::WriteMemory(u32 wordAddress, const void* bytes, u32 byteCount)
{
    u32 words = byteCount / 2;
    if (wordAddress >= MemoryWords)
    {
        return;
    }

    if (wordAddress + words > MemoryWords)
    {
        words = MemoryWords - wordAddress;
    }

    const s16* source = static_cast<const s16*>(bytes);
    for (u32 word = 0; word < words; word++)
    {
        m_memory[wordAddress + word] = source[word];
    }
}

void SdlAudio::Spu2::Device::UpdateNoise(Core& core)
{
    u32 level = 0x8000u >> (core.noiseClock >> 2);
    level <<= 16;

    core.noiseCounter += 0x10000;
    core.noiseCounter += NoiseFrequencyAdd[core.noiseClock & 3];
    if ((core.noiseCounter & 0xFFFF) >= NoiseFrequencyAdd[4])
    {
        core.noiseCounter += 0x10000;
        core.noiseCounter -= NoiseFrequencyAdd[core.noiseClock & 3];
    }

    if (core.noiseCounter >= level && level != 0)
    {
        while (core.noiseCounter >= level)
        {
            core.noiseCounter -= level;
        }

        core.noiseOutput = (core.noiseOutput << 1) | NoiseAdd[(core.noiseOutput >> 10) & 63];
    }
}

void SdlAudio::Spu2::Device::UpdateBlockHeader(s32 core, s32 index)
{
    Voice& voice = m_cores[core].voices[index];
    voice.blockFlags = static_cast<u8>(static_cast<u16>(m_memory[voice.next & BlockAlign]) >> 8);
    if ((voice.blockFlags & BlockLoopStart) && !voice.manualLoopStart)
    {
        voice.loopStart = voice.next & BlockAlign;
    }
}

void SdlAudio::Spu2::Device::NextData(s32 core, s32 index)
{
    Voice& voice = m_cores[core].voices[index];
    u32 blockAddress = voice.next & BlockAlign;
    if (voice.cachedBlock != blockAddress)
    {
        DecodeBlock(&m_memory[blockAddress], voice.block, voice.previous1, voice.previous2);
        voice.cachedBlock = blockAddress;
        voice.previous1 = voice.block[27];
        voice.previous2 = voice.block[26];
    }

    // The block's first word is its header, so the quarter the address is on is a quarter of the 28 samples after it
    s32 first = (static_cast<s32>(voice.next % BlockWords) - 1) * 4;
    for (s32 sample = 0; sample < 4; sample++)
    {
        voice.fifo[(voice.fifoWrite + sample) % 32] = voice.block[first + sample];
    }
}

void SdlAudio::Spu2::Device::DecodeSamples(s32 core, s32 index)
{
    Voice& voice = m_cores[core].voices[index];

    // The voice reads a block ahead; once this much is buffered there is nothing to fetch
    if (static_cast<s32>(voice.fifoWrite - voice.fifoRead) > 12)
    {
        return;
    }

    // The block's header is read for the block about to be fetched, and only when something is fetched: the memory is the 2 MB
    // table and the read walks into it cold, so reading it on a call that returns at once (the common one, a voice buffered ahead)
    // was a memory read a four samples per voice that told nothing new -- the flags only move on with the block, which the fetch
    // below is what advances
    UpdateBlockHeader(core, index);

    if (voice.Active())
    {
        NextData(core, index);
    }

    voice.fifoWrite += 4;
    voice.next++;

    if ((voice.next & 7) == 0)
    {
        // The block was the last of the loop: back to the loop point, or the end of the sound. A block with the end flag
        // but not the loop flag is where a one-shot sound finishes, and the voice stops there.
        if ((voice.blockFlags & BlockLoopEnd) != 0)
        {
            voice.next = voice.loopStart;
            if ((voice.blockFlags & BlockLoop) == 0)
            {
                voice.Stop();
            }
        }
        else if (voice.dataEnd != 0 && voice.next >= voice.dataEnd)
        {
            // The data carried no end flag, and the sound is over. The module knows its length, so the voice goes back to its
            // loop point if the play asked for more than one pass, and stops otherwise - rather than reading on into whatever
            // follows it in the memory (another sound's samples, heard as noise)
            if (voice.dataLoop)
            {
                voice.next = voice.loopStart;
            }
            else
            {
                voice.Stop();
            }
        }

        voice.next++;
        voice.cachedBlock = Voice::NoBlock;
    }
}

s32 SdlAudio::Spu2::Device::VoiceSamples(s32 core, s32 index)
{
    Voice& voice = m_cores[core].voices[index];
    const s16(*table)[4] = g_InterpolationTable.values;
    s32 phase = (voice.sampleAddress & 0xFF0) >> 4;

    s32 out = 0;
    out += (table[phase][0] * voice.fifo[(voice.fifoRead + 0) % 32]) >> 15;
    out += (table[phase][1] * voice.fifo[(voice.fifoRead + 1) % 32]) >> 15;
    out += (table[phase][2] * voice.fifo[(voice.fifoRead + 2) % 32]) >> 15;
    out += (table[phase][3] * voice.fifo[(voice.fifoRead + 3) % 32]) >> 15;
    return out;
}

s32 SdlAudio::Spu2::Device::MixVoice(s32 coreIndex, s32 index, s32& dryLeft, s32& dryRight, s32& effectLeft, s32& effectRight)
{
    Core& core = m_cores[coreIndex];
    Voice& voice = core.voices[index];

    // Held where it is: its place, its envelope and its volumes stay put and it says nothing, which is what the module's pause
    // does to the music. Nothing is let go of, so a resume carries on from the same sample.
    if (voice.held)
    {
        return 0;
    }

    voice.volumeLeft.Step();
    voice.volumeRight.Step();

    DecodeSamples(coreIndex, index);

    s32 value = 0;
    s32 outLeft = 0;
    s32 outRight = 0;
    if (voice.Active())
    {
        value = voice.useNoise ? static_cast<s16>(core.noiseOutput) : VoiceSamples(coreIndex, index);

        // The envelope moves after the sample is read, then scales it: a voice that has run out contributes nothing
        if (!voice.envelope.Step())
        {
            voice.Stop();
        }

        value = ApplyVolume(value, voice.envelope.level);
        voice.out = value;

        outLeft = ApplyVolume(value, voice.volumeLeft.level);
        outRight = ApplyVolume(value, voice.volumeRight.level);
    }

    // The pitch counter, the voice before this one able to bend it by its own output
    s32 pitch = voice.pitch;
    if (voice.pitchModulated && index > 0)
    {
        pitch = (voice.pitch * (32768 + core.voices[index - 1].out)) >> 15;
        pitch = pitch < 0 ? 0 : (pitch > 0x3FFF ? 0x3FFF : pitch);
    }

    pitch = pitch > 0x3FFF ? 0x3FFF : pitch;
    voice.sampleAddress += pitch;

    s32 consumed = voice.sampleAddress >> 12;
    voice.sampleAddress &= 0xFFF;
    voice.fifoRead += consumed;

    u32 bit = 1u << index;
    if ((core.dryLeft & bit) != 0)
    {
        dryLeft += outLeft;
    }

    if ((core.dryRight & bit) != 0)
    {
        dryRight += outRight;
    }

    if ((core.effectLeft & bit) != 0)
    {
        effectLeft += outLeft;
    }

    if ((core.effectRight & bit) != 0)
    {
        effectRight += outRight;
    }

    return value;
}

// Where in the effect area a register's line is this half frame: the area is a ring the network walks a word at a time,
// twice the output rate
s32 ReverbIndex(const Core& core, u32 cycles, s32 offset)
{
    u32 start = core.effectStart & 0x3FFFFF;
    u32 end = (core.effectEnd & 0x3FFFFF) | 0xFFFF;
    u32 span = (end - start) + 1;
    if (span == 0)
    {
        return 0;
    }

    u32 index = ((cycles >> 1) + static_cast<u32>(offset)) % span;
    return static_cast<s32>((index + start) & 0xFFFFF);
}

s32 SdlAudio::Spu2::Device::Downsample(Core& core, bool right)
{
    s32 index = (static_cast<s32>(core.resamplePosition) - static_cast<s32>(ResampleTaps)) & 63;
    s32 out = 0;
    for (u32 tap = 0; tap < ResampleTaps; tap++)
    {
        out += core.downsample[right ? 1 : 0][index + tap] * DownCoefficients[tap];
    }

    return Saturate(out >> 15);
}

void SdlAudio::Spu2::Device::Upsample(Core& core, s32& left, s32& right)
{
    s32 index = (static_cast<s32>(core.resamplePosition) - static_cast<s32>(ResampleTaps)) & 63;
    s32 leftOut = 0;
    s32 rightOut = 0;
    for (u32 tap = 0; tap < ResampleTaps; tap++)
    {
        s32 coefficient = DownCoefficients[tap] * 2;
        coefficient = coefficient > 32767 ? 32767 : (coefficient < -32768 ? -32768 : coefficient);
        leftOut += core.upsample[0][index + tap] * coefficient;
        rightOut += core.upsample[1][index + tap] * coefficient;
    }

    left = Saturate(leftOut >> 15);
    right = Saturate(rightOut >> 15);
}

// One half frame of a core's network: the reflections, the four comb taps and the two all-pass stages, exactly as published
void DoReverb(Device& device, Core& core, s32 inputLeft, s32 inputRight, s32& wetLeft, s32& wetRight)
{
    if (core.effectStart >= core.effectEnd || !core.reverbOn)
    {
        wetLeft = 0;
        wetRight = 0;
        return;
    }

    s16* memory = device.Memory();
    const ReverbRegisters& regs = core.reverb;

    s16 clampLeft = static_cast<s16>(Saturate(inputLeft));
    s16 clampRight = static_cast<s16>(Saturate(inputRight));
    core.downsample[0][core.resamplePosition] = clampLeft;
    core.downsample[0][core.resamplePosition | 64] = clampLeft;
    core.downsample[1][core.resamplePosition] = clampRight;
    core.downsample[1][core.resamplePosition | 64] = clampRight;

    // Each output frame runs one side of the network, the left on even frames
    bool right = (device.Cycles() & 1) != 0;
    u32 cycles = device.Cycles();

    s32 sameSource = ReverbIndex(core, cycles, static_cast<s32>(right ? regs.sameRightSource : regs.sameLeftSource));
    s32 sameTarget = ReverbIndex(core, cycles, static_cast<s32>(right ? regs.sameRightTarget : regs.sameLeftTarget));
    s32 samePrevious = ReverbIndex(core, cycles, static_cast<s32>(right ? regs.sameRightTarget - 1 : regs.sameLeftTarget - 1));

    s32 differentSource =
        ReverbIndex(core, cycles, static_cast<s32>(right ? regs.differentLeftSource : regs.differentRightSource));
    s32 differentTarget =
        ReverbIndex(core, cycles, static_cast<s32>(right ? regs.differentRightTarget : regs.differentLeftTarget));
    s32 differentPrevious =
        ReverbIndex(core, cycles, static_cast<s32>(right ? regs.differentRightTarget - 1 : regs.differentLeftTarget - 1));

    s32 combSource[4];
    for (s32 comb = 0; comb < 4; comb++)
    {
        combSource[comb] = ReverbIndex(core, cycles, static_cast<s32>(regs.combSource[comb][right ? 1 : 0]));
    }

    s32 apf1Source = ReverbIndex(core, cycles, static_cast<s32>((right ? regs.apfTarget[0][1] : regs.apfTarget[0][0]) -
                                                                regs.apf1Size));
    s32 apf1Target = ReverbIndex(core, cycles, static_cast<s32>(right ? regs.apfTarget[0][1] : regs.apfTarget[0][0]));
    s32 apf2Source = ReverbIndex(core, cycles, static_cast<s32>((right ? regs.apfTarget[1][1] : regs.apfTarget[1][0]) -
                                                                regs.apf2Size));
    s32 apf2Target = ReverbIndex(core, cycles, static_cast<s32>(right ? regs.apfTarget[1][1] : regs.apfTarget[1][0]));

    s32 in = ApplyVolume(right ? regs.inputRight : regs.inputLeft, device.Downsample(core, right));

    s32 same = ApplyVolume(regs.iirVolume, in + ApplyVolume(regs.wallVolume, memory[sameSource]) - memory[samePrevious]) +
               memory[samePrevious];
    s32 different =
        ApplyVolume(regs.iirVolume, in + ApplyVolume(regs.wallVolume, memory[differentSource]) - memory[differentPrevious]) +
        memory[differentPrevious];

    s32 out = 0;
    for (s32 comb = 0; comb < 4; comb++)
    {
        out += ApplyVolume(regs.combVolume[comb], memory[combSource[comb]]);
    }

    s32 apf1 = out - ApplyVolume(regs.apf1Volume, memory[apf1Source]);
    out = memory[apf1Source] + ApplyVolume(regs.apf1Volume, apf1);
    s32 apf2 = out - ApplyVolume(regs.apf2Volume, memory[apf2Source]);
    out = memory[apf2Source] + ApplyVolume(regs.apf2Volume, apf2);

    memory[sameTarget] = static_cast<s16>(Saturate(same));
    memory[differentTarget] = static_cast<s16>(Saturate(different));
    memory[apf1Target] = static_cast<s16>(Saturate(apf1));
    memory[apf2Target] = static_cast<s16>(Saturate(apf2));

    out = Saturate(out);

    core.upsample[right ? 1 : 0][core.resamplePosition] = static_cast<s16>(out);
    core.upsample[right ? 0 : 1][core.resamplePosition] = 0;
    core.upsample[right ? 1 : 0][core.resamplePosition | 64] = static_cast<s16>(out);
    core.upsample[right ? 0 : 1][core.resamplePosition | 64] = 0;

    core.resamplePosition = (core.resamplePosition + 1) & 63;

    device.Upsample(core, wetLeft, wetRight);
}

void SdlAudio::Spu2::Device::MixCore(Core& core, s32 dryLeft, s32 dryRight, s32 effectLeft, s32 effectRight, s32 externalLeft,
                              s32 externalRight, s32& outLeft, s32& outRight)
{
    core.masterLeft.Step();
    core.masterRight.Step();
    UpdateNoise(core);

    // The voices' two sums are held to the range before the effect sees them
    s32 voicesLeft = Saturate(dryLeft);
    s32 voicesRight = Saturate(dryRight);
    s32 sendLeft = Saturate(effectLeft);
    s32 sendRight = Saturate(effectRight);

    // What core 0 hands this core is part of the dry sum only, not the effect send: its voices are heard here but not fed into
    // the reverb network. Retail's MMIX value after libsd init is 0xFFC for core 1, which gates Ext to the dry (bits 2/3 set)
    // and off from the effect send (bits 0/1 clear)
    s32 dryOutLeft = voicesLeft + (ExternalInputToDry ? externalLeft : 0);
    s32 dryOutRight = voicesRight + (ExternalInputToDry ? externalRight : 0);

    s32 wetLeft = 0;
    s32 wetRight = 0;
    DoReverb(*this, core, sendLeft, sendRight, wetLeft, wetRight);

    outLeft = dryOutLeft + ApplyVolume(wetLeft, core.effectVolumeLeft);
    outRight = dryOutRight + ApplyVolume(wetRight, core.effectVolumeRight);
}

void SdlAudio::Spu2::Device::Mix(s32& left, s32& right)
{
    s32 coreOut[CoreCount][2] = {};

    for (s32 index = 0; index < CoreCount; index++)
    {
        s32 dryLeft = 0;
        s32 dryRight = 0;
        s32 effectLeft = 0;
        s32 effectRight = 0;
        for (s32 voice = 0; voice < CoreVoices; voice++)
        {
            MixVoice(index, voice, dryLeft, dryRight, effectLeft, effectRight);
        }

        // Core 0's output is the external input of core 1, through each core's volume for it
        s32 externalLeft = 0;
        s32 externalRight = 0;
        if (index == 1)
        {
            externalLeft = ApplyVolume(Saturate(coreOut[0][0]), m_cores[0].masterLeft.level);
            externalLeft = ApplyVolume(externalLeft, m_cores[1].externalVolumeLeft);
            externalRight = ApplyVolume(Saturate(coreOut[0][1]), m_cores[0].masterRight.level);
            externalRight = ApplyVolume(externalRight, m_cores[1].externalVolumeRight);
        }

        MixCore(m_cores[index], dryLeft, dryRight, effectLeft, effectRight, externalLeft, externalRight, coreOut[index][0],
                coreOut[index][1]);
    }

    // The DAC takes core 1's output, held to the range: the master volumes are the last thing applied
    left = Saturate(ApplyVolume(Saturate(coreOut[1][0]), m_cores[1].masterLeft.level));
    right = Saturate(ApplyVolume(Saturate(coreOut[1][1]), m_cores[1].masterRight.level));

    m_cycles++;
}

SdlAudio::Spu2::Device& SdlAudio::Spu2::SoundProcessor()
{
    static Device device;
    return device;
}

// ---- the registers ----

namespace
{
// A voice's index across the two cores: the game names them 0 to 47, 24 to a core
inline Voice& VoiceAt(Device& device, s32 index)
{
    return device.GetCore(index / CoreVoices).voices[index % CoreVoices];
}
}

void SdlAudio::Spu2::Device::SetVoiceVolume(s32 voice, u16 left, u16 right)
{
    Voice& target = VoiceAt(*this, voice);
    target.volumeLeft.Set(left);
    target.volumeRight.Set(right);
}

void SdlAudio::Spu2::Device::SetVoicePitch(s32 voice, u16 pitch)
{
    VoiceAt(*this, voice).pitch = pitch;
}

void SdlAudio::Spu2::Device::SetVoiceEnvelope(s32 voice, u16 attackDecay, u16 sustainRelease)
{
    Voice& target = VoiceAt(*this, voice);
    target.envelope.attackDecay = attackDecay;
    target.envelope.sustainRelease = sustainRelease;
    target.envelope.Cache();
}

void SdlAudio::Spu2::Device::SetVoiceStart(s32 voice, u32 wordAddress)
{
    VoiceAt(*this, voice).start = wordAddress & BlockAlign;
}

void SdlAudio::Spu2::Device::SetVoiceLoopStart(s32 voice, u32 wordAddress)
{
    Voice& target = VoiceAt(*this, voice);
    // Written by hand, so the block flags' own loop point does not take over for this play
    target.manualLoopStart = 1;
    target.loopStart = wordAddress & BlockAlign;
}

void SdlAudio::Spu2::Device::SetVoiceNoise(s32 voice, bool noise)
{
    VoiceAt(*this, voice).useNoise = noise;
}

void SdlAudio::Spu2::Device::SetVoiceDataEnd(s32 voice, u32 wordAddress)
{
    VoiceAt(*this, voice).dataEnd = wordAddress;
}

void SdlAudio::Spu2::Device::SetVoiceDataLoop(s32 voice, bool loop)
{
    VoiceAt(*this, voice).dataLoop = loop;
}

void SdlAudio::Spu2::Device::SetVoiceHeld(s32 voice, bool held)
{
    VoiceAt(*this, voice).held = held;
}

bool SdlAudio::Spu2::Device::VoiceHeld(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return false;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].held;
}

void SdlAudio::Spu2::Device::SetDrySides(s32 core, u32 left, u32 right)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].dryLeft = left & 0xFFFFFF;
    m_cores[core].dryRight = right & 0xFFFFFF;
}

void SdlAudio::Spu2::Device::SetVoiceEffectSend(s32 core, s32 voice, bool on)
{
    if (core < 0 || core >= CoreCount || voice < 0 || voice >= CoreVoices)
    {
        return;
    }

    u32 bit = 1u << voice;
    Core& target = m_cores[core];
    if (on)
    {
        target.effectLeft |= bit;
        target.effectRight |= bit;
    }
    else
    {
        target.effectLeft &= ~bit;
        target.effectRight &= ~bit;
    }
}

void SdlAudio::Spu2::Device::KeyOn(s32 voice)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return;
    }

    VoiceAt(*this, voice).Start();
}

void SdlAudio::Spu2::Device::KeyOff(s32 voice)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return;
    }

    VoiceAt(*this, voice).envelope.KeyOff();
}

bool SdlAudio::Spu2::Device::VoiceActive(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return false;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].Active();
}

s32 SdlAudio::Spu2::Device::VoiceEnvelope(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return 0;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].envelope.level;
}

u32 SdlAudio::Spu2::Device::VoiceNext(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return 0;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].next;
}

u32 SdlAudio::Spu2::Device::VoiceStart(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return 0;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].start;
}

u32 SdlAudio::Spu2::Device::VoiceLoopStart(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return 0;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].loopStart;
}

u32 SdlAudio::Spu2::Device::VoiceDataEnd(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return 0;
    }

    return m_cores[voice / CoreVoices].voices[voice % CoreVoices].dataEnd;
}

void SdlAudio::Spu2::Device::MoveVoice(s32 voice, s32 delta)
{
    Voice& target = VoiceAt(*this, voice);
    target.start = static_cast<u32>(static_cast<s32>(target.start) + delta);
    // The window the module gave the voice goes with it: a loop point left behind sends the sound back to where its data
    // used to be, and an end left behind lets it read on past its own samples into whatever is under it now
    target.loopStart = static_cast<u32>(static_cast<s32>(target.loopStart) + delta);
    if (target.dataEnd != 0)
    {
        target.dataEnd = static_cast<u32>(static_cast<s32>(target.dataEnd) + delta);
    }

    if (target.Active())
    {
        target.next = static_cast<u32>(static_cast<s32>(target.next) + delta);
        target.cachedBlock = Voice::NoBlock;
    }
}

void SdlAudio::Spu2::Device::SetVoiceRelease(s32 voice, u32 rate)
{
    Voice& target = VoiceAt(*this, voice);
    target.envelope.sustainRelease =
        static_cast<u16>((target.envelope.sustainRelease & ~0x1Fu) | (rate & 0x1F));
    target.envelope.Cache();
}

void SdlAudio::Spu2::Device::SetMasterVolume(s32 core, u16 left, u16 right)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].masterLeft.Set(left);
    m_cores[core].masterRight.Set(right);
}

void SdlAudio::Spu2::Device::SetEffectVolume(s32 core, u16 left, u16 right)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].effectVolumeLeft = static_cast<s16>(left);
    m_cores[core].effectVolumeRight = static_cast<s16>(right);
}

void SdlAudio::Spu2::Device::SetExternalVolume(s32 core, u16 left, u16 right)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].externalVolumeLeft = static_cast<s16>(left);
    m_cores[core].externalVolumeRight = static_cast<s16>(right);
}

void SdlAudio::Spu2::Device::SetNoiseClock(s32 core, u8 clock)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].noiseClock = static_cast<u8>(clock & 0x3F);
}

void SdlAudio::Spu2::Device::SetEffectArea(s32 core, u32 startWord, u32 endWord)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].effectStart = startWord;
    m_cores[core].effectEnd = endWord;
}

void SdlAudio::Spu2::Device::SetEffectEnabled(s32 core, bool on)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].reverbOn = on;
}

void SdlAudio::Spu2::Device::SetEffects(s32 core, const ReverbRegisters& registers)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_cores[core].reverb = registers;
}
