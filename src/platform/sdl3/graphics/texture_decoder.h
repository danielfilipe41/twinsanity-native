// PS2 texture decoder: turns a retail texture transfer's packet (after the GS header and 0x20 leftover bytes;
// src/platform/ps2/renderer/resources.cpp TextureRead) into RGBA8.
//
// The file holds a total size, a Texture header (power-of-two width/height, levels, pixel format, upload format,
// has alpha, level offsets and widths, palette offset), 0x20 unused bytes, then a RET DMA tag around a VIF1 DIRECT
// of TRXPOS, TRXREG and TRXDIR, followed by the image GIF tag and bytes.
//
// Every retail texture is uploaded as PSMCT32 with a 64-pixel buffer width (textures.cpp WriteTextureUpload), then
// read back through its own indexed swizzle, so the decode replays both steps. That matches TT-Lab's EzSwizzle,
// which wrote the retail textures. Retail data only uses PSMT8 (0x13) and PSMCT32 (0x00); other formats are refused.
#pragma once

#include "common.h"
#include <cstdint>
#include <vector>

// A retail texture's GS header (src/platform/ps2/renderer/renderer.h Texture, without the runtime transfer/slot/
// secondContext tail)
struct Ps2TextureHeader
{
    u16 widthPower;   // log2 width (TEX0 TW)
    u16 heightPower;  // log2 height (TEX0 TH)
    u8 levels;        // mip count, 1 = none
    u8 pixelFormat;   // GS PSM (0x00 PSMCT32, 0x13 PSMT8)
    u8 uploadFormat;  // BITBLTBUF source format (0 in retail data: the bytes go in as PSMCT32 words)
    u8 hasAlpha;      // TEX0 TCC
    u8 function;      // TEX0 TFX
    u32 levelOffsets[7];  // mip byte offsets / 256 (MIPTBP TBP units)
    u32 levelWidths[7];   // mip buffer widths / 64 (MIPTBP TBW units; [0] is the image's TBW)
    u32 paletteOffset;    // CLUT byte offset / 256 (TEX0 CBP units)
};

// One level of a decoded texture: RGBA8 rows of width pixels, height rows
struct Ps2DecodedLevel
{
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

// A decoded texture: the 32 bit image's first level, and the mip levels the file itself declares after it (the
// header's levels and levelOffsets: each level's data sits at its own 64-word offset inside the same transfer)
struct Ps2DecodedTexture
{
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
    std::vector<Ps2DecodedLevel> mips;
};

// Decodes a transfer's packet (from its first DMA tag) into RGBA8 at the header's declared size. Returns false when
// the header or the packet's shape is bogus, or the format is neither PSMCT32 nor PSMT8. PSMCT32 words copy through
// (channels doubled); PSMT8 indices and their CLUT are read back out of the packet's image as 32-bit memory.
bool DecodePs2Texture(const Ps2TextureHeader* header, const u8* packet, size_t packetSize, Ps2DecodedTexture* out);
