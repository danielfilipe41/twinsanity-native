// The sound module: the table of sounds in memory, the channels and their key status, the volume groups, the effect per core
// and the memory's compaction. See soundmodule.h for what the game expects of it.
#include "soundmodule.h"

#include "platform/stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using namespace SdlAudio::Spu2;

// The effect modes, as the game names them (include/game/sound.h's ReverbMode): 0 off, then the SPU's ten
constexpr s32 EffectModes = 10;

// A mode's work area in bytes (retail g_MsEffectSizesByMode, asm/data/data.data.s D_002E7D38: the SPU's published preset
// sizes)
constexpr u32 EffectWorkBytes[EffectModes] = {0x80, 0x26C0, 0x1F40, 0x4840, 0x6FE0,
                                             0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};

// A mode's thirty two register words (APF sizes, volumes, wall, APF volumes, same/different/comb/APF addresses, input
// drive): Sony's own table from the disc's LIBSD.IRX, one register set per mode, mode 0 the published "off" dummy
constexpr u16 EffectPresets[EffectModes][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001,
     0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
     0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333,
     0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5,
     0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B,
     0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
     0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B,
     0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
     0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58,
     0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
     0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB,
     0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
     0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31,
     0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
     0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD,
     0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
     0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD,
     0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
     0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF,
     0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
     0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

// The full setting of the game's delay and feedback knobs
constexpr s32 FullSetting = 127;

// TS_AUDIOLOG or TS_SOUNDLOG: what the module is asked to do, so a run's own sound can be followed - which sound was loaded
// where, what every play was handed, when a voice was let go of and what the memory came to. A measurement (a sound that is
// too loud, a voice that never comes back) is a line this log does not answer for
bool ModuleLogOn()
{
    static const bool on = std::getenv("TS_AUDIOLOG") != nullptr || std::getenv("TS_SOUNDLOG") != nullptr;
    return on;
}

// A volume ramp's step in the mix's level: the whole range in about ten milliseconds, which is what the module's own smooth
// volume change is for (a step and a start the game hears as a fade, never as a click)
constexpr s32 VolumeRampStep = 32768 / 480;

// A music voice's envelope: full throughout, its sustain held at its slowest so the level does not drift under a track, and a
// release the same rate the module lets a track go at - a release rate of nothing is the chip's fastest, which cuts a track
// off with a click
constexpr u16 StreamEnvelope = 0x5FED;

// One frame of a level's way to its target
s32 RampTowards(s32 level, s32 target)
{
    s32 difference = target - level;
    if (difference > VolumeRampStep)
    {
        return level + VolumeRampStep;
    }

    if (difference < -VolumeRampStep)
    {
        return level - VolumeRampStep;
    }

    return target;
}

// How a register's line offset becomes a distance in the work area: the register holds the address divided by eight, so the
// offset is eight bytes a unit, and the delay knob draws the lines out from half their length to all of it
u32 EffectOffset(u16 value, u16 delay, u32 words)
{
    u32 scaled = static_cast<u32>(value) * 4;
    scaled = scaled * (static_cast<u32>(delay) + FullSetting) / (2 * FullSetting);
    return words == 0 ? 0 : scaled % words;
}

// A preset, with the game's knobs over it: the input drive is the depth, the wall reflection the feedback, the line lengths the
// delay. The addresses are the work area's own, which sits at start.
ReverbRegisters ProgramEffect(const u16* preset, const CoreEffect& effect, u32 start, u32 words)
{
    ReverbRegisters registers;
    auto line = [&](u16 value) { return start + EffectOffset(value, effect.delay, words); };

    registers.apf1Size = EffectOffset(preset[0], effect.delay, words);
    registers.apf2Size = EffectOffset(preset[1], effect.delay, words);
    registers.iirVolume = static_cast<s16>(preset[2]);
    for (s32 comb = 0; comb < 4; comb++)
    {
        registers.combVolume[comb] = static_cast<s16>(preset[3 + comb]);
    }

    registers.wallVolume =
        static_cast<s16>(static_cast<s32>(static_cast<s16>(preset[7])) * effect.feedback / FullSetting);
    registers.apf1Volume = static_cast<s16>(preset[8]);
    registers.apf2Volume = static_cast<s16>(preset[9]);

    registers.sameLeftTarget = line(preset[10]);
    registers.sameRightTarget = line(preset[11]);
    registers.combSource[0][0] = line(preset[12]);
    registers.combSource[0][1] = line(preset[13]);
    registers.combSource[1][0] = line(preset[14]);
    registers.combSource[1][1] = line(preset[15]);
    registers.sameLeftSource = line(preset[16]);
    registers.sameRightSource = line(preset[17]);
    registers.differentLeftTarget = line(preset[18]);
    registers.differentRightTarget = line(preset[19]);
    registers.combSource[2][0] = line(preset[20]);
    registers.combSource[2][1] = line(preset[21]);
    registers.combSource[3][0] = line(preset[22]);
    registers.combSource[3][1] = line(preset[23]);
    registers.differentLeftSource = line(preset[24]);
    registers.differentRightSource = line(preset[25]);
    registers.apfTarget[0][0] = line(preset[26]);
    registers.apfTarget[0][1] = line(preset[27]);
    registers.apfTarget[1][0] = line(preset[28]);
    registers.apfTarget[1][1] = line(preset[29]);

    // The preset's own input drive is full; the game's depth is what the effect is fed
    registers.inputLeft = static_cast<s16>(effect.depthLeft);
    registers.inputRight = static_cast<s16>(effect.depthRight);

    return registers;
}

// The envelope a played sound gets: the attack the sound asks for, a decay to full at once, a sustain that holds, and the
// release the play asks for, falling exponentially the way a let-go voice does
void SoundEnvelope(u32 attack, u32 release, u16& attackDecay, u16& sustainRelease)
{
    // Sustain level 0xF (full), the decay's rate at its fastest
    constexpr u32 SustainFull = 0xF;
    constexpr u32 DecayFastest = 0;
    attackDecay = static_cast<u16>(SustainFull | (DecayFastest << 4) | ((attack & 0x7F) << 8));

    // The release's rate, exponential, the sustain held at its slowest so it does not drift before the key-off
    constexpr u32 SustainHold = 0x1F;
    constexpr u32 SustainStepSlowest = 3;
    constexpr u32 ReleaseExponential = 1;
    constexpr u32 SustainDecreases = 1;
    sustainRelease = static_cast<u16>((release & 0x1F) | (ReleaseExponential << 5) | (SustainStepSlowest << 6) |
                                      (SustainHold << 8) | (SustainDecreases << 14));
}
}

SdlAudio::Spu2::SoundModule& SdlAudio::Spu2::Module()
{
    static SoundModule module;
    return module;
}

void SdlAudio::Spu2::SoundModule::Reset()
{
    Device& device = SoundProcessor();
    device.Reset();

    m_memory.Reset(MemoryWords);

    for (Sound& sound : m_sounds)
    {
        sound = Sound();
    }

    for (Channel& channel : m_channels)
    {
        channel = Channel();
    }

    for (MusicStream& stream : m_streams)
    {
        stream = MusicStream();
    }

    m_pendingFile = -1;
    m_pendingOffset = 0;
    m_pendingSize = 0;

    for (CoreEffect& effect : m_effects)
    {
        effect = CoreEffect();
    }

    for (s32 group = 0; group < GroupCount; group++)
    {
        m_groupLeft[group] = FullGroupVolume;
        m_groupRight[group] = FullGroupVolume;
    }

    m_paused = false;
    m_lent = false;

    // What libsd sets when it starts the processor over: both cores' master volumes full, every voice dry to both sides,
    // every effect send off, the effect's return silent until a mode is chosen
    for (s32 core = 0; core < CoreCount; core++)
    {
        device.SetMasterVolume(core, 0x3FFF, 0x3FFF);
        // Core 0 hands core 1 everything it plays: the volume for that is a plain 15 bit register, full at 0x7FFF, not the
        // 0x3FFF a voice's volume register uses. Half here is 6 dB off every voice on core 0, which is where the music sits.
        device.SetExternalVolume(core, 0x7FFF, 0x7FFF);
        device.SetEffectVolume(core, 0, 0);
        device.SetEffectEnabled(core, false);
        device.SetDrySides(core, 0xFFFFFF, 0xFFFFFF);
        for (s32 voice = 0; voice < CoreVoices; voice++)
        {
            device.SetVoiceEffectSend(core, voice, false);
            device.SetVoiceNoise(core * CoreVoices + voice, false);
        }
    }
}

void SdlAudio::Spu2::SoundModule::Mix(s16* stereo, u32 frames)
{
    // The mix runs whatever the pause says: a paused module has its music held and let its sounds go, but the processor is
    // still mixing, so a sound played during a pause (a menu, the frontend) is heard
    Device& device = SoundProcessor();
    for (u32 frame = 0; frame < frames; frame++)
    {
        StepRamps();

        s32 left = 0;
        s32 right = 0;
        device.Mix(left, right);
        stereo[frame * 2] = static_cast<s16>(left);
        stereo[frame * 2 + 1] = static_cast<s16>(right);

        TrackChannels();
    }

    // The tracks' rings are topped up behind their voices, and a track whose data has run out is over: both once a mix, which
    // is far more often than either needs
    FillStreams();
    TrackStreams();

    // Once a second: how many voices the processor is holding, so voices that never come back show as a climb. A voice the game
    // cannot get free is a play it never makes, which is a menu that has gone quiet
    if (ModuleLogOn())
    {
        m_loggedFrames += frames;
        if (m_loggedFrames >= 48000)
        {
            m_loggedFrames = 0;
            s32 active = 0;
            for (s32 voice = 0; voice < VoiceCount; voice++)
            {
                if (device.VoiceActive(voice))
                {
                    active++;
                }
            }

            s32 held = 0;
            for (s32 voice = 0; voice < VoiceCount; voice++)
            {
                if (m_channels[voice].state != ChannelState::Off)
                {
                    held++;
                }
            }

            // Which ones, not just how many: a stream's four voices dropping to three is one side of a track gone silent,
            // and the numbers alone do not say which
            char which[160];
            s32 at = 0;
            which[0] = 0;
            for (s32 voice = 0; voice < VoiceCount && at < static_cast<s32>(sizeof(which)) - 8; voice++)
            {
                if (device.VoiceActive(voice))
                {
                    at += std::snprintf(which + at, sizeof(which) - static_cast<size_t>(at), "%s%d", at > 0 ? " " : "",
                                        voice);
                }
            }

            std::fprintf(stderr,
                         "audio: %d of %d voices sounding [%s], %d the game has not been told are free, %u/%u memory used\n",
                         active, VoiceCount, which, held, m_memory.Used(), m_memory.Ceiling() - MemoryBase);
        }
    }
}

// ---- the processor's whole state ----

void SdlAudio::Spu2::SoundModule::PauseAll()
{
    // The module's pause: the game's sounds are let go of and the music is held where it is. It is deliberately not a mute of
    // the whole processor - the game pauses and resumes around its own movies and menus, and does not always pair the two calls
    // (a movie that ends into the title screen never resumes). A module that went silent for good on one unmatched pause left
    // the frontend with no menu sounds at all, which is not what retail's does.
    if (m_paused)
    {
        return;
    }

    m_paused = true;
    Device& device = SoundProcessor();

    for (s32 voice = 0; voice < VoiceCount; voice++)
    {
        Channel& channel = m_channels[voice];
        if (channel.state != ChannelState::Off)
        {
            // A sound is let go of, and fades rather than cutting
            device.SetVoiceRelease(voice, 0x0D);
            device.KeyOff(voice);
        }
    }

    // The music keeps its place: its voices are held, silent, and carried on from the same sample when the pause ends
    for (s32 channel = 0; channel < StreamChannels; channel++)
    {
        MusicStream& stream = m_streams[channel];
        // Only a track the module still holds, and only a voice it has actually keyed on: a stream that has been let go of
        // keeps neither, and holding the voice it used to be on froze a voice the game had been given back
        if (!stream.bound || !stream.playing || !stream.started || stream.voice < 0)
        {
            continue;
        }

        device.SetVoiceHeld(stream.voice, true);
        if (stream.childChannel >= 0 && m_streams[stream.childChannel].voice >= 0)
        {
            device.SetVoiceHeld(m_streams[stream.childChannel].voice, true);
        }
    }

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: paused (the sounds let go, the music held where it is)\n");
    }
}

void SdlAudio::Spu2::SoundModule::ResumeAll()
{
    if (!m_paused)
    {
        return;
    }

    m_paused = false;
    Device& device = SoundProcessor();

    // Every hold the processor is under, whichever stream put it there: a track re-bound to another voice while it was
    // paused is not on the channel's voice any more, and asking the streams would have left that voice held for good
    for (s32 voice = 0; voice < VoiceCount; voice++)
    {
        if (device.VoiceHeld(voice))
        {
            device.SetVoiceHeld(voice, false);
        }
    }

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: resumed\n");
    }
}

void SdlAudio::Spu2::SoundModule::LendToMovie()
{
    // Retail takes the module's key-on callback away for the movie and puts it back after. No channel is let go of and no
    // track is stopped: the game's music player is still in its playing state when the module comes back, and nothing in the
    // game will re-start a track it thinks is playing
    m_lent = true;
    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: lent to a movie (no key-ons until it is reclaimed)\n");
    }
}

void SdlAudio::Spu2::SoundModule::ReclaimFromMovie()
{
    m_lent = false;
    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: reclaimed from the movie\n");
    }
}

void SdlAudio::Spu2::SoundModule::CompactSoundMemory()
{
    m_memory.Compact();

    // The samples are the ones that have to move: the arena only knows where a sound's data *should* be. Compaction packs
    // every block down onto the one before it, so every move's destination is at or below the block's own address and copying
    // the moves in the order the arena lists them (ascending) never overwrites a block still waiting to be copied
    Device& device = SoundProcessor();
    s16* memory = device.Memory();
    for (const MemoryMove& move : m_memory.Moves())
    {
        std::memmove(memory + move.to, memory + move.from, static_cast<size_t>(move.size) * sizeof(s16));
    }

    // The table says where each sound's samples are, so every sound whose block moved moves with it: a table left pointing at
    // the address a sound used to be at makes every play after the compaction read whichever sound was moved down onto it -
    // heard as a sound that is missing, or is somebody else's data. The moves are in the arena's own order, so a sound is
    // found by where its block was
    for (u32 sound = 0; sound < SoundSlots; sound++)
    {
        Sound& entry = m_sounds[sound];
        if (!entry.used)
        {
            continue;
        }

        for (const MemoryMove& move : m_memory.Moves())
        {
            if (entry.address >= move.from && entry.address < move.from + move.size)
            {
                entry.address = static_cast<u32>(static_cast<s32>(entry.address) + (static_cast<s32>(move.to) -
                                                                                    static_cast<s32>(move.from)));
                break;
            }
        }
    }

    // A voice reading a block that moved goes with it, or it would read whatever lands there next: the noise a level's loads
    // and unloads leave behind
    u32 moved = 0;
    for (const MemoryMove& move : m_memory.Moves())
    {
        s32 delta = static_cast<s32>(move.to) - static_cast<s32>(move.from);
        for (s32 voice = 0; voice < VoiceCount; voice++)
        {
            Channel& channel = m_channels[voice];
            if (channel.state == ChannelState::Off || !device.VoiceActive(voice))
            {
                continue;
            }

            u32 start = device.VoiceStart(voice);
            if (start >= move.from && start < move.from + move.size)
            {
                device.MoveVoice(voice, delta);
                channel.lastNext = static_cast<u32>(static_cast<s32>(channel.lastNext) + delta);
                moved++;
            }
        }
    }

    if (ModuleLogOn() && !m_memory.Moves().empty())
    {
        std::fprintf(stderr, "audio: compacted the memory: %u block(s) moved, %u voice(s) followed, %u words used\n",
                     static_cast<u32>(m_memory.Moves().size()), moved, m_memory.Used());
    }
}

// ---- the voices ----

void SdlAudio::Spu2::SoundModule::SetGroupVolume(s32 group, u32 left, u32 right)
{
    u32 index = static_cast<u32>(group) >> GroupShift;
    if (index == 0 || index > static_cast<u32>(GroupCount) || left > static_cast<u32>(FullGroupVolume) ||
        right > static_cast<u32>(FullGroupVolume))
    {
        return;
    }

    m_groupLeft[index - 1] = static_cast<s16>(left);
    m_groupRight[index - 1] = static_cast<s16>(right);

    // The processor has no groups: a group's volume is each of its voices' registers scaled. A voice the game plays is scaled
    // while it sounds; a stream's voice is not one of the game's channels (its state stays Off), so it is scaled whenever one
    // of the streams is bound to it — otherwise the music volume option would reach nothing but the voices the game plays
    // itself, and a track would go on at its own level with the option turned down
    for (s32 voice = 0; voice < VoiceCount; voice++)
    {
        if (m_channels[voice].group != group)
        {
            continue;
        }

        if (m_channels[voice].state == ChannelState::Off && !IsStreamVoice(voice))
        {
            continue;
        }

        TargetChannel(voice, m_channels[voice].rawLeft, m_channels[voice].rawRight, false);
    }
}

bool SdlAudio::Spu2::SoundModule::IsStreamVoice(s32 voice) const
{
    for (s32 channel = 0; channel < StreamChannels; channel++)
    {
        // Bound and still the module's: a channel that has been let go of is not holding its voice any more, which is what
        // gives the voice back to the game once its fade is over
        if (m_streams[channel].bound && m_streams[channel].playing && m_streams[channel].voice == voice)
        {
            return true;
        }
    }

    return false;
}

s32 SdlAudio::Spu2::SoundModule::SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return -1;
    }

    // A play the game has taken to silence while it still sounds: this is how a sound stops in the middle of itself, so the log
    // says which voice and where the app was
    if (ModuleLogOn() && left == 0 && right == 0 && m_channels[voice].state != ChannelState::Off &&
        m_channels[voice].levelLeft != 0)
    {
        std::fprintf(stderr, "audio: voice %d cut to silence (was %d/%d, sound %u)\n", voice, m_channels[voice].rawLeft,
                     m_channels[voice].rawRight, m_channels[voice].sound);
    }

    TargetChannel(voice, left, right, false);
    return 0;
}

s32 SdlAudio::Spu2::SoundModule::SetVoicePitch(s32 voice, u16 pitch)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return -1;
    }

    SoundProcessor().SetVoicePitch(voice, pitch);
    return 0;
}

void SdlAudio::Spu2::SoundModule::SetVoiceReverb(s32 voice, bool on)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return;
    }

    SoundProcessor().SetVoiceEffectSend(voice / CoreVoices, voice % CoreVoices, on);
}

void SdlAudio::Spu2::SoundModule::ReleaseVoice(s32 voice, u32 rate)
{
    if (voice < 0 || voice >= VoiceCount || rate >= 0x20)
    {
        return;
    }

    // The module sets the release rate the game asked for and keys the voice off: what it sounds like from there is the
    // envelope's own fade, not a cut
    Device& device = SoundProcessor();
    device.SetVoiceRelease(voice, rate);
    device.KeyOff(voice);
}

s32 SdlAudio::Spu2::SoundModule::IsVoiceFree(s32 voice)
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return -1;
    }

    // A track's voice is not free while the track is the module's: retail marks a music channel taken when its stream starts
    // (MsPlayStream sets the channel's state before it hands the stream over) and only frees it when the stream ends or is
    // stopped. Reported free, the game marked the voice unplaying and then treated it as stealable, which is how a sound came
    // to be played onto a track's voice and a side of the track was lost
    if (IsStreamVoice(voice))
    {
        return 0;
    }

    return m_channels[voice].state == ChannelState::Off ? 1 : 0;
}

SdlAudio::Spu2::ChannelState SdlAudio::Spu2::SoundModule::StateOf(s32 voice) const
{
    if (voice < 0 || voice >= VoiceCount)
    {
        return ChannelState::Off;
    }

    return m_channels[voice].state;
}

// The channel's level from the game's register and its group's volume
void SdlAudio::Spu2::SoundModule::TargetChannel(s32 voice, s16 left, s16 right, bool immediate)
{
    Channel& channel = m_channels[voice];
    channel.rawLeft = left;
    channel.rawRight = right;

    s32 index = static_cast<s32>(channel.group >> GroupShift) - 1;
    s32 groupLeft = index >= 0 && index < GroupCount ? m_groupLeft[index] : FullGroupVolume;
    s32 groupRight = index >= 0 && index < GroupCount ? m_groupRight[index] : FullGroupVolume;

    // The register's level times the group's volume, which is 1.0 at 0x1000
    channel.targetLeft = LevelOfRegister(left) * groupLeft / FullGroupVolume;
    channel.targetRight = LevelOfRegister(right) * groupRight / FullGroupVolume;

    if (immediate)
    {
        channel.levelLeft = channel.targetLeft;
        channel.levelRight = channel.targetRight;
        SoundProcessor().SetVoiceVolume(voice, RegisterOfLevel(channel.levelLeft), RegisterOfLevel(channel.levelRight));
    }
}

void SdlAudio::Spu2::SoundModule::StepRamps()
{
    Device& device = SoundProcessor();
    for (s32 voice = 0; voice < VoiceCount; voice++)
    {
        Channel& channel = m_channels[voice];
        if (channel.levelLeft == channel.targetLeft && channel.levelRight == channel.targetRight)
        {
            continue;
        }

        channel.levelLeft = RampTowards(channel.levelLeft, channel.targetLeft);
        channel.levelRight = RampTowards(channel.levelRight, channel.targetRight);
        device.SetVoiceVolume(voice, RegisterOfLevel(channel.levelLeft), RegisterOfLevel(channel.levelRight));
    }
}

// A voice's key status, as the module would report it: the processor says whether the voice is still sounding, and a looping
// sound's passes are counted here, since the processor loops a block for good and the game asked for a number of them
void SdlAudio::Spu2::SoundModule::TrackChannels()
{
    Device& device = SoundProcessor();
    for (s32 voice = 0; voice < VoiceCount; voice++)
    {
        Channel& channel = m_channels[voice];
        if (channel.state == ChannelState::Off)
        {
            continue;
        }

        if (!device.VoiceActive(voice))
        {
            channel.state = ChannelState::Off;
            channel.sound = NoSound;
            continue;
        }

        u32 next = device.VoiceNext(voice);
        // The reading address only ever goes backwards when the sound has gone round its loop point
        if (next < channel.lastNext)
        {
            if (channel.passesLeft != 0)
            {
                channel.passesLeft--;
                if (channel.passesLeft == 0)
                {
                    device.KeyOff(voice);
                }
            }
        }

        channel.lastNext = next;
    }
}

// ---- the sounds ----

u32 SdlAudio::Spu2::SoundModule::LoadSoundBank(u32 sound, const u8* data, u32 size)
{
    if (sound >= SoundSlots || data == nullptr || size == 0)
    {
        return 0;
    }

    // Retail loads each sound once, but a level loaded again would otherwise leave its block behind
    if (m_sounds[sound].used)
    {
        m_memory.Release(sound);
        m_sounds[sound] = Sound();
    }

    // The bank's own header sits before its samples and is not the voice's to read
    u32 skip = size >= BankHeaderBytes && std::memcmp(data, "MSVp", 4) == 0 ? BankHeaderBytes : 0;
    const u8* samples = data + skip;
    u32 bytes = size - skip;
    if (bytes == 0)
    {
        return 0;
    }

    u32 words = (bytes + 1) / 2;
    u32 address = 0;
    if (!m_memory.Allocate(words, address, sound))
    {
        return 0;
    }

    SoundProcessor().WriteMemory(address, samples, bytes & ~1u);

    m_sounds[sound].used = true;
    m_sounds[sound].address = address;
    m_sounds[sound].words = words;
    m_sounds[sound].blocks = bytes / BlockBytes;
    if (ModuleLogOn())
    {
        // What the data itself says: the first block's shift, filter and flags, and how many blocks claim an end or a loop. The
        // disc's sounds are not all terminated with a flag - the module's own length is what ends them - so this says whether a
        // sound leans on the flag or on its size
        u32 blocks = m_sounds[sound].blocks;
        u32 ends = 0;
        u32 loops = 0;
        for (u32 block = 0; block < blocks; block++)
        {
            u8 flags = static_cast<u8>(static_cast<u16>(SoundProcessor().Memory()[address + block * BlockWords]) >> 8);
            if ((flags & BlockLoopEnd) != 0)
            {
                ends++;
            }

            if ((flags & BlockLoop) != 0)
            {
                loops++;
            }
        }

        u16 first = static_cast<u16>(SoundProcessor().Memory()[address]);
        std::fprintf(stderr,
                     "audio: load sound %u %u bytes -> %u words at %u (header %u, %u blocks, first shift %u filter %u flags "
                     "%02x, %u end %u loop, memory %u/%u free)\n",
                     sound, bytes, words, address, skip, blocks, first & 0xF, (first >> 4) & 0xF, (first >> 8) & 0xFF, ends,
                     loops, m_memory.Free(), m_memory.Ceiling() - MemoryBase);
    }

    return address;
}

void SdlAudio::Spu2::SoundModule::SoundBankLoaded(u16 bank)
{
    // The sound's block was registered as its samples arrived, and its address given to the processor then: nothing is left
    // to do here but be told, which is what retail's module does with the same call
    (void)bank;
}

void SdlAudio::Spu2::SoundModule::ReserveSounds(u16 count)
{
    // Retail reserves a table of sound slots in the I/O processor's memory. The host's table is fixed and covers every id the
    // game's resources can have, so there is nothing to reserve
    (void)count;
}

void SdlAudio::Spu2::SoundModule::ReleaseSound(u32 sound)
{
    if (sound >= SoundSlots)
    {
        return;
    }

    if (m_sounds[sound].used)
    {
        if (ModuleLogOn())
        {
            std::fprintf(stderr, "audio: release sound %u (%u words at %u)\n", sound, m_sounds[sound].words,
                         m_sounds[sound].address);
        }

        // A voice still reading this block would be left reading whatever the memory is handed out for next, which is heard as
        // noise: the game has let the samples go, so the voice is let go with them
        Device& device = SoundProcessor();
        for (s32 voice = 0; voice < VoiceCount; voice++)
        {
            Channel& channel = m_channels[voice];
            if (channel.state == ChannelState::Off || channel.sound != sound)
            {
                continue;
            }

            device.SetVoiceRelease(voice, 0x0D);
            device.KeyOff(voice);
            channel.sound = NoSound;
            if (ModuleLogOn())
            {
                std::fprintf(stderr, "audio: voice %d let go with the sound it was playing\n", voice);
            }
        }

        m_memory.Release(sound);
        m_sounds[sound] = Sound();
    }
    else if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: release sound %u, which was not in the memory\n", sound);
    }
}

void SdlAudio::Spu2::SoundModule::SetSoundCaller(const void* caller)
{
    m_soundCaller = caller;
}

s32 SdlAudio::Spu2::SoundModule::PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack,
                                           u32 release, u32 loops)
{
    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= static_cast<u32>(VoiceCount))
    {
        return -1;
    }

    if (loops > 0xFFFF)
    {
        return -2;
    }

    if (sound >= SoundSlots)
    {
        return 0;
    }

    // The module's key-ons are off while a movie has it, and a sound whose samples are not in the memory cannot play
    if (m_lent || !m_sounds[sound].used)
    {
        if (ModuleLogOn())
        {
            std::fprintf(stderr, "audio: play sound %u voice %u REFUSED (%s)\n", sound, voice,
                         m_lent ? "the module is lent to a movie" : "its samples are not in the memory");
        }

        return 0;
    }

    Device& device = SoundProcessor();
    Channel& channel = m_channels[static_cast<s32>(voice)];
    channel.sound = sound;
    channel.group = static_cast<s32>(voiceAndGroup & 0xFFFF0000);
    // One pass unless the game asked for more: a count of 0 has to mean a pass here too, or a sound whose own data carries a
    // loop flag goes round for good (the processor loops a block for as long as its flag says) and its voice is never let go
    channel.passesLeft = loops <= 1 ? 1 : loops;

    if (ModuleLogOn())
    {
        std::fprintf(stderr,
                     "audio: play sound %u voice %u vol %d/%d pitch %u attack %u release %u loops %u group %d from %p%s\n",
                     sound, voice, left, right, pitch, attack, release, loops,
                     static_cast<s32>((voiceAndGroup & 0xFFFF0000) >> GroupShift), m_soundCaller,
                     device.VoiceActive(static_cast<s32>(voice)) ? " ON A VOICE ALREADY PLAYING" : "");
    }

    u16 attackDecay = 0;
    u16 sustainRelease = 0;
    SoundEnvelope(attack, release, attackDecay, sustainRelease);

    device.SetVoiceEnvelope(static_cast<s32>(voice), attackDecay, sustainRelease);
    device.SetVoicePitch(static_cast<s32>(voice), pitch);
    device.SetVoiceStart(static_cast<s32>(voice), m_sounds[sound].address);
    device.SetVoiceNoise(static_cast<s32>(voice), false);

    // The sound's own length: the disc's sounds do not all end with a flag, so the voice has to be told where its data stops
    // or it reads on into the next sound in the memory
    device.KeyOn(static_cast<s32>(voice));

    // After the key-on, which resets the voice's own idea of where it loops and stops: the window is the module's to give,
    // since it is the module that knows the sound's length.
    //
    // Only an explicit count of passes makes the voice go round its own data. A play of 0 or 1 passes is one pass and stops at
    // the end of the samples: the game's repeatable sounds are not the module's to repeat. Its own sound objects re-key them -
    // a repeatable instance sound waits for the voice to stop and then plays it again (soundobjects.cpp's StateWaiting) - so a
    // module that looped them instead would never let the voice go, and the game would keep playing the sound on top of a copy
    // of itself that nothing could stop
    u32 finish = m_sounds[sound].address + m_sounds[sound].blocks * BlockWords;
    device.SetVoiceDataEnd(static_cast<s32>(voice), finish);
    device.SetVoiceDataLoop(static_cast<s32>(voice), loops > 1);

    // The play's own volume, straight away: a key-on that faded in from the last sound's level would be heard as the sound
    // starting late
    TargetChannel(static_cast<s32>(voice), left, right, true);

    channel.state = ChannelState::KeyedOn;
    channel.lastNext = m_sounds[sound].address | 1;
    return 0;
}

s32 SdlAudio::Spu2::SoundModule::PitchOfRate(s32 rate)
{
    // The pitch that plays a sample of this rate at the processor's own: 0x1000 is 48 kHz
    return (rate << 12) / 48000;
}

u32 SdlAudio::Spu2::SoundModule::ReserveSoundMemory(u32 bytes)
{
    // The music players' buffers, reserved once at start-up: the words they take are theirs for good. The game's addresses are
    // the retail module's, in bytes, and it does its own arithmetic on them (a player's second half is its first plus 0x4000)
    u32 words = (bytes + 1) / 2;
    u32 address = 0;
    if (!m_memory.Allocate(words, address))
    {
        return 0;
    }

    return address * 2;
}

u32 SdlAudio::Spu2::SoundModule::ChannelBufferAddress(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return 0;
    }

    // The ring the channel plays out of, in the game's bytes, which is what it lends the movie: a player whose buffer reads 0 is
    // one the game refuses to lend
    const MusicStream& stream = m_streams[channel];
    if (stream.ringAddress != 0)
    {
        return stream.ringAddress * 2;
    }

    return 0;
}

// ---- the music's streams ----

// The blocks of the ring the writer keeps free: enough that the voice cannot run into a block not yet written
constexpr u32 StreamBlocksHeldBack = 128;

void SdlAudio::Spu2::SoundModule::SetChannelBuffer(s32 channel, u32 address, u32 words)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    MusicStream& stream = m_streams[channel];
    stream.ringAddress = (address & ~7u) / 2;
    stream.ringWords = words / 2;

    // The first block of the ring is where a loop of the ring comes back to
    if (address != 0 && words != 0)
    {
        stream.writeBlock = 0;
    }
}

void SdlAudio::Spu2::SoundModule::ReadMusic(s32 file, u32 offset, u32 size)
{
    // Retail's module keeps one pending read: the next stream started is the one that takes it
    m_pendingFile = file;
    m_pendingOffset = offset;
    m_pendingSize = size;
}

void SdlAudio::Spu2::SoundModule::PrepareStream(s32 file, std::vector<u8>* data)
{
    data->clear();
    if (file < 0 || m_pendingSize == 0)
    {
        return;
    }

    data->resize(m_pendingSize);
    Platform::Stream::Read(0, file, m_pendingOffset, m_pendingSize, data->data());
}

s32 SdlAudio::Spu2::SoundModule::StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once,
                                             std::vector<u8>* prepared)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return -1;
    }

    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= static_cast<u32>(VoiceCount))
    {
        return -1;
    }

    MusicStream& stream = m_streams[channel];

    // A channel given a new track lets the one it had go, and its other side with it. A re-bound channel that left the old
    // voices looping their rings would go on sounding under the new track, and once the binding is replaced nothing in the game
    // knows those voices are there to stop - the music would pile up, a copy per track change, and never stop
    LetStreamVoiceGo(channel);
    if (stream.childChannel >= 0)
    {
        LetStreamVoiceGo(stream.childChannel);
    }

    // The region and the ring the player was given stay with the channel; anything else about it is the new track's
    u32 ringAddress = stream.ringAddress;
    u32 ringWords = stream.ringWords;
    if (ringWords == 0)
    {
        // No ring was handed over for this channel: reserve half a player's buffer now (the reservation is in bytes, the
        // device's addresses in words)
        ringWords = StreamRingWords;
        ringAddress = ReserveSoundMemory(StreamRingBytes) / 2;
    }

    stream = MusicStream();
    stream.ringAddress = ringAddress;
    stream.ringWords = ringWords;
    stream.bound = ringAddress != 0;
    stream.voice = static_cast<s32>(voice);
    stream.group = voiceAndGroup & 0xFFFF0000;
    stream.pitch = pitch;
    stream.once = once;
    stream.looping = !once;
    stream.file = file;
    stream.offset = m_pendingOffset;
    stream.size = m_pendingSize;
    stream.ready = false;
    stream.playing = true;

    // The region's ADPCM is read once, here: the ring below is filled from the host's memory while the track plays, so the
    // audio thread never touches the disc. A track is a few hundred thousand bytes; the whole region being here is what makes
    // the ring's refills a copy rather than a read.
    if (stream.size != 0 && file >= 0)
    {
        if (prepared != nullptr)
        {
            // Read by the caller before it took the lock (Platform::Audio::StreamMusic), so the mixer was never parked
            // for the disc read's length
            stream.data.swap(*prepared);
        }
        else
        {
            stream.data.resize(stream.size);
            Platform::Stream::Read(0, file, stream.offset, stream.size, stream.data.data());
        }
    }

    // The ring is the voice's: it loops back to its own first block, which is where the writer keeps laying the track
    Device& device = SoundProcessor();

    // Retail records the music's channel as taken by the stream and scales its volumes by its group's volume
    // (MsPlayStream: MsSetChannelGroup, MsStoreChannelVolumes, then the group-scaled left/right go to the stream). That is
    // how the game's own music volume option reaches a track. The voice here is not a game channel, so only the group is
    // recorded: the levels it is given then come to it through the group like any other voice's
    m_channels[static_cast<s32>(voice)].group = static_cast<s32>(stream.group);
    device.SetVoiceEnvelope(static_cast<s32>(voice), 0x000F, StreamEnvelope);
    device.SetVoicePitch(static_cast<s32>(voice), pitch);
    device.SetVoiceStart(static_cast<s32>(voice), ringAddress);
    device.SetVoiceLoopStart(static_cast<s32>(voice), ringAddress);
    // A stream's end is its ring's flags, not a length: whatever the voice was told before is not this track's
    device.SetVoiceDataEnd(static_cast<s32>(voice), 0);
    device.SetVoiceNoise(static_cast<s32>(voice), false);

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: stream channel %d file %d offset %u size %u rate pitch %u voice %u loop %d ring %u\n",
                     channel, file, stream.offset, stream.size, pitch, voice, stream.looping ? 1 : 0, ringAddress);
    }

    return channel;
}

void SdlAudio::Spu2::SoundModule::InterleaveMusic(s32 channel, u32 blockSize)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    // The track's data alternates in blocks between the two sides of the pair
    m_streams[channel].blockSize = blockSize;
    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: stream channel %d interleaved in %u byte chunks\n", channel, blockSize);
    }
}

void SdlAudio::Spu2::SoundModule::AddMusicChannel(s32 childChannel, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    if (childChannel < 0 || childChannel >= StreamChannels || parent < 0 || parent >= StreamChannels)
    {
        return;
    }

    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= static_cast<u32>(VoiceCount))
    {
        return;
    }

    MusicStream& parentStream = m_streams[parent];
    MusicStream& stream = m_streams[childChannel];
    stream = MusicStream();
    stream.bound = true;
    stream.child = true;
    stream.parent = parent;
    stream.voice = static_cast<s32>(voice);
    stream.group = voiceAndGroup & 0xFFFF0000;
    stream.pitch = parentStream.pitch;
    stream.blockSize = parentStream.blockSize;
    stream.once = parentStream.once;
    stream.looping = parentStream.looping;
    stream.file = parentStream.file;
    stream.offset = parentStream.offset;
    stream.size = parentStream.size;
    stream.ready = parentStream.ready;
    stream.playing = parentStream.playing;
    // The child's ring is the half of the player's buffer the game gave it, and its data is the same region's other chunks
    stream.ringAddress = (soundAddress & ~7u) / 2;
    stream.ringWords = parentStream.ringWords;
    stream.nextChunk = 1;
    parentStream.childChannel = childChannel;

    Device& device = SoundProcessor();

    // The other side is a channel of the same group, recorded the same way: retail's MsAddStreamChannel does its group's
    // scaling for the child as well as the parent
    m_channels[static_cast<s32>(voice)].group = static_cast<s32>(stream.group);
    device.SetVoiceEnvelope(static_cast<s32>(voice), 0x000F, StreamEnvelope);
    device.SetVoicePitch(static_cast<s32>(voice), stream.pitch);
    device.SetVoiceStart(static_cast<s32>(voice), stream.ringAddress);
    device.SetVoiceLoopStart(static_cast<s32>(voice), stream.ringAddress);
    device.SetVoiceDataEnd(static_cast<s32>(voice), 0);
    device.SetVoiceNoise(static_cast<s32>(voice), false);

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: stream channel %d is the second side of channel %d on voice %u at ring %u\n", childChannel,
                     parent, voice, stream.ringAddress);
    }
}

void SdlAudio::Spu2::SoundModule::SetMusicEnd(s32 channel, u32 end)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    // Where the track's data ends in its last block: the region's own size is what the module streams, so this only records
    // what the game said
    m_streams[channel].endOffset = end;
}

void SdlAudio::Spu2::SoundModule::PrepareMusic(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    MusicStream& stream = m_streams[channel];
    // The ring is primed before the voice is let loose on it: the first blocks are laid down here, so a key-on finds data
    // under it rather than whatever the memory held
    PrimeStream(channel);

    stream.ready = true;
}

// A track's ring, and the ring of its other side. Both are primed before either voice is keyed on: a side left unprimed
// played whatever the memory held - heard as a burst of the wrong sound, and then the voice read on until it met a block
// whose flags stopped it, so that side of the track was gone for good
void SdlAudio::Spu2::SoundModule::PrimeStream(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    MusicStream& stream = m_streams[channel];
    FillStream(stream);
    if (stream.child)
    {
        // The child has no region of its own: its blocks come from the parent's, so the parent is primed first
        FillStream(stream);
    }

    if (stream.childChannel >= 0)
    {
        MusicStream& other = m_streams[stream.childChannel];
        if (other.bound)
        {
            FillStream(other);
        }
    }
}

bool SdlAudio::Spu2::SoundModule::IsMusicReady(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return false;
    }

    return m_streams[channel].bound && m_streams[channel].ready;
}

s32 SdlAudio::Spu2::SoundModule::PlayMusic(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return -1;
    }

    MusicStream& stream = m_streams[channel];
    if (!stream.bound || stream.voice < 0)
    {
        return -1;
    }

    Device& device = SoundProcessor();
    s32 voice = stream.voice;

    // Whatever the game did before this, the voice is not let loose on unwritten memory: the rings are primed here as well as
    // at the prepare, since a play is the last moment it can be done and a ring with nothing under it is heard as the memory
    // Retail reads the region as it plays, on the I/O processor, so a music bank that was not open when the channel was set up
    // costs it nothing. This port takes the whole region at the set-up, so that read is the one that has to happen again here:
    // a channel whose read found no file has nothing to prime, and refusing the track leaves the game believing a track it
    // started is playing when it is not
    if (stream.data.empty() && stream.size != 0 && stream.file >= 0)
    {
        stream.data.resize(stream.size);
        Platform::Stream::Read(0, stream.file, stream.offset, stream.size, stream.data.data());
        if (ModuleLogOn())
        {
            std::fprintf(stderr, "audio: stream channel %d read its region again at the play (%zu bytes)\n", channel,
                         stream.data.size());
        }
    }

    PrimeStream(channel);

    // A region that could not be read (a size of 0, a read past the end of the file) primes nothing, and a voice keyed on
    // anyway would walk the memory through blocks with no end in them: it never stops, so the game never hears the track end
    // and never starts another one
    if (stream.writeBlock == 0)
    {
        // The module does not hold the track, so the game's player sees it over and stops it: a track that never starts and
        // never ends would keep the player in Playing for the rest of the session
        stream.playing = false;
        stream.started = false;

        if (ModuleLogOn())
        {
            std::fprintf(stderr,
                         "audio: stream channel %d has no data to play (nothing was read: %zu bytes in memory of %u, ring %u "
                         "words, file %d)\n",
                         channel, stream.data.size(), stream.size, stream.ringWords, stream.file);
        }

        return -1;
    }

    // The voice plays the ring from its first block, at the volume and group the game gave it
    device.SetVoiceStart(voice, stream.ringAddress);
    device.SetVoiceLoopStart(voice, stream.ringAddress);
    device.SetVoiceDataEnd(voice, 0);
    device.SetVoiceEnvelope(voice, 0x000F, StreamEnvelope);
    device.SetVoicePitch(voice, stream.pitch);
    device.KeyOn(voice);
    stream.playing = true;
    stream.started = true;

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: stream channel %d plays on voice %d with volume %d/%d group %u\n", channel, voice,
                     m_channels[voice].rawLeft, m_channels[voice].rawRight, stream.group >> GroupShift);
    }

    // The interleaved pair starts together, the way the module starts a stream's child with its parent
    if (stream.childChannel >= 0)
    {
        MusicStream& other = m_streams[stream.childChannel];
        if (other.bound && other.voice >= 0)
        {
            device.SetVoiceStart(other.voice, other.ringAddress);
            device.SetVoiceLoopStart(other.voice, other.ringAddress);
            device.SetVoiceDataEnd(other.voice, 0);
            device.SetVoiceEnvelope(other.voice, 0x000F, StreamEnvelope);
            device.SetVoicePitch(other.voice, other.pitch);
            device.KeyOn(other.voice);
            other.playing = true;
            other.started = true;
        }
    }

    return 0;
}

bool SdlAudio::Spu2::SoundModule::IsMusicPlaying(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return false;
    }

    // The module lists the stream until it ends or is stopped: a pause or a movie taking the module over does not stop it
    // being the game's track, and the game would not start another one if it thought this had ended
    return m_streams[channel].playing;
}

// A stream's voice lets go of what it is playing: keyed off with a release, and no longer the module's to refill. Called by the
// stop, and by a channel being given another track - the voices of the track it had would otherwise keep looping their rings
// with nothing in the game left knowing they are there
void SdlAudio::Spu2::SoundModule::LetStreamVoiceGo(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    MusicStream& stream = m_streams[channel];
    if (stream.voice >= 0 && stream.bound)
    {
        SoundProcessor().SetVoiceRelease(stream.voice, 0x0D);
        SoundProcessor().KeyOff(stream.voice);
    }

    stream.playing = false;
    stream.ready = false;
    stream.started = false;
    // The voice is the game's again from here: a stream that kept the number would key off, or hold, whatever the game put
    // there next
    stream.voice = -1;
}

void SdlAudio::Spu2::SoundModule::StopMusic(s32 channel)
{
    if (channel < 0 || channel >= StreamChannels)
    {
        return;
    }

    MusicStream& stream = m_streams[channel];
    Device& device = SoundProcessor();
    LetStreamVoiceGo(channel);
    if (stream.childChannel >= 0)
    {
        LetStreamVoiceGo(stream.childChannel);
    }

    if (ModuleLogOn())
    {
        s32 held = 0;
        for (s32 index = 0; index < VoiceCount; index++)
        {
            if (device.VoiceActive(index))
            {
                held++;
            }
        }

        std::fprintf(stderr, "audio: stream channel %d stopped (%d of %d voices still sounding)\n", channel, held, VoiceCount);
    }
}

void SdlAudio::Spu2::SoundModule::EndStream(s32 channel)
{
    MusicStream& stream = m_streams[channel];
    stream.playing = false;
    // The track's voices are the game's again: the stop that follows must not find the number and key off, or hold, a voice
    // the game has been given back and used for something else since
    stream.started = false;
    stream.voice = -1;
    if (stream.childChannel >= 0)
    {
        MusicStream& other = m_streams[stream.childChannel];
        other.playing = false;
        other.started = false;
        other.voice = -1;
    }

    if (ModuleLogOn())
    {
        std::fprintf(stderr, "audio: stream channel %d ended\n", channel);
    }
}

const std::vector<u8>& SdlAudio::Spu2::SoundModule::StreamData(const MusicStream& stream) const
{
    static const std::vector<u8> NoData;
    if (!stream.data.empty())
    {
        return stream.data;
    }

    if (stream.parent >= 0 && stream.parent < StreamChannels)
    {
        return m_streams[stream.parent].data;
    }

    return NoData;
}

// One block of the region into the ring, its flags made the module's own: the region's data may carry loop and end flags of
// its own (a track's own loop point), and the ring's flags are what the voice must follow
void SdlAudio::Spu2::SoundModule::WriteStreamBlock(MusicStream& stream)
{
    u32 blocks = stream.ringWords / BlockWords;
    const std::vector<u8>& data = StreamData(stream);
    if (blocks == 0 || data.empty())
    {
        stream.finished = true;
        return;
    }

    // The track's data alternates in blocks between the two sides of an interleaved pair, so a side's blocks are every other
    // run of blockSize bytes
    bool interleaved = stream.blockSize >= BlockBytes;
    u32 run = interleaved ? stream.blockSize : BlockBytes;
    u32 runBlocks = interleaved ? stream.blockSize / BlockBytes : 1;

    // Where this block comes from. A track that loops starts its region over once it is spent: the ring goes on being
    // written, so its voice never reaches memory nobody has written (a ring that stops being filled is a side of the track
    // that plays the memory that was there before it, and then stops on whatever flags it finds)
    u32 at = stream.nextChunk * run + stream.chunkBlock * BlockBytes;
    if (at + BlockBytes > stream.size)
    {
        if (!stream.looping)
        {
            stream.finished = true;
            return;
        }

        // The region's beginning, for this side: the child's blocks are every other run
        stream.nextChunk = stream.child ? 1 : 0;
        stream.chunkBlock = 0;
        at = stream.nextChunk * run;
        if (at + BlockBytes > stream.size)
        {
            stream.finished = true;
            return;
        }
    }

    // Where the one after it will be: the last block of the region is the one nothing follows
    u32 afterChunk = stream.nextChunk;
    u32 afterBlock = stream.chunkBlock + 1;
    if (afterBlock >= runBlocks)
    {
        afterBlock = 0;
        afterChunk += interleaved ? 2 : 1;
    }

    u32 after = afterChunk * run + afterBlock * BlockBytes;
    stream.nextChunk = afterChunk;
    stream.chunkBlock = afterBlock;

    u8 block[BlockBytes];
    std::memcpy(block, data.data() + at, BlockBytes);
    // The region's own flags are not the ring's to follow
    block[1] = 0;

    // The flags: the ring's last block comes back to its first, and a track that plays once ends at the region's last block
    bool lastOfRegion = after + BlockBytes > stream.size;
    if (lastOfRegion && !stream.looping)
    {
        block[1] = BlockLoopEnd;
        stream.finished = true;
    }
    else if (stream.writeBlock + 1 >= blocks)
    {
        block[1] = BlockLoopEnd | BlockLoop;
    }

    SoundProcessor().WriteMemory(stream.ringAddress + stream.writeBlock * BlockWords, block, BlockBytes);
    stream.writeBlock = (stream.writeBlock + 1) % blocks;
}

// The ring topped up behind the voice's reading, keeping StreamBlocksHeldBack blocks clear
void SdlAudio::Spu2::SoundModule::FillStream(MusicStream& stream)
{
    u32 blocks = stream.ringWords / BlockWords;
    if (blocks == 0 || stream.ringAddress == 0 || StreamData(stream).empty())
    {
        return;
    }

    Device& device = SoundProcessor();
    u32 readBlock = 0;
    // The voice's reading position, once the voice is this stream's. Before the play it may still be finishing the track the
    // channel had before on the same ring, and its position there says nothing about how full this ring is: taken as this
    // track's, it put the writer 1023 blocks of 1024 "behind" it, so the prime wrote nothing at all and the play had no data
    if (stream.started && stream.voice >= 0 && device.VoiceActive(stream.voice))
    {
        u32 reading = device.VoiceNext(stream.voice) & BlockAlign;
        if (reading >= stream.ringAddress)
        {
            readBlock = ((reading - stream.ringAddress) / BlockWords) % blocks;
        }
    }

    u32 pending = (stream.writeBlock + blocks - readBlock) % blocks;
    while (pending < blocks - StreamBlocksHeldBack)
    {
        u32 before = stream.writeBlock;
        WriteStreamBlock(stream);
        if (stream.writeBlock == before)
        {
            // Nothing could be written: the region is over and the track does not loop
            break;
        }

        pending++;
    }
}

void SdlAudio::Spu2::SoundModule::FillStreams()
{
    for (s32 channel = 0; channel < StreamChannels; channel++)
    {
        MusicStream& stream = m_streams[channel];
        if (!stream.bound || !stream.playing || stream.voice < 0)
        {
            continue;
        }

        FillStream(stream);
    }
}

// A stream whose data has run out is over once its voice has stopped, which is when the game's own player asks whether it is
// still playing
void SdlAudio::Spu2::SoundModule::TrackStreams()
{
    Device& device = SoundProcessor();
    for (s32 channel = 0; channel < StreamChannels; channel++)
    {
        MusicStream& stream = m_streams[channel];
        if (!stream.bound || !stream.playing || stream.voice < 0)
        {
            continue;
        }

        // A stream that has not been played yet has no voice of its own to lose: its ring is primed and waiting
        if (!stream.started)
        {
            continue;
        }

        if (!device.VoiceActive(stream.voice))
        {
            // A looped track's voice has no business stopping on its own: its ring always has a block that comes back
            // round. If one does, the ring is not the shape the filler thinks it is, and the side goes silent for good.
            // Said once, since it would otherwise be said once a mix for the rest of the session
            if (!stream.finished && stream.looping && !stream.stoppedLogged)
            {
                stream.stoppedLogged = true;
                if (ModuleLogOn())
                {
                    std::fprintf(stderr,
                                 "audio: stream channel %d voice %d STOPPED ON ITS OWN at %u (ring %u, wrote to %u)\n", channel,
                                 stream.voice, device.VoiceNext(stream.voice), stream.ringAddress, stream.writeBlock);
                }
            }

            if (stream.finished && !stream.looping)
            {
                EndStream(channel);
            }
        }
    }
}

void SdlAudio::Spu2::SoundModule::SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_effects[core].mode = mode;
    m_effects[core].depthLeft = depthLeft;
    m_effects[core].depthRight = depthRight;
    m_effects[core].delay = delay;
    m_effects[core].feedback = feedback;
    UpdateEffectAreas();
}

void SdlAudio::Spu2::SoundModule::ClearReverb(s32 core)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_effects[core] = CoreEffect();
    UpdateEffectAreas();
}

void SdlAudio::Spu2::SoundModule::SetReverbVolume(s32 core, s16 left, s16 right)
{
    if (core < 0 || core >= CoreCount)
    {
        return;
    }

    m_effects[core].volumeLeft = left;
    m_effects[core].volumeRight = right;
    SoundProcessor().SetEffectVolume(core, static_cast<u16>(left), static_cast<u16>(right));
}

// Both cores' work areas sit at the top of the memory, each as large as its mode needs, and the sound banks keep below them
void SdlAudio::Spu2::SoundModule::UpdateEffectAreas()
{
    u32 wanted[CoreCount] = {};
    u32 total = 0;
    for (s32 core = 0; core < CoreCount; core++)
    {
        s32 mode = m_effects[core].mode;
        if (mode > 0 && mode < EffectModes)
        {
            wanted[core] = (EffectWorkBytes[mode] / 2 + 7) & ~7u;
            total += wanted[core];
        }
    }

    // The areas take the top of the memory, and the banks' ceiling comes down to meet them
    u32 ceiling = MemoryWords - total;
    if (!m_memory.SetCeiling(ceiling))
    {
        // The sounds in the memory reach too high for this mode's area: the effect keeps the room it had
        return;
    }

    u32 top = MemoryWords;
    for (s32 core = 0; core < CoreCount; core++)
    {
        Device& device = SoundProcessor();
        if (wanted[core] == 0)
        {
            device.SetEffectEnabled(core, false);
            continue;
        }

        u32 start = top - wanted[core];
        device.SetEffectArea(core, start, top - 1);
        device.SetEffects(core, ProgramEffect(EffectPresets[m_effects[core].mode], m_effects[core], start, wanted[core]));
        device.SetEffectEnabled(core, true);
        top = start;
    }
}
