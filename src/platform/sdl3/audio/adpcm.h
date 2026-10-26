// The PlayStation ADPCM decoder: a 16 byte block holds 28 4 bit samples,
// its first two bytes the shift and filter and the loop flags.
#pragma once

#include "common.h"

namespace SdlAudio
{
constexpr u32 AdpcmBlockBytes = 16;
constexpr u32 AdpcmBlockSamples = 28;
constexpr u32 AdpcmHeaderBytes = 2;

// The loop flags in a block's second byte, the SPU2's own
constexpr u8 AdpcmLoopEnd = 0x1;
constexpr u8 AdpcmLoopStart = 0x4;

// Decodes one block after old (its last sample but one, older the one before
// it), which it brings up to date. Returns the block's loop flags
u8 DecodeAdpcmBlock(const u8* block, s16* out, s32& old, s32& older);
}
