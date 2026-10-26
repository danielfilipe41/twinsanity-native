// The PlayStation ADPCM decoder: a 16 byte block is 28 4 bit samples,
// its first two bytes the shift and filter and the loop flags.
#include "adpcm.h"

namespace
{
// The filters' coefficients in 64ths (the SPU2's own table)
constexpr s32 FilterCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
}

u8 SdlAudio::DecodeAdpcmBlock(const u8* block, s16* out, s32& old, s32& older)
{
    s32 shift = block[0] & 0xF;
    if (shift > 12)
    {
        shift = 12;
    }

    u32 filter = (block[0] >> 4) & 0x7;
    s32 first = 0;
    s32 second = 0;
    if (filter < 5)
    {
        first = FilterCoefficients[filter][0];
        second = FilterCoefficients[filter][1];
    }

    for (u32 index = 0; index < AdpcmBlockSamples; index++)
    {
        u8 packed = block[AdpcmHeaderBytes + index / 2];
        s32 nibble = index % 2 == 0 ? packed & 0xF : packed >> 4;
        s32 sample = (((nibble ^ 8) - 8) << 12) >> shift;
        sample += (old * first + older * second + 32) / 64;
        if (sample > 32767)
        {
            sample = 32767;
        }
        else if (sample < -32768)
        {
            sample = -32768;
        }

        older = old;
        old = sample;
        out[index] = static_cast<s16>(sample);
    }

    return block[1];
}
