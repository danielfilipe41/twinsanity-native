// The sound processor's mixer: 48 SPU2 voices (2 cores of 24) playing
// PlayStation ADPCM out of its memory, or a music stream decoded from its
// region, through SDL3's 48 kHz stereo stream.
//
// The voice is the SPU2's: a pitch counter with 12 fraction bits, four sample
// history fed through the hardware's Gaussian table, an ADSR envelope with the
// hardware's rate table, a per-voice volume in the other-phase encoding, and a
// reverb send. Music runs through the same voices as the sound effects, so the
// game's SetVoiceVolume, SetVoicePitch, SetVoiceReverb and ReleaseVoice calls
// reach it exactly as they do on the PS2.
#include "spu_mixer.h"

#include "adpcm.h"
#include "../settings.h"

#include "platform/stream.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
namespace
{
// The sound processor's own: 2 cores of 24 voices, a pitch of 0x1000 for a
// sample's own rate, 14 bit volumes (0x3FFF full, 0x4000 and up the other
// phase), its 10 reverb modes, the groups' volumes of 1 in 4.12 fixed point
constexpr s32 GroupVolumeShift = 12;
constexpr s32 ReverbModes = 10;
// The sound IDs the game plays are its resources' header IDs: their low 15 bits are the resource's place in its table and bit 15
// marks a voice (the language's) resource, so the table covers the whole 16 bits (the front end's menu sounds and every spoken
// line have bit 15 set, and a table of fewer IDs silently refuses them)
constexpr u32 MaxSounds = 0x10000;
// The reverb network's rate (the SPU family's, half the mixer's 48 kHz) and
// the fixed-point shifts its volumes (1.15) and the movie's volume (the
// StartSound 0 to 1 float in 12 fraction bits) use
constexpr s32 ReverbRate = 22050;
constexpr s32 MixRate = 48000;
constexpr s32 ReverbVolumeShift = 15;
constexpr s32 MovieVolumeShift = 12;
constexpr s32 FullReverbSetting = 127;
constexpr s16 NegativeFullVolume = -32768;
// A volume register's signed value, with 0x8000 read as full (the presets'
// input drive: PCSX2's MUL reads it as +32768 too)
s32 ReverbVolume(s16 volume)
{
    return volume == NegativeFullVolume ? 32768 : volume;
}

// A mode's work area, in bytes (retail g_MsEffectSizesByMode,
// asm/data/data.data.s D_002E7D38: the SPU2's, matching the published PSX
// preset sizes all the way: Room 26C0, Small 1F40, Medium 4840, Large 6FE0,
// Hall ADE0, Space F6C0, Echo/Delay 18040, Pipe 3C00)
constexpr u32 ReverbWorkBytes[ReverbModes] = {0x80, 0x26C0, 0x1F40, 0x4840, 0x6FE0,
                                              0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};
// A mode's register set (32 words: APF sizes, volumes, wall, APF volumes,
// same/diff/comb/APF addresses, input drive). Sony's SPU2 effect table from
// the disc's LIBSD.IRX at 0x4b68: Room's 64 bytes, then 8 records of 4
// reserved bytes and 64 register bytes (Studio Small through Half Echo).
// Modes line up with ReverbMode (Room 1 through Pipe 9); mode 0 is the
// published "reverb off" dummy, never ticked.
constexpr u16 ReverbPresetRegs[ReverbModes][32] = {
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

// The SPU2's Gaussian interpolation table (the hardware's, 0x200 entries of
// 1.15; PCSX2 carries the same table)
constexpr s16 InterpolationTable[0x200] = {
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    -0x001, -0x001, -0x001, -0x001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003, 0x0003, 0x0004, 0x0004, 0x0005,
    0x0005, 0x0006, 0x0007, 0x0007, 0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018, 0x0019, 0x001B, 0x001C, 0x001E,
    0x0020, 0x0021, 0x0023, 0x0025, 0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
    0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050, 0x0054, 0x0057, 0x005B, 0x005F,
    0x0063, 0x0067, 0x006B, 0x006F, 0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
    0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7, 0x00CD, 0x00D4, 0x00DB, 0x00E3,
    0x00EA, 0x00F2, 0x00FA, 0x0101, 0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
    0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C, 0x01A8, 0x01B4, 0x01C0, 0x01CC,
    0x01D9, 0x01E5, 0x01F2, 0x0200, 0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
    0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9, 0x030B, 0x031D, 0x0330, 0x0343,
    0x0356, 0x036A, 0x037E, 0x0392, 0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
    0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506, 0x0520, 0x053B, 0x0556, 0x0572,
    0x058E, 0x05AA, 0x05C7, 0x05E4, 0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
    0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF, 0x0813, 0x0838, 0x085D, 0x0883,
    0x08A9, 0x08D0, 0x08F7, 0x091E, 0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
    0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8, 0x0C07, 0x0C38, 0x0C68, 0x0C99,
    0x0CCB, 0x0CFD, 0x0D30, 0x0D63, 0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
    0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB, 0x1116, 0x1153, 0x118F, 0x11CD,
    0x120B, 0x1249, 0x1288, 0x12C7, 0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
    0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700, 0x1747, 0x1790, 0x17D8, 0x1821,
    0x186B, 0x18B5, 0x1900, 0x194B, 0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
    0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37, 0x1E89, 0x1EDC, 0x1F2F, 0x1F82,
    0x1FD6, 0x202A, 0x207F, 0x20D4, 0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
    0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653, 0x26AD, 0x2708, 0x2763, 0x27BE,
    0x281A, 0x2876, 0x28D2, 0x292E, 0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
    0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D, 0x2F6C, 0x2FCC, 0x302B, 0x308B,
    0x30EA, 0x314A, 0x31AA, 0x3209, 0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
    0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807, 0x3867, 0x38C6, 0x3926, 0x3985,
    0x39E4, 0x3A43, 0x3AA2, 0x3B00, 0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
    0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0, 0x412A, 0x4185, 0x41DF, 0x4239,
    0x4292, 0x42EB, 0x4344, 0x439C, 0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
    0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9, 0x493A, 0x498A, 0x49D9, 0x4A29,
    0x4A77, 0x4AC5, 0x4B13, 0x4B5F, 0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
    0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7, 0x5019, 0x505A, 0x509A, 0x50DA,
    0x5118, 0x5156, 0x5194, 0x51D0, 0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
    0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529, 0x5558, 0x5585, 0x55B2, 0x55DE,
    0x5609, 0x5632, 0x565B, 0x5684, 0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
    0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886, 0x589E, 0x58B5, 0x58CB, 0x58E0,
    0x58F4, 0x5907, 0x5919, 0x592A, 0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
    0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3};

SdlAudio::Voice g_Voices[Platform::Audio::Voices];
s32 g_GroupVolumesLeft[4] = {Platform::Audio::FullGroupVolume, Platform::Audio::FullGroupVolume,
                             Platform::Audio::FullGroupVolume, Platform::Audio::FullGroupVolume};
s32 g_GroupVolumesRight[4] = {Platform::Audio::FullGroupVolume, Platform::Audio::FullGroupVolume,
                              Platform::Audio::FullGroupVolume, Platform::Audio::FullGroupVolume};
SdlAudio::Reverb g_Reverbs[Platform::Audio::Cores];
// A core's running reverb: the cooked network (volumes in 1.15, line offsets
// in work-area cells) over its work area, fed averaged 48 kHz input ticks at
// 22050 Hz, its wet output held between ticks. g_Reverbs keeps the game's
// settings; this keeps what ticks from them, so spu_mixer.h stays as it was.
struct ReverbState
{
    bool on = false;
    u32 cells = 0;
    std::vector<s16> buffer;
    u32 base = 0;
    s32 phase = 0;
    s32 inputLeft = 0;
    s32 inputRight = 0;
    u32 inputCount = 0;
    s32 previousLeft = 0;
    s32 previousRight = 0;
    s32 currentLeft = 0;
    s32 currentRight = 0;
    s32 inputCoefLeft = 0;
    s32 inputCoefRight = 0;
    s32 reflection = 0;
    s32 wall = 0;
    s32 comb[4] = {};
    s32 apf[2] = {};
    u32 sameSource[2] = {};
    u32 sameTarget[2] = {};
    u32 diffSource[2] = {};
    u32 diffTarget[2] = {};
    u32 combSource[4][2] = {};
    u32 apfTarget[2][2] = {};
    u32 apfSize[2] = {};
};

ReverbState g_ReverbStates[Platform::Audio::Cores];

s32 ClampReverb(s32 sample)
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

// The mix's output stage. The console's sound processor feeds a DAC, and what
// leaves the machine is coupled through capacitors: a level that sits away
// from zero is gone before it reaches the speakers, and the samples keep
// their whole range for the sound itself. The host's stream is digital end to
// end and has no such stage, so the two things it does are done here.
//
// The blocker matters most: the banks' own samples sit off zero (a sound of
// theirs read back whole has a level of a good part of full scale), and a
// play of it then arrives with that much of the range spent before a single
// sample is heard. Two such sounds together have nowhere to go but the top of
// the range, and what is left of the mix is squared off there, which is heard
// as the crackle of a busy moment.
//
// The knee is what a peak that still reaches the top does instead of being
// squared off: the last 0.85 of the range is untouched, and past it the drive
// bends towards the ceiling rather than stopping dead. The bend's own
// distortion at a couple of dB over is a fraction of a clipped peak's, and
// nothing below the knee is changed at all.
constexpr s32 OutputDcPole = 65365;                     // 20 Hz, one pole, 0.16 fixed point at 48 kHz
constexpr s32 OutputKnee = 27852;                       // 0.85 of full scale
constexpr s32 OutputKneeRange = 32767 - OutputKnee;

struct OutputStage
{
    // The blocker's last input and output a side
    s32 inLeft = 0;
    s32 inRight = 0;
    s32 outLeft = 0;
    s32 outRight = 0;
};

OutputStage g_OutputStage;

// How many frames of the mix the knee had to bend, for the sound log: the
// count against the frames mixed is how hard the moment is being pushed
u64 g_BentFrames = 0;

void ResetOutputStage()
{
    g_OutputStage = OutputStage();
    g_BentFrames = 0;
}

// One channel through the stage: the DC blocker (y = x - x' + pole * y'), then
// the knee. The blocker keeps the unbent signal as its own state, so the knee
// never feeds back into it.
s32 OutputChannel(s32 sample, s32& previousIn, s32& previousOut, bool& bent)
{
    s32 blocked = sample - previousIn + static_cast<s32>((static_cast<s64>(OutputDcPole) * previousOut) >> 16);
    previousIn = sample;
    // A tail under a sample's own step is nothing: it settles to zero rather
    // than ringing down for good, so a stopped mix is digital silence again
    if (blocked > -2 && blocked < 2)
    {
        blocked = 0;
    }

    previousOut = blocked;

    s32 magnitude = blocked < 0 ? -blocked : blocked;
    if (magnitude <= OutputKnee)
    {
        return blocked;
    }

    bent = true;
    s32 over = magnitude - OutputKnee;
    s32 bentMagnitude = OutputKnee + static_cast<s32>(static_cast<s64>(OutputKneeRange) * over / (OutputKneeRange + over));
    return blocked < 0 ? -bentMagnitude : bentMagnitude;
}

// A register line offset in work-area cells (the register holds an SPU
// address divided by 8, so bytes are 8 times it and cells half that),
// shortened by the delay knob (full at 127, half at 0, never degenerate)
u32 ReverbCells(u16 offset, u16 delay, u32 cells)
{
    u32 scaled = static_cast<u32>(offset) * 4 * (static_cast<u32>(delay) + FullReverbSetting) / (2 * FullReverbSetting);
    return cells == 0 ? 0 : scaled % cells;
}

// (Re)cooks a core's network from the game's settings: preset registers,
// depth driving the input, feedback the wall, delay the line lengths. The
// buffer is zeroed like the hardware's must be before enabling.
void CookReverb(s32 core)
{
    ReverbState& state = g_ReverbStates[core];
    const SdlAudio::Reverb& reverb = g_Reverbs[core];
    s32 mode = reverb.mode;
    if (mode <= 0 || mode >= ReverbModes)
    {
        state = ReverbState();
        return;
    }

    const u16* regs = ReverbPresetRegs[mode];
    state.on = true;
    state.cells = ReverbWorkBytes[mode] / 2;
    state.buffer.assign(state.cells, 0);
    state.base = 0;
    state.phase = 0;
    state.inputLeft = 0;
    state.inputRight = 0;
    state.inputCount = 0;
    state.previousLeft = 0;
    state.previousRight = 0;
    state.currentLeft = 0;
    state.currentRight = 0;
    state.inputCoefLeft = 32768 * static_cast<s32>(reverb.depthLeft) / 32767;
    state.inputCoefRight = 32768 * static_cast<s32>(reverb.depthRight) / 32767;
    state.reflection = ReverbVolume(static_cast<s16>(regs[2]));
    state.wall = static_cast<s32>(static_cast<s16>(regs[7])) * reverb.feedback / FullReverbSetting;
    for (u32 tap = 0; tap < 4; tap++)
    {
        state.comb[tap] = ReverbVolume(static_cast<s16>(regs[3 + tap]));
    }
    state.apf[0] = ReverbVolume(static_cast<s16>(regs[8]));
    state.apf[1] = ReverbVolume(static_cast<s16>(regs[9]));
    state.sameSource[0] = ReverbCells(regs[16], reverb.delay, state.cells);
    state.sameSource[1] = ReverbCells(regs[17], reverb.delay, state.cells);
    state.sameTarget[0] = ReverbCells(regs[10], reverb.delay, state.cells);
    state.sameTarget[1] = ReverbCells(regs[11], reverb.delay, state.cells);
    // The different-side sources cross over (R feeds L's line and back)
    state.diffSource[0] = ReverbCells(regs[25], reverb.delay, state.cells);
    state.diffSource[1] = ReverbCells(regs[24], reverb.delay, state.cells);
    state.diffTarget[0] = ReverbCells(regs[18], reverb.delay, state.cells);
    state.diffTarget[1] = ReverbCells(regs[19], reverb.delay, state.cells);
    for (u32 tap = 0; tap < 4; tap++)
    {
        // The comb addresses sit in two pairs (12 to 15, then 20 to 23)
        u32 pair = tap < 2 ? 12 + tap * 2 : 20 + (tap - 2) * 2;
        state.combSource[tap][0] = ReverbCells(regs[pair], reverb.delay, state.cells);
        state.combSource[tap][1] = ReverbCells(regs[pair + 1], reverb.delay, state.cells);
    }
    state.apfTarget[0][0] = ReverbCells(regs[26], reverb.delay, state.cells);
    state.apfTarget[0][1] = ReverbCells(regs[27], reverb.delay, state.cells);
    state.apfTarget[1][0] = ReverbCells(regs[28], reverb.delay, state.cells);
    state.apfTarget[1][1] = ReverbCells(regs[29], reverb.delay, state.cells);
    state.apfSize[0] = ReverbCells(regs[0], reverb.delay, state.cells);
    state.apfSize[1] = ReverbCells(regs[1], reverb.delay, state.cells);
}

// One network tick a side: the published SPU formula (PCSX2's SPU2 DoReverb
// runs the same steps). input is the averaged feed, at most one tick's worth
// of 48 kHz frames. All products are 1.15, all stores saturated.
void TickReverbSide(ReverbState& state, u32 side, s32 input, s32 inputCoef, s32* wet)
{
    s16* cells = state.buffer.data();
    auto at = [&](u32 offset)
    {
        return static_cast<s32>(cells[(state.base + offset) % state.cells]);
    };
    auto store = [&](u32 offset, s32 sample)
    {
        cells[(state.base + offset) % state.cells] = static_cast<s16>(ClampReverb(sample));
    };

    // Same/different-side IIR reflections off the previous pass's outputs
    u32 old = (state.base + state.cells - 1) % state.cells;
    s32 sameOld = cells[(old + state.sameTarget[side]) % state.cells];
    s32 diffOld = cells[(old + state.diffTarget[side]) % state.cells];
    s32 driven = inputCoef * input / 32768;
    s32 same = state.reflection * (driven + state.wall * at(state.sameSource[side]) / 32768 - sameOld) / 32768 + sameOld;
    s32 diff = state.reflection * (driven + state.wall * at(state.diffSource[side]) / 32768 - diffOld) / 32768 + diffOld;
    store(state.sameTarget[side], same);
    store(state.diffTarget[side], diff);

    // Early echo: the 4 comb taps straight off the buffer
    s32 out = 0;
    for (u32 tap = 0; tap < 4; tap++)
    {
        out += state.comb[tap] * at(state.combSource[tap][side]) / 32768;
    }

    // Late reverb: two all-pass stages in series
    for (u32 stage = 0; stage < 2; stage++)
    {
        u32 target = state.apfTarget[stage][side];
        u32 source = (target + state.cells - state.apfSize[stage]) % state.cells;
        s32 delayed = at(source);
        s32 fed = out - state.apf[stage] * delayed / 32768;
        store(target, fed);
        out = delayed + state.apf[stage] * fed / 32768;
    }

    *wet = ClampReverb(out);
}

// One network tick: both sides from their averaged inputs, the wet pair held
// for the frames until the next tick, the work area advancing a cell
void TickReverb(ReverbState& state)
{
    s32 inputLeft = state.inputCount == 0 ? 0 : state.inputLeft / static_cast<s32>(state.inputCount);
    s32 inputRight = state.inputCount == 0 ? 0 : state.inputRight / static_cast<s32>(state.inputCount);
    state.inputLeft = 0;
    state.inputRight = 0;
    state.inputCount = 0;
    state.previousLeft = state.currentLeft;
    state.previousRight = state.currentRight;
    TickReverbSide(state, 0, inputLeft, state.inputCoefLeft, &state.currentLeft);
    TickReverbSide(state, 1, inputRight, state.inputCoefRight, &state.currentRight);
    state.base = (state.base + 1) % state.cells;
}

// A mix frame's reverb: feed the voices' send, tick at 22050 Hz, answer the
// held wet pair through the output volumes
void StepReverb(s32 core, s32 sendLeft, s32 sendRight, s32& wetLeft, s32& wetRight)
{
    ReverbState& state = g_ReverbStates[core];
    if (!state.on || state.cells == 0)
    {
        wetLeft = 0;
        wetRight = 0;
        return;
    }

    state.inputLeft += sendLeft;
    state.inputRight += sendRight;
    state.inputCount++;
    state.phase += ReverbRate;
    while (state.phase >= MixRate)
    {
        state.phase -= MixRate;
        TickReverb(state);
    }

    s32 blendLeft = state.previousLeft + (state.currentLeft - state.previousLeft) * state.phase / MixRate;
    s32 blendRight = state.previousRight + (state.currentRight - state.previousRight) * state.phase / MixRate;
    wetLeft = blendLeft * g_Reverbs[core].volumeLeft / 32768;
    wetRight = blendRight * g_Reverbs[core].volumeRight / 32768;
}

SdlAudio::MusicChannel g_MusicChannels[48];
u8 g_SpuMemory[SdlAudio::SpuMemorySize];

// A sound's region opens with its own header (the "MSVp" the bank's tools
// write: the size, the rate, the sound's name) before its samples. Playing it
// as ADPCM puts those bytes at the front of every sound, heard as a crackle
// when the sound starts, and their flag bits are taken for loop points
constexpr u32 SoundHeaderBytes = 48;

u32 SoundHeaderSkip(const u8* data, u32 size)
{
    return size >= SoundHeaderBytes && std::memcmp(data, "MSVp", 4) == 0 ? SoundHeaderBytes : 0;
}

// A bank read that never registered a sound (SoundEntry::sound)
constexpr u32 NoSound = 0xFFFFFFFF;

struct SoundEntry
{
    bool used = false;
    u32 sound = NoSound; // the bank ID it was registered under
    u32 address = 0;
    u32 size = 0;
    u32 loopStart = 0;
    bool hasLoop = false;
};

// The sound processor's memory allocator: the blocks out of g_SpuMemory (the
// music's buffers, reserved once, and the sound banks' samples, loaded and
// released as the game reads and drops them), freeing and merging blocks so a
// long session's level loads don't run the arena out.
struct SpuBlock
{
    u32 address = 0;
    u32 size = 0;
    bool free = false;
};

std::vector<SpuBlock> g_SpuBlocks;
u32 g_SpuEnd = 0;

// A block of the arena (its size rounded up to 16 bytes, the ADPCM block
// unit): the first free one big enough, else the arena's end. Returns whether
// there was room.
bool SpuAllocate(u32 size, u32& address)
{
    u32 wanted = (size + 15) & ~15u;
    if (wanted == 0)
    {
        address = 0;
        return true;
    }

    for (size_t index = 0; index < g_SpuBlocks.size(); index++)
    {
        if (!g_SpuBlocks[index].free || g_SpuBlocks[index].size < wanted)
        {
            continue;
        }

        SpuBlock& block = g_SpuBlocks[index];
        address = block.address;
        if (block.size > wanted)
        {
            // The rest stays free behind it
            SpuBlock rest{address + wanted, block.size - wanted, true};
            block.size = wanted;
            block.free = false;
            g_SpuBlocks.insert(g_SpuBlocks.begin() + static_cast<std::ptrdiff_t>(index) + 1, rest);
        }
        else
        {
            block.free = false;
        }

        return true;
    }

    if (g_SpuEnd + wanted > SdlAudio::SpuMemorySize)
    {
        return false;
    }

    address = g_SpuEnd;
    g_SpuEnd += wanted;
    g_SpuBlocks.push_back(SpuBlock{address, wanted, false});
    return true;
}

// Lets a block go and merges it with the free blocks around it (the list stays
// sorted by address)
void SpuFree(u32 address)
{
    for (size_t index = 0; index < g_SpuBlocks.size(); index++)
    {
        if (g_SpuBlocks[index].address != address)
        {
            continue;
        }

        g_SpuBlocks[index].free = true;
        if (index > 0 && g_SpuBlocks[index - 1].free)
        {
            g_SpuBlocks[index - 1].size += g_SpuBlocks[index].size;
            g_SpuBlocks.erase(g_SpuBlocks.begin() + static_cast<std::ptrdiff_t>(index));
            index--;
        }

        if (index + 1 < g_SpuBlocks.size() && g_SpuBlocks[index + 1].free)
        {
            g_SpuBlocks[index].size += g_SpuBlocks[index + 1].size;
            g_SpuBlocks.erase(g_SpuBlocks.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        }

        return;
    }
}

// Every free block merged together (CompactSoundMemory's host equivalent of
// retail's moving the sounds in its memory together)
void SpuCoalesce()
{
    for (size_t index = 0; index + 1 < g_SpuBlocks.size();)
    {
        if (g_SpuBlocks[index].free && g_SpuBlocks[index + 1].free)
        {
            g_SpuBlocks[index].size += g_SpuBlocks[index + 1].size;
            g_SpuBlocks.erase(g_SpuBlocks.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        }
        else
        {
            index++;
        }
    }
}

SoundEntry g_Sounds[MaxSounds];
SDL_Mutex* g_MixerMutex = nullptr;
// How many frames the mixer has produced: the sound logs carry it, so a click measured in a dumped mix can be matched to the
// event (a play, a cut) that made it
u64 g_MixedFrames = 0;

// The module's MsPauseAll: the game's sound effects are let go of, while the
// music streams are held in place (not mixed and not advanced), so a paused
// track keeps its position instead of starting over
bool g_HoldMusic = false;

// TS_AUDIOLOG=1: logs the sound processor's decisions - the sounds loaded and played
bool AudioLogOn()
{
    static const bool on = std::getenv("TS_AUDIOLOG") != nullptr;
    return on;
}

// The hold's transitions, for a run asking for the sound log: a hold with no release behind it silences everything
// afterwards, so which call left it is worth saying
void LogPause(const char* what)
{
    if (AudioLogOn())
    {
        std::fprintf(stderr, "audio: %s (music held %d)\n", what, g_HoldMusic ? 1 : 0);
    }
}

SDL_Mutex* MixerMutex()
{
    if (g_MixerMutex == nullptr)
    {
        g_MixerMutex = SDL_CreateMutex();
    }

    return g_MixerMutex;
}

// A voice's volume scaled by its group (4.12 fixed point). Volumes past
// MaxVolume are the SPU2's inverted phase, scaled from the other end
s32 ScaleVolume(s32 groupVolume, s32 volume)
{
    bool inverted = volume > Platform::Audio::MaxVolume;
    if (inverted)
    {
        volume = Platform::Audio::InvertedVolumeBase - volume;
    }

    s32 scaled = volume * groupVolume;
    if (scaled < 0)
    {
        scaled += (1 << GroupVolumeShift) - 1;
    }

    scaled >>= GroupVolumeShift;
    return inverted ? Platform::Audio::InvertedVolumeBase - scaled : scaled;
}

s32 GroupIndex(s32 group)
{
    return (group >> Platform::Audio::GroupShift) - 1;
}

// The SPU2's envelope rate table (PsxRates in PCSX2's ADSR.cpp): four steps
// per shift, from 4 to 4 << 31, capped at 0x3FFFFFFF
u32 g_AdsrRates[160] = {};
bool g_AdsrRatesReady = false;

void EnsureAdsrRates()
{
    if (g_AdsrRatesReady)
    {
        return;
    }

    for (s32 index = 0; index < 160; index++)
    {
        s32 shift = (index - 32) >> 2;
        s64 rate = (index & 3) + 4;
        if (shift < 0)
        {
            rate >>= -shift;
        }
        else
        {
            rate <<= shift;
        }

        g_AdsrRates[index] = static_cast<u32>(rate > 0x3FFFFFFF ? 0x3FFFFFFF : rate);
    }

    g_AdsrRatesReady = true;
}

u32 AdsrRate(s32 index)
{
    if (index < 0)
    {
        return g_AdsrRates[0];
    }
    if (index > 159)
    {
        return g_AdsrRates[159];
    }
    return g_AdsrRates[index];
}

// The inverse-exponential offsets the SPU2's decay, sustain and release use at
// each of the envelope's top three bits
constexpr u32 InverseExpOffsets[8] = {0, 4, 6, 8, 9, 10, 11, 12};

constexpr u32 AdsrMax = 0x7FFFFFFF;

// One frame of the voice's envelope, the hardware's phases and rates
void AdvanceEnvelope(SdlAudio::Envelope& envelope, bool releasing)
{
    if (envelope.phase == 0)
    {
        envelope.value = 0;
        return;
    }

    if (releasing && envelope.phase < 5)
    {
        envelope.phase = 5;
    }

    switch (envelope.phase)
    {
    case 1: // attack
        if (envelope.value == AdsrMax)
        {
            envelope.phase = 2;
            break;
        }

        // Pseudo-exponential above 75%, linear below (and linear mode all the way)
        if (envelope.attackExponential && envelope.value >= 0x60000000)
        {
            envelope.value += AdsrRate((static_cast<s32>(envelope.attackRate) ^ 0x7F) - 0x18 + 32);
        }
        else
        {
            envelope.value += AdsrRate((static_cast<s32>(envelope.attackRate) ^ 0x7F) - 0x10 + 32);
        }

        if (envelope.value > AdsrMax || static_cast<s32>(envelope.value) < 0)
        {
            envelope.value = AdsrMax;
            envelope.phase = 2;
        }
        break;

    case 2: // decay
    {
        u32 offset = InverseExpOffsets[(envelope.value >> 28) & 7];
        s32 index = (static_cast<s32>(envelope.decayRate) ^ 0x1F) * 4 - 0x18 + static_cast<s32>(offset) + 32;
        s64 value = static_cast<s64>(envelope.value) - AdsrRate(index);
        u32 sustain = static_cast<u32>((0x80000000ull / 0x10) * (envelope.sustainLevel + 1) - 1);
        if (value <= sustain)
        {
            envelope.value = value < 0 ? 0 : static_cast<u32>(value);
            envelope.phase = 3;
        }
        else
        {
            envelope.value = static_cast<u32>(value);
        }
        break;
    }

    case 3: // sustain
        // 0x7F disables sustain (it holds forever)
        if (envelope.sustainRate == 0x7F)
        {
            break;
        }

        if (envelope.sustainDecreasing)
        {
            u32 offset = InverseExpOffsets[(envelope.value >> 28) & 7];
            if (envelope.sustainExponential)
            {
                envelope.value -= AdsrRate((static_cast<s32>(envelope.sustainRate) ^ 0x7F) - 0x1B +
                                           static_cast<s32>(offset) + 32);
            }
            else
            {
                envelope.value -= AdsrRate((static_cast<s32>(envelope.sustainRate) ^ 0x7F) - 0xF + 32);
            }

            if (static_cast<s32>(envelope.value) <= 0)
            {
                envelope.value = 0;
                envelope.phase = 4;
            }
        }
        else
        {
            if (envelope.sustainExponential && envelope.value >= 0x60000000)
            {
                envelope.value += AdsrRate((static_cast<s32>(envelope.sustainRate) ^ 0x7F) - 0x18 + 32);
            }
            else
            {
                envelope.value += AdsrRate((static_cast<s32>(envelope.sustainRate) ^ 0x7F) - 0x10 + 32);
            }

            if (static_cast<s32>(envelope.value) < 0)
            {
                envelope.value = AdsrMax;
                envelope.phase = 4;
            }
        }
        break;

    case 4: // sustain end
        envelope.value = envelope.sustainDecreasing ? 0 : AdsrMax;
        if (envelope.value == 0)
        {
            envelope.phase = 6;
        }
        break;

    case 5: // release
        if (envelope.releaseExponential)
        {
            u32 offset = InverseExpOffsets[(envelope.value >> 28) & 7];
            s32 index = (static_cast<s32>(envelope.releaseRate) ^ 0x1F) * 4 - 0x18 + static_cast<s32>(offset) + 32;
            s64 value = static_cast<s64>(envelope.value) - AdsrRate(index);
            envelope.value = value < 0 ? 0 : static_cast<u32>(value);
        }
        else if (envelope.releaseRate != 0x1F)
        {
            s64 value = static_cast<s64>(envelope.value) - (1LL << (0x1F - envelope.releaseRate));
            envelope.value = value < 0 ? 0 : static_cast<u32>(value);
        }

        if (envelope.value == 0)
        {
            envelope.phase = 6;
        }
        break;

    default:
        envelope.value = 0;
        break;
    }
}

// The volume being mixed on its way to the game's value. libsd's
// OpSetVolumeSmooth ramps the register, so a cut does not end the waveform
// mid-cycle.
constexpr s32 VolumeApproach = 64;

void RampVolume(s32& current, s32 target)
{
    if (current == target)
    {
        return;
    }

    s32 difference = target - current;
    s32 step = difference / VolumeApproach;
    if (step == 0)
    {
        step = difference > 0 ? 1 : -1;
    }

    current += step;
}

// A register volume as the mix uses it: past MaxVolume the value is the
// other phase, its magnitude from InvertedVolumeBase down
s32 PhaseVolume(s32 volume)
{
    if (volume > Platform::Audio::MaxVolume)
    {
        return -(Platform::Audio::InvertedVolumeBase - volume);
    }
    return volume;
}

s32 SpuSampleCount(const SdlAudio::Voice& voice)
{
    return static_cast<s32>(voice.spuSize / SdlAudio::AdpcmBlockBytes) * static_cast<s32>(SdlAudio::AdpcmBlockSamples);
}

bool LoadBlock(SdlAudio::Voice& voice, u32 block)
{
    u64 byte = static_cast<u64>(voice.spuAddress) + static_cast<u64>(block) * SdlAudio::AdpcmBlockBytes;
    if (byte + SdlAudio::AdpcmBlockBytes > SdlAudio::SpuMemorySize)
    {
        return false;
    }

    voice.blockFlags = SdlAudio::DecodeAdpcmBlock(&g_SpuMemory[byte], voice.blockSamples, voice.blockOld, voice.blockOlder);
    voice.blockIndex = block;

    return true;
}

// A key-on fades its sound in over this many frames (about 2 ms at 48 kHz):
// the SPU2's own attack can be quicker than a sample's first frames can carry,
// and the change of waveform at full level is heard as a pop
constexpr s32 KeyOnFadeFrames = 96;
// The frames a finished source's last sample takes to fall away by itself,
// before the envelope's release carries the rest
constexpr s32 HoldFadeFrames = 960;

// Starts the voice's release (the module keys a voice off when its passes are
// used up, or a stream ends). A release rate that holds the envelope (0x1F)
// would leave the voice sounding for good, so the module's own rate (0xD,
// about 170 ms) takes over when the play asked for that one
void FinishVoice(SdlAudio::Voice& voice)
{
    voice.releasing = true;
    if (voice.envelope.releaseRate >= 0x1F)
    {
        voice.envelope.releaseRate = 0xD;
    }
}

// The source has run out (its passes are used up, its data ends, or there was
// nothing there): the voice holds what it stopped at and fades on its release
void VoiceSourceOver(SdlAudio::Voice& voice, s32 sample)
{
    FinishVoice(voice);
    voice.holdSample = sample;
    voice.holdFade = HoldFadeFrames;
    voice.sourceOver = true;
}

// The next sample the voice's source has, in order. False when the source is
// over (the voice then fades on what it last sounded like).
bool NextSample(SdlAudio::Voice& voice, s32& sample)
{
    if (voice.source == SdlAudio::VoiceSource::Stream)
    {
        if (voice.stream < 0)
        {
            VoiceSourceOver(voice, 0);
            return false;
        }

        SdlAudio::MusicChannel& channel = g_MusicChannels[voice.stream];
        if (channel.samples.empty())
        {
            channel.playing = false;
            VoiceSourceOver(voice, 0);
            return false;
        }

        if (voice.index >= channel.samples.size())
        {
            if (channel.looping)
            {
                voice.index = 0;
            }
            else
            {
                // The track is over: it stops where it was rather than at
                // silence, so its last frame does not step down to zero
                channel.playing = false;
                VoiceSourceOver(voice, channel.samples.back());
                return false;
            }
        }

        sample = channel.samples[voice.index];
        voice.index++;
        return true;
    }

    if (voice.source != SdlAudio::VoiceSource::Spu)
    {
        return false;
    }

    s32 count = SpuSampleCount(voice);
    if (count == 0)
    {
        VoiceSourceOver(voice, 0);
        return false;
    }

    u32 totalBlocks = static_cast<u32>(count) / SdlAudio::AdpcmBlockSamples;
    if (totalBlocks == 0)
    {
        VoiceSourceOver(voice, 0);
        return false;
    }

    u32 block = static_cast<u32>(voice.index) / SdlAudio::AdpcmBlockSamples;
    if (block != voice.blockIndex)
    {
        // Leaving a block that ends the loop: wrap to the loop point while
        // passes are left. When they are used up the play is over and the
        // voice is let go there; its release fades what the loop had going,
        // rather than the sample stopping dead at the loop point (which is
        // heard as a click)
        bool loopEnded = voice.blockIndex != 0xFFFFFFFF && (voice.blockFlags & SdlAudio::AdpcmLoopEnd) != 0 &&
                         voice.hasLoop && voice.loopStartBlock <= voice.blockIndex;
        if (loopEnded)
        {
            if (voice.loopForever)
            {
                voice.index = static_cast<u64>(voice.loopStartBlock) * SdlAudio::AdpcmBlockSamples;
                voice.blockIndex = 0xFFFFFFFF;
                block = static_cast<u32>(voice.index) / SdlAudio::AdpcmBlockSamples;
            }
            else if (voice.loopsLeft > 1)
            {
                voice.loopsLeft--;
                voice.index = static_cast<u64>(voice.loopStartBlock) * SdlAudio::AdpcmBlockSamples;
                voice.blockIndex = 0xFFFFFFFF;
                block = static_cast<u32>(voice.index) / SdlAudio::AdpcmBlockSamples;
            }
            else
            {
                // The passes are used up: the play ends at the loop-end block,
                // which is where the sound's audio ends. What it last sounded
                // like is held and faded, so the waveform does not stop dead
                s32 last = voice.blockIndex == 0xFFFFFFFF ? 0 : voice.blockSamples[SdlAudio::AdpcmBlockSamples - 1];
                VoiceSourceOver(voice, last);
                return false;
            }
        }
        else if (block >= totalBlocks)
        {
            // Past the data: a sample with no loop point repeats whole while
            // its passes are left, else the play is over (the same fade out)
            if (!voice.loopForever && voice.loopsLeft > 1)
            {
                voice.loopsLeft--;
                voice.index = 0;
                voice.blockIndex = 0xFFFFFFFF;
                voice.blockOld = 0;
                voice.blockOlder = 0;
                block = 0;
            }
            else
            {
                s32 last = voice.blockIndex == 0xFFFFFFFF ? 0 : voice.blockSamples[SdlAudio::AdpcmBlockSamples - 1];
                VoiceSourceOver(voice, last);
                return false;
            }
        }

        if (block >= totalBlocks || !LoadBlock(voice, block))
        {
            VoiceSourceOver(voice, voice.blockSamples[SdlAudio::AdpcmBlockSamples - 1]);
            return false;
        }
    }

    sample = voice.blockSamples[static_cast<u32>(voice.index) % SdlAudio::AdpcmBlockSamples];
    voice.index++;
    return true;
}

// The SPU2's 4-point Gaussian interpolation over the voice's sample history
s32 Interpolate(const SdlAudio::Voice& voice, u32 fraction)
{
    s32 index = static_cast<s32>((fraction & 0xFF0) >> 4);
    if (index < 0)
    {
        index = 0;
    }
    if (index > 0xFF)
    {
        index = 0xFF;
    }

    s64 output = 0;
    output += static_cast<s64>(InterpolationTable[0x0FF - index]) * voice.history[0];
    output += static_cast<s64>(InterpolationTable[0x1FF - index]) * voice.history[1];
    output += static_cast<s64>(InterpolationTable[0x100 + index]) * voice.history[2];
    output += static_cast<s64>(InterpolationTable[0x000 + index]) * voice.history[3];
    return static_cast<s32>(output >> 15);
}

// A frame of the voice's source, interpolated by its pitch counter
bool VoiceFrame(SdlAudio::Voice& voice, s32& output)
{
    if (voice.sourceOver)
    {
        // Nothing left of it: what it stopped at falls away on its own, so the
        // waveform ends without stepping down to silence (the release carries
        // the rest of the voice's time)
        if (voice.holdFade > 1)
        {
            voice.holdSample = static_cast<s32>(static_cast<s64>(voice.holdSample) * (voice.holdFade - 1) /
                                                voice.holdFade);
            voice.holdFade--;
        }
        else
        {
            voice.holdSample = 0;
            voice.holdFade = 0;
        }

        output = voice.holdSample;
        return true;
    }

    if (voice.pitch <= 0)
    {
        output = 0;
        return true;
    }

    // While the counter is past the next sample, pull samples in and shift
    // the history (PV4 <- PV3 <- PV2 <- PV1 <- next)
    u32 step = static_cast<u32>(voice.pitch);
    if (step > 0x3FFF)
    {
        step = 0x3FFF;
    }

    while (voice.fraction >= 0x1000)
    {
        s32 next = 0;
        if (!NextSample(voice, next))
        {
            // The source is over. The history still holds where the waveform
            // was, so that is what is kept (a fresh sample's own last value
            // would step from the interpolated one and be heard as a click)
            voice.holdSample = Interpolate(voice, voice.fraction);
            voice.holdFade = HoldFadeFrames;
            output = voice.holdSample;
            return true;
        }

        voice.history[0] = voice.history[1];
        voice.history[1] = voice.history[2];
        voice.history[2] = voice.history[3];
        voice.history[3] = next;
        voice.fraction -= 0x1000;
    }

    output = Interpolate(voice, voice.fraction);
    voice.fraction += step;
    return true;
}

// What each voice put into the last frame and how far it moved (TS_AUDIOLOG
// only): the frame that pops names the voice behind it
s32 g_VoiceStep[Platform::Audio::Voices] = {};
s32 g_VoiceLevel[Platform::Audio::Voices] = {};
s32 g_VoicePrev[Platform::Audio::Voices] = {};
u32 g_VoicePhase[Platform::Audio::Voices] = {};
u32 g_VoiceAddress[Platform::Audio::Voices] = {};
s32 g_VoicePitch[Platform::Audio::Voices] = {};
s32 g_LastMixLeft = 0;
s32 g_LastMixRight = 0;

// A voice's frame into the dry mix; with its reverb send on, the same
// post-volume signal also feeds its core's network (the EON routing)
void MixVoice(s32 voiceIndex, SdlAudio::Voice& voice, s32& left, s32& right, s32& sendLeft, s32& sendRight)
{
    // What the voice put into the mix last frame, which the pop check reads
    const s32 previousLeft = voice.lastLeft;
    const s32 previousRight = voice.lastRight;
    if (!voice.active.load(std::memory_order_acquire))
    {
        voice.lastLeft = 0;
        voice.lastRight = 0;
        return;
    }

    RampVolume(voice.left, voice.targetLeft);
    RampVolume(voice.right, voice.targetRight);

    s32 sample = 0;
    if (voice.paused)
    {
        // Held in place (the module's pause): the source and its envelope stay
        // where they were while the volume takes it to silence, so the pause
        // does not cut the waveform either
        sample = voice.sourceOver ? voice.holdSample : Interpolate(voice, voice.fraction);
    }
    else
    {
        AdvanceEnvelope(voice.envelope, voice.releasing);
        if (voice.envelope.phase == 6)
        {
            voice.active.store(false, std::memory_order_release);
            voice.lastLeft = 0;
            voice.lastRight = 0;
            return;
        }

        if (!VoiceFrame(voice, sample))
        {
            voice.lastLeft = 0;
            voice.lastRight = 0;
            return;
        }
    }

    s32 level = static_cast<s32>(voice.envelope.value >> 16);
    s32 scaled = static_cast<s32>((static_cast<s64>(sample) * level) / 32768);
    s32 voiceLeft = static_cast<s32>(static_cast<s64>(scaled) * PhaseVolume(voice.left) / Platform::Audio::MaxVolume);
    s32 voiceRight = static_cast<s32>(static_cast<s64>(scaled) * PhaseVolume(voice.right) / Platform::Audio::MaxVolume);

    // A key-on on a voice that was still sounding (the module steals voices):
    // the new sound comes in over the level the old one left, over a couple of
    // milliseconds. A change of waveform at full level in one frame is the pop
    // a re-keyed voice makes
    if (voice.guardFrames > 0)
    {
        s32 progress = (KeyOnFadeFrames - voice.guardFrames) * 65536 / KeyOnFadeFrames;
        voiceLeft = static_cast<s32>(voice.guardLeft + static_cast<s64>(voiceLeft - voice.guardLeft) * progress /
                                                 65536);
        voiceRight = static_cast<s32>(voice.guardRight + static_cast<s64>(voiceRight - voice.guardRight) * progress /
                                                  65536);
        voice.guardFrames--;
    }

    voice.lastLeft = voiceLeft;
    voice.lastRight = voiceRight;

    // TS_AUDIOLOG: what this voice put in and how far it moved, so the frame
    // that pops can name the voice behind it
    if (AudioLogOn())
    {
        s32 stepLeft = voiceLeft - previousLeft;
        s32 stepRight = voiceRight - previousRight;
        if (stepLeft < 0) { stepLeft = -stepLeft; }
        if (stepRight < 0) { stepRight = -stepRight; }
        g_VoiceStep[voiceIndex] = stepLeft > stepRight ? stepLeft : stepRight;
        g_VoiceLevel[voiceIndex] = voiceLeft + voiceRight;
        g_VoicePrev[voiceIndex] = previousLeft + previousRight;
        g_VoicePhase[voiceIndex] = voice.envelope.phase;
        g_VoiceAddress[voiceIndex] = voice.spuAddress;
        g_VoicePitch[voiceIndex] = voice.pitch;
    }

    left += voiceLeft;
    right += voiceRight;
    if (voice.reverbSend)
    {
        sendLeft += voiceLeft;
        sendRight += voiceRight;
    }
}

// Puts a voice at the start of an SPU sound (a key-on)
void KeyOnSpu(SdlAudio::Voice& voice, u32 address, u32 size, u32 loopStart, bool hasLoop, s32 pitch, s32 group,
              s16 left, s16 right, u32 attack, u32 release, u32 loops)
{
    voice.source = SdlAudio::VoiceSource::Spu;
    voice.stream = -1;
    voice.spuAddress = address;
    voice.spuSize = size;
    voice.loopStartBlock = loopStart;
    voice.hasLoop = hasLoop;
    voice.index = 0;
    voice.fraction = 0;
    voice.pitch = pitch;
    for (s32& value : voice.history)
    {
        value = 0;
    }
    voice.blockIndex = 0xFFFFFFFF;
    voice.blockFlags = 0;
    voice.blockOld = 0;
    voice.blockOlder = 0;
    voice.loopsLeft = loops;
    voice.loopForever = loops == 0 && hasLoop;
    voice.releasing = false;
    voice.group = group;
    voice.rawLeft = left;
    voice.rawRight = right;
    s32 index = GroupIndex(group);
    s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
    s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
    voice.targetLeft = ScaleVolume(groupLeft, left);
    voice.targetRight = ScaleVolume(groupRight, right);
    voice.left = voice.targetLeft;
    voice.right = voice.targetRight;
    voice.envelope = SdlAudio::Envelope();
    voice.envelope.attackRate = attack & 0x7F;
    voice.envelope.releaseRate = release & 0x1F;
    voice.envelope.phase = 1;
    voice.envelope.value = 0;
    voice.paused = false;
    // A voice that was still sounding hands its level to the fade the new
    // sound comes in over (a silent one just fades in)
    voice.guardFrames = KeyOnFadeFrames;
    voice.guardLeft = voice.active.load(std::memory_order_acquire) ? voice.lastLeft : 0;
    voice.guardRight = voice.active.load(std::memory_order_acquire) ? voice.lastRight : 0;
    voice.sourceOver = false;
    voice.holdSample = 0;
    voice.holdFade = 0;
    voice.active.store(true, std::memory_order_release);
}

// Puts a music channel's voice at the start of its stream (a key-on). The
// stream is already decoded; the volume and pitch were set by the game's
// SetVoiceVolume and SetVoicePitch, exactly as it does on the PS2.
void KeyOnStream(SdlAudio::Voice& voice, s32 channel, s32 group, s32 pitch)
{
    voice.source = SdlAudio::VoiceSource::Stream;
    voice.stream = channel;
    voice.spuAddress = 0;
    voice.spuSize = 0;
    voice.hasLoop = g_MusicChannels[channel].looping;
    voice.loopStartBlock = 0;
    voice.index = 0;
    voice.fraction = 0;
    voice.pitch = pitch;
    for (s32& value : voice.history)
    {
        value = 0;
    }
    voice.blockIndex = 0xFFFFFFFF;
    voice.loopsLeft = 0;
    voice.loopForever = g_MusicChannels[channel].looping;
    voice.releasing = false;
    voice.group = group;
    voice.envelope = SdlAudio::Envelope();
    voice.envelope.attackRate = 0;
    voice.envelope.releaseRate = 0xD;
    voice.envelope.phase = 1;
    voice.envelope.value = 0;
    s32 index = GroupIndex(group);
    s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
    s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
    voice.targetLeft = ScaleVolume(groupLeft, voice.rawLeft);
    voice.targetRight = ScaleVolume(groupRight, voice.rawRight);
    voice.left = voice.targetLeft;
    voice.right = voice.targetRight;
    voice.paused = false;
    // The same fade in as a sound's key-on: a track starting over a voice
    // that was still sounding does not change waveform at full level at once
    voice.guardFrames = KeyOnFadeFrames;
    voice.guardLeft = voice.active.load(std::memory_order_acquire) ? voice.lastLeft : 0;
    voice.guardRight = voice.active.load(std::memory_order_acquire) ? voice.lastRight : 0;
    voice.sourceOver = false;
    voice.holdSample = 0;
    voice.holdFade = 0;
    voice.active.store(true, std::memory_order_release);
}

// A movie's PCM: de-interleaved stereo frames looping dry into the mix (no
// reverb send: retail plays it through the SPU2's input, past the effects)
struct MoviePcm
{
    bool playing = false;
    std::vector<s16> frames;
    u32 position = 0;
    s32 volume = 0;
    // The volume being ramped to: a movie stopped (or skipped) with its sound still sounding would otherwise be cut
    // mid-waveform, which is heard as a click
    s32 targetVolume = 0;
};
static MoviePcm g_MoviePcm;

// One of the movie's frames into the mix: it is the sound module's own stream (a movie lends the module), so a pause of the
// game's voices holds it not
void MoviePcmFrame(s32& left, s32& right)
{
    if (!g_MoviePcm.playing || g_MoviePcm.frames.empty())
    {
        return;
    }

    if (g_MoviePcm.volume != g_MoviePcm.targetVolume)
    {
        g_MoviePcm.volume += (g_MoviePcm.targetVolume - g_MoviePcm.volume) / VolumeApproach +
                             (g_MoviePcm.volume < g_MoviePcm.targetVolume ? 1 : -1);
    }

    u32 at = g_MoviePcm.position * 2;
    if (at + 1 >= g_MoviePcm.frames.size())
    {
        // Its sound is over: it stays silent rather than starting over (a movie's sound runs its length)
        g_MoviePcm.targetVolume = 0;
        return;
    }

    left += g_MoviePcm.frames[at] * g_MoviePcm.volume / (1 << MovieVolumeShift);
    right += g_MoviePcm.frames[at + 1] * g_MoviePcm.volume / (1 << MovieVolumeShift);
    g_MoviePcm.position++;
}
}

void SdlAudio::LockMixer()
{
    SDL_LockMutex(MixerMutex());
}

void SdlAudio::UnlockMixer()
{
    SDL_UnlockMutex(MixerMutex());
}

void SdlAudio::MixStereo(s16* out, u32 frames)
{
    // Once a second the options the player set are written back to the host's settings file. It reads the game's own values
    // and touches the file, so it happens outside the mixer's lock
    static u32 settingsSince = 0;
    settingsSince += frames;
    if (settingsSince >= MixRate)
    {
        settingsSince = 0;
        HostSettingsKeep();
    }

    // A run asking for the sound log gets the mixer's own timing as well: a
    // callback that waits long on the lock, or takes long itself, is exactly
    // where the device runs dry and everything stops for a moment
    const bool logTiming = AudioLogOn();
    const u64 lockAt = logTiming ? SDL_GetTicksNS() : 0;
    LockMixer();
    const u64 mixAt = logTiming ? SDL_GetTicksNS() : 0;
    if (logTiming && (mixAt - lockAt) >= 8000000ull)
    {
        std::fprintf(stderr, "audio: waited %llu ms for the mixer's lock at frame %llu\n",
                     static_cast<unsigned long long>((mixAt - lockAt) / 1000000ull),
                     static_cast<unsigned long long>(g_MixedFrames));
    }

    EnsureAdsrRates();
    if (AudioLogOn())
    {
        // Once a second: how many voices the mixer is holding, so a leak (voices a sound left behind) shows
        static u32 since = 0;
        since += frames;
        if (since >= MixRate)
        {
            since = 0;
            u32 active = 0;
            u32 stuckAttack = 0;
            u32 stuckRate = 0;
            for (u32 index = 0; index < Platform::Audio::Voices; index++)
            {
                const SdlAudio::Voice& voice = g_Voices[index];
                if (!voice.active.load(std::memory_order_acquire))
                {
                    continue;
                }

                active++;
                // A play whose attack rate never rises holds its voice in the
                // attack for ever: it is never heard and never gives the voice
                // back, which is where a run's later sounds go missing
                if (voice.envelope.phase == 1 &&
                    AdsrRate((static_cast<s32>(voice.envelope.attackRate) ^ 0x7F) - 0x10 + 32) == 0)
                {
                    stuckAttack++;
                    stuckRate = voice.envelope.attackRate;
                }
            }

            std::fprintf(stderr, "audio: voices %u/%u active (music held %d)\n", active, Platform::Audio::Voices,
                         g_HoldMusic ? 1 : 0);
            // The mix's own headroom, once a second: frames the knee had to
            // bend. A moment that bends most of a second is one the sounds are
            // overdriving, and it is where the hard top used to be heard
            if (g_BentFrames != 0)
            {
                std::fprintf(stderr, "audio: the mix bent its top on %llu of %u frames\n",
                             static_cast<unsigned long long>(g_BentFrames), static_cast<u32>(MixRate));
            }

            g_BentFrames = 0;
            if (stuckAttack != 0)
            {
                std::fprintf(stderr,
                             "audio: %u voice(s) held by an attack that never runs (the play's parameter1 = %u)\n",
                             stuckAttack, stuckRate);
            }
        }
    }

    g_MixedFrames += frames;
    for (u32 frame = 0; frame < frames; frame++)
    {
        s32 left = 0;
        s32 right = 0;
        s32 sendLeft[Platform::Audio::Cores] = {};
        s32 sendRight[Platform::Audio::Cores] = {};
        if (logTiming)
        {
            for (u32 voice = 0; voice < Platform::Audio::Voices; voice++)
            {
                g_VoiceStep[voice] = 0;
                g_VoiceLevel[voice] = 0;
                g_VoicePrev[voice] = 0;
            }
        }

        for (u32 voice = 0; voice < Platform::Audio::Voices; voice++)
        {
            s32 core = static_cast<s32>(voice / Platform::Audio::VoicesPerCore);
            MixVoice(static_cast<s32>(voice), g_Voices[voice], left, right, sendLeft[core], sendRight[core]);
        }

        for (s32 core = 0; core < Platform::Audio::Cores; core++)
        {
            s32 wetLeft = 0;
            s32 wetRight = 0;
            StepReverb(core, sendLeft[core], sendRight[core], wetLeft, wetRight);
            left += wetLeft;
            right += wetRight;
        }

        MoviePcmFrame(left, right);

        // The output stage: the level back to zero and the top of the range
        // bent rather than squared off (see OutputChannel)
        bool bent = false;
        left = OutputChannel(left, g_OutputStage.inLeft, g_OutputStage.outLeft, bent);
        right = OutputChannel(right, g_OutputStage.inRight, g_OutputStage.outRight, bent);
        if (bent)
        {
            g_BentFrames++;
        }

        // TS_AUDIOLOG: the frame the whole mix steps a long way on names the
        // voice that did it, with where it was reading and what its envelope
        // was doing
        if (logTiming)
        {
            s32 mixStep = left - g_LastMixLeft;
            s32 mixStepRight = right - g_LastMixRight;
            if (mixStep < 0) { mixStep = -mixStep; }
            if (mixStepRight < 0) { mixStepRight = -mixStepRight; }
            if (mixStep > 4500 || mixStepRight > 4500)
            {
                s32 worst = 0;
                for (s32 index = 1; index < Platform::Audio::Voices; index++)
                {
                    if (g_VoiceStep[index] > g_VoiceStep[worst]) { worst = index; }
                }

                std::fprintf(stderr,
                             "audio: mix pop at %llu step %d/%d  %d->%d   voice %d moved %d (%d->%d) phase %u "
                             "address %u pitch %d reading %llu of %u loop %d passes %d guard %d over %d\n",
                             static_cast<unsigned long long>(g_MixedFrames + frame), mixStep, mixStepRight,
                             g_LastMixLeft, left, worst, g_VoiceStep[worst], g_VoicePrev[worst],
                             g_VoiceLevel[worst], g_VoicePhase[worst], g_VoiceAddress[worst], g_VoicePitch[worst],
                             static_cast<unsigned long long>(g_Voices[worst].index),
                             static_cast<u32>(SpuSampleCount(g_Voices[worst])), g_Voices[worst].hasLoop ? 1 : 0,
                             static_cast<s32>(g_Voices[worst].loopsLeft), static_cast<s32>(g_Voices[worst].guardFrames),
                             g_Voices[worst].sourceOver ? 1 : 0);
            }
        }

        g_LastMixLeft = left;
        g_LastMixRight = right;

        out[frame * 2] = static_cast<s16>(left);
        out[frame * 2 + 1] = static_cast<s16>(right);
    }

    if (logTiming && (SDL_GetTicksNS() - mixAt) >= 16000000ull)
    {
        // A 1024 frame chunk is 21 ms of the device's time: longer than that
        // and the next callback arrives with the buffer already empty
        std::fprintf(stderr, "audio: %u frames mixed in %llu ms at frame %llu\n", frames,
                     static_cast<unsigned long long>((SDL_GetTicksNS() - mixAt) / 1000000ull),
                     static_cast<unsigned long long>(g_MixedFrames));
    }

    UnlockMixer();
}

u8* SdlAudio::SpuMemory()
{
    return g_SpuMemory;
}

// A sound bank's samples: into the sound processor's memory (its own arena,
// blocks freed and merged as sounds are released), its ADPCM blocks scanned
// for the loop flags, then registered under the bank's ID so
// Platform::Audio::PlaySound can find it. A bank loaded again at the same ID
// takes its old block over (retail loads each sound once, but a level
// reloaded would otherwise leak its block).
u32 SdlAudio::LoadSoundBankSamples(u32 sound, const u8* data, u32 size)
{
    if (sound >= MaxSounds || data == nullptr || size == 0)
    {
        if (AudioLogOn())
        {
            std::fprintf(stderr, "audio: load sound %u size=%u REFUSED (max %u data %p)\n", sound, size, MaxSounds,
                         static_cast<const void*>(data));
        }

        return 0;
    }

    LockMixer();
    if (g_Sounds[sound].used && g_Sounds[sound].size != 0)
    {
        SpuFree(g_Sounds[sound].address);
        g_Sounds[sound] = SoundEntry();
    }

    u32 skip = SoundHeaderSkip(data, size);
    const u8* samples = data + skip;
    u32 sampleBytes = size - skip;
    if (sampleBytes == 0)
    {
        SdlAudio::UnlockMixer();
        return 0;
    }

    u32 address = 0;
    if (!SpuAllocate(sampleBytes, address))
    {
        if (AudioLogOn())
        {
            std::fprintf(stderr, "audio: load sound %u size=%u FAILED (no room)\n", sound, size);
        }

        UnlockMixer();
        return 0;
    }

    u32 blocks = sampleBytes / SdlAudio::AdpcmBlockBytes;

    // The block its loop-end flag sits on is the sound's last audio block: the
    // bank's tools append a marker block after it (flag 0x07, the samples all
    // 0x7777) which decodes to full scale DC, so a play reading to the end of
    // the region instead of to that flag is heard as a pop at the end of every
    // sound
    for (u32 block = 0; block < blocks; block++)
    {
        if ((samples[block * SdlAudio::AdpcmBlockBytes + 1] & SdlAudio::AdpcmLoopEnd) != 0)
        {
            blocks = block + 1;
            sampleBytes = blocks * SdlAudio::AdpcmBlockBytes;
            break;
        }
    }

    std::memcpy(&g_SpuMemory[address], samples, sampleBytes);

    u32 loopStart = 0;
    bool hasLoop = false;
    for (u32 block = 0; block < blocks; block++)
    {
        if ((samples[block * SdlAudio::AdpcmBlockBytes + 1] & SdlAudio::AdpcmLoopStart) != 0)
        {
            loopStart = block;
            hasLoop = true;
            break;
        }
    }

    // TS_SFXDUMP=<dir>: the sound's own bytes, so a decode can be checked
    // against the data the game actually handed over
    if (const char* dir = std::getenv("TS_SFXDUMP"); dir != nullptr && dir[0] != '\0')
    {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%u.adpcm", dir, sound);
        if (std::FILE* file = std::fopen(path, "wb"); file != nullptr)
        {
            std::fwrite(samples, 1, sampleBytes, file);
            std::fclose(file);
        }
    }

    g_Sounds[sound].used = true;
    g_Sounds[sound].sound = sound;
    g_Sounds[sound].address = address;
    g_Sounds[sound].size = sampleBytes;
    g_Sounds[sound].loopStart = loopStart;
    g_Sounds[sound].hasLoop = hasLoop;
    if (AudioLogOn())
    {
        std::fprintf(stderr, "audio: load sound %u size=%u -> %u loop=%d header=%u\n", sound, size, address,
                     hasLoop ? 1 : 0, skip);
    }

    UnlockMixer();
    return address;
}

void SdlAudio::MoviePcmOpen(u32 frames, const s16* stereo)
{
    LockMixer();
    g_MoviePcm = MoviePcm();
    if (frames != 0 && stereo != nullptr)
    {
        g_MoviePcm.frames.assign(stereo, stereo + static_cast<size_t>(frames) * 2);
    }
    UnlockMixer();
}

void SdlAudio::MoviePcmStart(f32 volume)
{
    LockMixer();
    g_MoviePcm.playing = !g_MoviePcm.frames.empty();
    g_MoviePcm.position = 0;
    s32 scaled = static_cast<s32>(volume * (1 << MovieVolumeShift));
    g_MoviePcm.volume = scaled < 0 ? 0 : scaled;
    g_MoviePcm.targetVolume = g_MoviePcm.volume;
    UnlockMixer();
}

void SdlAudio::MoviePcmStop()
{
    // Its sound is let go at once (the game has the screen back), which is heard as a click: it is ramped down and the movie's
    // samples stay to be mixed while the volume goes to nothing
    LockMixer();
    g_MoviePcm.targetVolume = 0;
    UnlockMixer();
}

void SdlAudio::StartMoviePcm(f32 volume)
{
    MoviePcmStart(volume);
}

void SdlAudio::StopMoviePcm()
{
    MoviePcmStop();
}

u32 SdlAudio::ReverbImpulseChecksum(s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    if (mode <= 0 || mode >= ReverbModes)
    {
        return 2166136261u;
    }

    // A scratch core cooked from the settings (full output volumes, like the
    // game core's init): a full-scale impulse, then silence, hashed wet
    LockMixer();
    SdlAudio::Reverb saved = g_Reverbs[0];
    ReverbState savedState = g_ReverbStates[0];
    g_Reverbs[0].mode = mode;
    g_Reverbs[0].depthLeft = depthLeft;
    g_Reverbs[0].depthRight = depthRight;
    g_Reverbs[0].delay = delay;
    g_Reverbs[0].feedback = feedback;
    g_Reverbs[0].volumeLeft = 32767;
    g_Reverbs[0].volumeRight = 32767;
    CookReverb(0);
    ReverbState& state = g_ReverbStates[0];
    u32 hash = 2166136261u;
    auto fold = [&](s32 sample)
    {
        for (u32 byte = 0; byte < 2; byte++)
        {
            hash ^= static_cast<u32>(sample >> (byte * 8)) & 0xFF;
            hash *= 16777619u;
        }
    };
    for (u32 tick = 0; tick < ReverbRate; tick++)
    {
        state.inputLeft += tick == 0 ? 32767 : 0;
        state.inputRight += tick == 0 ? 32767 : 0;
        state.inputCount++;
        TickReverb(state);
        fold(state.currentLeft);
        fold(state.currentRight);
    }
    g_Reverbs[0] = saved;
    g_ReverbStates[0] = savedState;
    UnlockMixer();
    return hash;
}

void Platform::Audio::Reset()
{
    // The sound processor starting fresh is where the game puts its defaults: the host's settings file, if there is one, is
    // put in first (it calls SetGroupVolume, which takes the mixer's lock, so before it is taken here)
    HostSettingsApplyOnce();
    SdlAudio::LockMixer();
    g_HoldMusic = false;
    for (SdlAudio::Voice& voice : g_Voices)
    {
        voice.active.store(false, std::memory_order_release);
        voice.source = SdlAudio::VoiceSource::None;
        voice.stream = -1;
        voice.paused = false;
        voice.spuAddress = 0;
        voice.spuSize = 0;
        voice.loopStartBlock = 0;
        voice.hasLoop = false;
        voice.index = 0;
        voice.fraction = 0;
        voice.pitch = 0x1000;
        for (s32& value : voice.history)
        {
            value = 0;
        }
        voice.blockIndex = 0xFFFFFFFF;
        voice.blockFlags = 0;
        voice.blockOld = 0;
        voice.blockOlder = 0;
        voice.loopsLeft = 0;
        voice.loopForever = false;
        voice.envelope = SdlAudio::Envelope();
        voice.releasing = false;
        voice.sourceOver = false;
        voice.holdSample = 0;
        voice.holdFade = 0;
        voice.guardFrames = 0;
        voice.guardLeft = 0;
        voice.guardRight = 0;
        voice.lastLeft = 0;
        voice.lastRight = 0;
        voice.rawLeft = 0;
        voice.rawRight = 0;
        voice.group = 0;
        voice.left = 0;
        voice.right = 0;
        voice.targetLeft = 0;
        voice.targetRight = 0;
        voice.reverbSend = false;
    }

    for (SdlAudio::MusicChannel& channel : g_MusicChannels)
    {
        channel = SdlAudio::MusicChannel();
    }

    for (s32 core = 0; core < Platform::Audio::Cores; core++)
    {
        g_Reverbs[core] = SdlAudio::Reverb();
        g_ReverbStates[core] = ReverbState();
    }

    g_MoviePcm = MoviePcm();
    ResetOutputStage();

    for (SoundEntry& sound : g_Sounds)
    {
        sound = SoundEntry();
    }

    g_SpuBlocks.clear();
    g_SpuEnd = 0;
    for (s32 group = 0; group < 4; group++)
    {
        g_GroupVolumesLeft[group] = Platform::Audio::FullGroupVolume;
        g_GroupVolumesRight[group] = Platform::Audio::FullGroupVolume;
    }

    SdlAudio::UnlockMixer();
}

// A voice's volumes back to the ones its registers hold: a pause took them to
// silence, and the resume fades them back rather than jumping there
void RetargetVoice(SdlAudio::Voice& voice)
{
    s32 index = GroupIndex(voice.group);
    s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
    s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
    voice.targetLeft = ScaleVolume(groupLeft, voice.rawLeft);
    voice.targetRight = ScaleVolume(groupRight, voice.rawRight);
}

void Platform::Audio::PauseAll()
{
    // The module's MsPauseAll: playing sounds are let go of, while music is
    // held in place (not mixed and not advanced), so a track keeps its
    // position and does not restart when the game resumes.
    SdlAudio::LockMixer();
    for (SdlAudio::Voice& voice : g_Voices)
    {
        if (!voice.active.load(std::memory_order_acquire))
        {
            continue;
        }

        if (voice.source == SdlAudio::VoiceSource::Stream)
        {
            // Held rather than cut: the volume reaches silence over the
            // module's own ramp, the source and its envelope staying put
            voice.paused = true;
            voice.targetLeft = 0;
            voice.targetRight = 0;
        }
        else
        {
            voice.releasing = true;
        }
    }

    g_HoldMusic = true;
    LogPause("PauseAll");
    SdlAudio::UnlockMixer();
}

void Platform::Audio::ResumeAll()
{
    SdlAudio::LockMixer();
    for (SdlAudio::Voice& voice : g_Voices)
    {
        if (voice.paused)
        {
            voice.paused = false;
            RetargetVoice(voice);
        }
    }

    g_HoldMusic = false;
    LogPause("ResumeAll");
    SdlAudio::UnlockMixer();
}

s32 Platform::Audio::LendToMovie()
{
    // Retail lends the sound module (and the SPU's callback) to the movie: its
    // MsDisableSpuCallback stops the module's key-ons, so no music or sound
    // plays under the movie. The host stops the music's streams here; the game
    // restarts whatever it wants when the movie ends.
    SdlAudio::LockMixer();
    for (s32 channel = 0; channel < 48; channel++)
    {
        SdlAudio::MusicChannel& music = g_MusicChannels[channel];
        music.playing = false;
        if (music.bound && music.voice < Platform::Audio::Voices)
        {
            SdlAudio::Voice& voice = g_Voices[music.voice];
            if (voice.source == SdlAudio::VoiceSource::Stream && voice.stream == channel)
            {
                voice.releasing = true;
            }
        }
    }

    g_MoviePcm = MoviePcm();
    g_HoldMusic = true;
    LogPause("LendToMovie");
    SdlAudio::UnlockMixer();
    return 0;
}

s32 Platform::Audio::ReclaimFromMovie()
{
    // The module is the game's again
    SdlAudio::LockMixer();
    for (SdlAudio::Voice& voice : g_Voices)
    {
        if (voice.paused)
        {
            voice.paused = false;
            RetargetVoice(voice);
        }
    }

    g_HoldMusic = false;
    LogPause("ReclaimFromMovie");
    SdlAudio::UnlockMixer();
    return 0;
}

void Platform::Audio::CompactSoundMemory()
{
    // Retail moves the sounds in its memory together over the gaps released
    // ones left. The host's blocks are addressed by where they are, so moving
    // them is unnecessary: merging the free blocks gets the same usable space
    SdlAudio::LockMixer();
    SpuCoalesce();
    SdlAudio::UnlockMixer();
}

void Platform::Audio::SetGroupVolume(s32 group, u32 left, u32 right)
{
    u32 index = static_cast<u32>(group) >> Platform::Audio::GroupShift;
    if (index == 0 || index > 4 || left > static_cast<u32>(Platform::Audio::FullGroupVolume) ||
        right > static_cast<u32>(Platform::Audio::FullGroupVolume))
    {
        return;
    }

    if (AudioLogOn())
    {
        std::fprintf(stderr, "audio: group %u volume %u/%u\n", index, left, right);
    }

    SdlAudio::LockMixer();
    g_GroupVolumesLeft[index - 1] = static_cast<s32>(left);
    g_GroupVolumesRight[index - 1] = static_cast<s32>(right);
    for (u32 voice = 0; voice < Platform::Audio::Voices; voice++)
    {
        SdlAudio::Voice& talking = g_Voices[voice];
        if (talking.group != group)
        {
            continue;
        }

        talking.targetLeft = ScaleVolume(g_GroupVolumesLeft[index - 1], talking.rawLeft);
        talking.targetRight = ScaleVolume(g_GroupVolumesRight[index - 1], talking.rawRight);
    }

    SdlAudio::UnlockMixer();
}

s32 Platform::Audio::SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    if (voice < 0 || voice >= Platform::Audio::Voices)
    {
        return -1;
    }

    SdlAudio::LockMixer();
    SdlAudio::Voice& talking = g_Voices[voice];
    if (AudioLogOn() && (talking.left != 0 || talking.right != 0) && left == 0 && right == 0)
    {
        // A sound cut to silence at once: its waveform ends mid-cycle, which is heard as a click
        std::fprintf(stderr, "audio: voice %d CUT to 0 (was %d/%d) at %llu\n", voice, talking.left, talking.right,
                     static_cast<unsigned long long>(g_MixedFrames));
        // Who asked for the cut: every one of these is a sound the player
        // hears stopping in the middle, so the caller names itself
        void* callers[16];
        int callerCount = backtrace(callers, 16);
        char** names = backtrace_symbols(callers, callerCount);
        for (int caller = 1; caller < callerCount && caller < 8; caller++)
        {
            std::fprintf(stderr, "audio:   cut from %s\n", names[caller]);
        }

        std::free(names);
    }

    talking.rawLeft = left;
    talking.rawRight = right;
    s32 index = GroupIndex(talking.group);
    s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
    s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
    talking.targetLeft = ScaleVolume(groupLeft, left);
    talking.targetRight = ScaleVolume(groupRight, right);
    SdlAudio::UnlockMixer();
    return 0;
}

s32 Platform::Audio::SetVoicePitch(s32 voice, u16 pitch)
{
    if (voice < 0 || voice >= Platform::Audio::Voices)
    {
        return -1;
    }

    SdlAudio::LockMixer();
    g_Voices[voice].pitch = pitch;
    SdlAudio::UnlockMixer();
    return 0;
}

void Platform::Audio::SetVoiceReverb(s32 voice, bool on)
{
    if (voice < 0 || voice >= Platform::Audio::Voices)
    {
        return;
    }

    SdlAudio::LockMixer();
    g_Voices[voice].reverbSend = on;
    SdlAudio::UnlockMixer();
}

void Platform::Audio::ReleaseVoice(s32 voice, u32 rate)
{
    if (voice < 0 || voice >= Platform::Audio::Voices || rate >= 0x20)
    {
        return;
    }

    SdlAudio::LockMixer();
    SdlAudio::Voice& talking = g_Voices[voice];
    if (talking.active.load(std::memory_order_acquire))
    {
        talking.releasing = true;
        talking.envelope.releaseRate = rate;
    }
    SdlAudio::UnlockMixer();
}

s32 Platform::Audio::IsVoiceFree(s32 voice)
{
    if (voice < 0 || voice >= Platform::Audio::Voices)
    {
        return -1;
    }

    bool free = !g_Voices[voice].active.load(std::memory_order_acquire);
    return free ? 1 : 0;
}

void Platform::Audio::SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    if (core < 0 || core >= Platform::Audio::Cores || mode < 0 || mode >= ReverbModes)
    {
        return;
    }

    SdlAudio::LockMixer();
    SdlAudio::Reverb& reverb = g_Reverbs[core];
    reverb.mode = mode;
    reverb.depthLeft = depthLeft;
    reverb.depthRight = depthRight;
    reverb.delay = delay;
    reverb.feedback = feedback;
    CookReverb(core);
    SdlAudio::UnlockMixer();
}

void Platform::Audio::ClearReverb(s32 core)
{
    if (core < 0 || core >= Platform::Audio::Cores)
    {
        return;
    }

    SdlAudio::LockMixer();
    g_Reverbs[core] = SdlAudio::Reverb();
    CookReverb(core);
    SdlAudio::UnlockMixer();
}

void Platform::Audio::SetReverbVolume(s32 core, s16 left, s16 right)
{
    if (core < 0 || core >= Platform::Audio::Cores)
    {
        return;
    }

    SdlAudio::LockMixer();
    g_Reverbs[core].volumeLeft = left;
    g_Reverbs[core].volumeRight = right;
    SdlAudio::UnlockMixer();
}

u32 Platform::Audio::ReserveSoundMemory(u32 size)
{
    // The music players' buffers, reserved once at start-up out of the same
    // memory (retail's g_MsSoundBankAddress starts the banks past them)
    SdlAudio::LockMixer();
    u32 address = 0;
    if (!SpuAllocate(size, address))
    {
        address = 0;
    }

    SdlAudio::UnlockMixer();
    return address;
}

void Platform::Audio::SoundBankLoaded(u16 bank)
{
    // The bank's samples are already registered (ReadSoundBank, which found
    // their sound's ID as it copied them in). Retail's module marks its own
    // table loaded here; the host's g_Sounds entries are marked as they land.
    (void)bank;
}

void Platform::Audio::ReserveSounds(u16 count)
{
    // Retail reserves a table of sound slots in the I/O processor's memory
    // (MsReserveSounds's OpAllocateSoundTable). The host's table is fixed
    // (g_Sounds), sized well past any bank, so there is nothing to reserve
    (void)count;
}

void Platform::Audio::ReleaseSound(u32 sound)
{
    if (sound < MaxSounds)
    {
        SdlAudio::LockMixer();
        if (g_Sounds[sound].used && g_Sounds[sound].size != 0)
        {
            SpuFree(g_Sounds[sound].address);
        }

        g_Sounds[sound] = SoundEntry();
        SdlAudio::UnlockMixer();
    }
}

s32 Platform::Audio::PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release,
                               u32 loops)
{
    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= Platform::Audio::Voices || sound >= MaxSounds || loops > 0xFFFF)
    {
        if (AudioLogOn())
        {
            std::fprintf(stderr, "audio: play sound %u voice %u REJECTED (voices %d max %u loops %u)\n", sound, voice,
                         Platform::Audio::Voices, MaxSounds, loops);
        }

        return voice >= Platform::Audio::Voices ? -1 : -2;
    }

    s32 group = static_cast<s32>(voiceAndGroup & 0xFFFF0000);

    SdlAudio::LockMixer();
    EnsureAdsrRates();
    if (!g_Sounds[sound].used)
    {
        if (AudioLogOn())
        {
            std::fprintf(stderr, "audio: play sound %u voice %u NOT LOADED\n", sound, voice);
        }

        SdlAudio::UnlockMixer();
        return 0;
    }

    if (AudioLogOn())
    {
        s32 index = GroupIndex(group);
        s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
        s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
        std::fprintf(stderr,
                     "audio: play sound %u voice %u vol %d/%d pitch %u attack %u release %u loops %u group %d/%d%s at %llu\n",
                     sound, voice, left, right, pitch, attack, release, loops, groupLeft, groupRight,
                     g_Voices[voice].active.load(std::memory_order_acquire) ? " ON A VOICE ALREADY PLAYING" : "",
                     static_cast<unsigned long long>(g_MixedFrames));
    }

    SdlAudio::Voice& talking = g_Voices[voice];
    if (talking.source == SdlAudio::VoiceSource::Stream && talking.stream >= 0 && talking.stream < 48)
    {
        g_MusicChannels[talking.stream].playing = false;
    }
    KeyOnSpu(talking, g_Sounds[sound].address, g_Sounds[sound].size, g_Sounds[sound].loopStart,
             g_Sounds[sound].hasLoop, pitch, group, left, right, attack, release, loops);
    talking.reverbSend = false;
    SdlAudio::UnlockMixer();
    return 0;
}

s32 Platform::Audio::PitchOfRate(s32 rate)
{
    return (rate << 12) / 48000;
}

// Retail's answer is the stream's buffer in the I/O processor's memory
// (MsGetStreamStatus's iopBuffer), which the movie writes its sound into.
// The host streams from the file, so only the game's use of it matters:
// LendMusicBuffer refuses to lend a player (and so leaves its music
// playing) when the address is 0, so it must look like a real buffer.
u32 Platform::Audio::ChannelBufferAddress(s32 channel)
{
    return static_cast<u32>(channel) + 1;
}

// The music the next StreamMusic will play: ReadMusic names its file, place
// and length, StreamMusic hands them to the channel it starts (the retail's
// SDRDRV keeps one such pending read).
struct PendingMusic
{
    s32 file = -1;
    u32 offset = 0;
    u32 size = 0;
};
PendingMusic g_PendingMusic;

// A track's ADPCM region decoded into PCM: the channel's own blocks (a mono
// track's all of them; an interleaved track's even or odd blockSize chunks),
// to the region's end. Decoded once: a music channel then plays plain
// samples, looping the whole lot when the request loops.
void DecodeMusicChannel(SdlAudio::MusicChannel& channel, const u8* region, u32 regionSize, bool oddChunks)
{
    channel.samples.clear();
    if (region == nullptr || regionSize < SdlAudio::AdpcmBlockBytes)
    {
        return;
    }

    u32 blockSize = channel.blockSize;
    bool interleaved = channel.parent >= 0 || channel.child >= 0;
    u32 chunk = interleaved && blockSize >= SdlAudio::AdpcmBlockBytes ? blockSize : regionSize;
    u32 chunkCount = regionSize / chunk;
    u32 included = interleaved ? (chunkCount + 1) / 2 : chunkCount;
    // The region can open with the track's own header (the "MSVp" the bank's
    // tools write): it sits in the first chunk and is not samples, so the
    // blocks after it are the track's
    u32 header = SoundHeaderSkip(region, regionSize);
    channel.samples.reserve(static_cast<size_t>(included) * (chunk / SdlAudio::AdpcmBlockBytes) *
                            SdlAudio::AdpcmBlockSamples);
    s32 old = 0;
    s32 older = 0;
    s16 decoded[SdlAudio::AdpcmBlockSamples];
    for (u32 index = 0; index < chunkCount; index++)
    {
        if (interleaved && ((index & 1u) != (oddChunks ? 1u : 0u)))
        {
            continue;
        }

        u32 base = index * chunk;
        u32 blocks = chunk / SdlAudio::AdpcmBlockBytes;
        if (index == 0 && header != 0 && blocks > header / SdlAudio::AdpcmBlockBytes)
        {
            base += header;
            blocks -= header / SdlAudio::AdpcmBlockBytes;
        }
        for (u32 block = 0; block < blocks; block++)
        {
            const u8* at = region + base + block * SdlAudio::AdpcmBlockBytes;
            if (base + (block + 1) * SdlAudio::AdpcmBlockBytes > regionSize)
            {
                break;
            }

            SdlAudio::DecodeAdpcmBlock(at, decoded, old, older);
            channel.samples.insert(channel.samples.end(), decoded, decoded + SdlAudio::AdpcmBlockSamples);
        }
    }
}

void Platform::Audio::ReadMusic(s32 file, u32 offset, u32 size)
{
    SdlAudio::LockMixer();
    g_PendingMusic.file = file;
    g_PendingMusic.offset = offset;
    g_PendingMusic.size = size;
    SdlAudio::UnlockMixer();
}

s32 Platform::Audio::StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once)
{
    if (channel < 0 || channel >= 48)
    {
        return -1;
    }

    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= Platform::Audio::Voices)
    {
        return -1;
    }

    SdlAudio::LockMixer();
    SdlAudio::MusicChannel& music = g_MusicChannels[channel];
    s32 parent = music.parent;
    u32 blockSize = music.blockSize;
    // A stream the game starts means the music is the game's again: whatever the pause held is dropped for the new track
    g_HoldMusic = false;
    music = SdlAudio::MusicChannel();
    music.parent = parent;
    music.blockSize = blockSize;
    music.bound = true;
    music.voice = voice;
    music.group = voiceAndGroup & 0xFFFF0000;
    music.looping = !once;
    music.playing = false;
    music.file = file;
    music.offset = g_PendingMusic.offset;
    music.size = g_PendingMusic.size;

    // The voice is the stream's now: its volume, pitch and group are the
    // game's (SetMusicVolume and SetMusicPitch write the same register)
    SdlAudio::Voice& talking = g_Voices[voice];
    talking.source = SdlAudio::VoiceSource::Stream;
    talking.stream = channel;
    talking.pitch = pitch;
    talking.group = static_cast<s32>(music.group);
    s32 index = GroupIndex(talking.group);
    s32 groupLeft = index >= 0 && index < 4 ? g_GroupVolumesLeft[index] : Platform::Audio::FullGroupVolume;
    s32 groupRight = index >= 0 && index < 4 ? g_GroupVolumesRight[index] : Platform::Audio::FullGroupVolume;
    talking.targetLeft = ScaleVolume(groupLeft, talking.rawLeft);
    talking.targetRight = ScaleVolume(groupRight, talking.rawRight);
    SdlAudio::UnlockMixer();
    return channel;
}

void Platform::Audio::InterleaveMusic(s32 channel, u32 blockSize)
{
    if (channel < 0 || channel >= 48)
    {
        return;
    }

    SdlAudio::LockMixer();
    g_MusicChannels[channel].blockSize = blockSize;
    SdlAudio::UnlockMixer();
}

void Platform::Audio::AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    (void)soundAddress;
    if (child < 0 || child >= 48 || parent < 0 || parent >= 48)
    {
        return;
    }

    u32 voice = voiceAndGroup & 0xFFFF;
    if (voice >= Platform::Audio::Voices)
    {
        return;
    }

    SdlAudio::LockMixer();
    SdlAudio::MusicChannel& channel = g_MusicChannels[child];
    channel = SdlAudio::MusicChannel();
    channel.bound = true;
    channel.parent = parent;
    channel.voice = voice;
    channel.group = voiceAndGroup & 0xFFFF0000;
    channel.blockSize = g_MusicChannels[parent].blockSize;
    channel.file = g_MusicChannels[parent].file;
    channel.offset = g_MusicChannels[parent].offset;
    channel.size = g_MusicChannels[parent].size;
    channel.looping = g_MusicChannels[parent].looping;
    g_MusicChannels[parent].child = child;

    SdlAudio::Voice& talking = g_Voices[channel.voice];
    talking.source = SdlAudio::VoiceSource::Stream;
    talking.stream = child;
    talking.pitch = g_MusicChannels[parent].voice < Platform::Audio::Voices
                        ? g_Voices[g_MusicChannels[parent].voice].pitch
                        : 0x1000;
    talking.group = static_cast<s32>(channel.group);
    SdlAudio::UnlockMixer();
}

void Platform::Audio::SetMusicEnd(s32 channel, u32 end)
{
    if (channel < 0 || channel >= 48)
    {
        return;
    }

    // Retail's MIB end offset: where the track's data ends in its last block
    // (the I/O processor would otherwise read past it). The host decodes
    // exactly the region's size and follows the blocks' own loop flags, so
    // the value is kept only for completeness.
    SdlAudio::LockMixer();
    g_MusicChannels[channel].endOffset = end;
    SdlAudio::UnlockMixer();
}

void Platform::Audio::PrepareMusic(s32 channel)
{
    if (channel < 0 || channel >= 48)
    {
        return;
    }

    // The region is read and decoded OUTSIDE the mixer lock: the mixer's callback takes that same lock, so a
    // synchronous disc read under it stalls the audio thread for the whole read and the device underruns (the pop).
    // Retail reads on its own I/O processor channel, never on the mixer's path
    s32 file = -1;
    u32 offset = 0;
    u32 size = 0;
    s32 parent = -1;
    s32 child = -1;
    SdlAudio::MusicChannel config = {};
    SdlAudio::MusicChannel parentConfig = {};
    SdlAudio::MusicChannel childConfig = {};
    bool parentDecoded = false;
    SdlAudio::LockMixer();
    SdlAudio::MusicChannel& music = g_MusicChannels[channel];
    if (music.file < 0 || music.size == 0 || !music.samples.empty())
    {
        music.ready = true;
        SdlAudio::UnlockMixer();
        return;
    }

    file = music.file;
    offset = music.offset;
    size = music.size;
    parent = music.parent;
    child = music.child;
    // The decode buffers start as COPIES of the channels': DecodeMusicChannel reads blockSize/parent/child off them to
    // split an interleaved track's odd chunks from its even, so empty locals made it decode the whole region
    // non-interleaved for both halves (the music played doubled and out of step). Only samples are filled by the decode
    config = music;
    if (parent >= 0)
    {
        parentConfig = g_MusicChannels[parent];
        parentDecoded = !g_MusicChannels[parent].samples.empty();
    }

    if (child >= 0)
    {
        childConfig = g_MusicChannels[child];
    }

    SdlAudio::UnlockMixer();

    // The region read here (retail's I/O processor reads it on its own
    // channel, into the SPU's memory; the host decodes it whole instead)
    std::vector<u8> region(size);
    Platform::Stream::Read(0, file, offset, size, region.data());
    SdlAudio::MusicChannel decoded = config;
    SdlAudio::MusicChannel decodedParent = parentConfig;
    SdlAudio::MusicChannel decodedChild = childConfig;
    if (parent >= 0)
    {
        // The child's own blocks; the parent decodes its side itself (unless it already has), so
        // the loop points come out of the same region either way
        if (!parentDecoded)
        {
            DecodeMusicChannel(decodedParent, region.data(), size, false);
        }

        DecodeMusicChannel(decoded, region.data(), size, true);
    }
    else
    {
        DecodeMusicChannel(decoded, region.data(), size, false);
        if (child >= 0)
        {
            DecodeMusicChannel(decodedChild, region.data(), size, true);
        }
    }

    // Published under the lock: the callback sees a channel only once its samples are all there
    SdlAudio::LockMixer();
    music.samples = std::move(decoded.samples);
    music.endOffset = decoded.endOffset;
    if (parent >= 0)
    {
        if (!parentDecoded)
        {
            g_MusicChannels[parent].samples = std::move(decodedParent.samples);
            g_MusicChannels[parent].endOffset = decodedParent.endOffset;
        }
    }
    else if (child >= 0)
    {
        g_MusicChannels[child].samples = std::move(decodedChild.samples);
        g_MusicChannels[child].endOffset = decodedChild.endOffset;
    }

    music.ready = true;
    if (AudioLogOn())
    {
        std::fprintf(stderr, "audio: music channel %d file %d offset %u size %u samples %u looping %d\n", channel,
                     file, offset, size, static_cast<u32>(music.samples.size()), music.looping ? 1 : 0);
    }

    SdlAudio::UnlockMixer();
}

bool Platform::Audio::IsMusicReady(s32 channel)
{
    if (channel < 0 || channel >= 48)
    {
        return false;
    }

    SdlAudio::LockMixer();
    bool ready = g_MusicChannels[channel].ready;
    SdlAudio::UnlockMixer();
    return ready;
}

s32 Platform::Audio::PlayMusic(s32 channel)
{
    if (channel < 0 || channel >= 48)
    {
        return -1;
    }

    SdlAudio::LockMixer();
    EnsureAdsrRates();
    SdlAudio::MusicChannel& music = g_MusicChannels[channel];
    music.playing = true;
    s32 child = music.child;
    if (child >= 0 && child < 48)
    {
        // An interleaved track's other side plays with its parent (retail's
        // MsAddStreamChannel starts the child when the parent is allowed)
        g_MusicChannels[child].playing = true;
    }

    s32 ownVoice = static_cast<s32>(music.voice);
    s32 group = static_cast<s32>(music.group);
    if (ownVoice < Platform::Audio::Voices)
    {
        KeyOnStream(g_Voices[ownVoice], channel, group, g_Voices[ownVoice].pitch);
    }

    s32 childVoice = child >= 0 && child < 48 ? static_cast<s32>(g_MusicChannels[child].voice) : -1;
    if (childVoice >= 0 && childVoice < Platform::Audio::Voices)
    {
        KeyOnStream(g_Voices[childVoice], child, static_cast<s32>(g_MusicChannels[child].group),
                    g_Voices[childVoice].pitch);
    }

    SdlAudio::UnlockMixer();
    return 0;
}

bool Platform::Audio::IsMusicPlaying(s32 channel)
{
    if (channel < 0 || channel >= 48)
    {
        return false;
    }

    SdlAudio::LockMixer();
    bool playing = g_MusicChannels[channel].playing;
    SdlAudio::UnlockMixer();
    return playing;
}

void Platform::Audio::StopMusic(s32 channel)
{
    if (channel < 0 || channel >= 48)
    {
        return;
    }

    SdlAudio::LockMixer();
    SdlAudio::MusicChannel& music = g_MusicChannels[channel];
    music.playing = false;
    s32 child = music.child;
    if (child >= 0 && child < 48)
    {
        g_MusicChannels[child].playing = false;
    }

    // The voice is keyed off, not cut: its release rate is the module's
    // (SD_VP_ADSR2, 0xD, about 170 ms)
    if (music.voice < Platform::Audio::Voices)
    {
        SdlAudio::Voice& voice = g_Voices[music.voice];
        if (voice.source == SdlAudio::VoiceSource::Stream && voice.stream == channel)
        {
            voice.releasing = true;
            voice.envelope.releaseRate = 0xD;
        }
    }

    if (child >= 0 && child < 48 && g_MusicChannels[child].voice < Platform::Audio::Voices)
    {
        SdlAudio::Voice& voice = g_Voices[g_MusicChannels[child].voice];
        if (voice.source == SdlAudio::VoiceSource::Stream && voice.stream == child)
        {
            voice.releasing = true;
            voice.envelope.releaseRate = 0xD;
        }
    }

    SdlAudio::UnlockMixer();
}
