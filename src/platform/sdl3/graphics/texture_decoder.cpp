// The SDL3 platform's PS2 texture decoder (see texture_decoder.h).
//
// A port of TT-Lab's EzSwizzle (TwinTech's Libraries/EzSwizzle.cs and Common/Color.cs): the same tables and swizzle
// functions in the same order, over little endian bytes. The retail textures were made with that tool, so its own
// decode is the ground truth for what the file's bytes mean.
#include "texture_decoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
// The block a pixel's word or byte is in, and the word and byte within it (GS memory's layouts, TT-Lab's tables)
constexpr u32 Block32[32] = {
    0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
};
constexpr u32 ColumnWord32[16] = {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};
constexpr u32 Block8[32] = {
    0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
};
constexpr u32 ColumnWord8[2][64] = {
    {0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 8, 9, 12,
     13, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7},
    {8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 0, 1, 4,
     5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15},
};
constexpr u32 ColumnByte8[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3,
};

// The GS's pixel formats this decodes (the rest are refused)
constexpr u8 FormatPsmct32 = 0x00;
constexpr u8 FormatPsmt8 = 0x13;

u64 ReadU64(const u8* bytes)
{
    u64 value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// A pixel's word in a 32 bit image (64 word pages of 8 by 8 word blocks), width being the buffer's width in pages
s32 Map32(s32 x, s32 y, s32 width)
{
    s32 pageX = x / 64;
    s32 pageY = y / 32;
    s32 page = pageX + pageY * width;
    s32 px = x - pageX * 64;
    s32 py = y - pageY * 32;
    s32 blockX = px / 8;
    s32 blockY = py / 8;
    s32 block = static_cast<s32>(Block32[blockX + blockY * 8]);
    s32 bx = px - blockX * 8;
    s32 by = py - blockY * 8;
    s32 column = by / 2;
    s32 cx = bx;
    s32 cy = by - column * 2;
    s32 cw = static_cast<s32>(ColumnWord32[cx + cy * 8]);
    return page * 2048 + block * 64 + column * 16 + cw;
}

// A pixel's word in an 8 bit image (256 byte pages of 16 by 16 byte blocks) and the byte of that word it sits in
s32 Map8(s32 x, s32 y, s32 width, s32* byte)
{
    s32 pageX = x / 128;
    s32 pageY = y / 64;
    s32 page = pageX + pageY * width;
    s32 px = x - pageX * 128;
    s32 py = y - pageY * 64;
    s32 blockX = px / 16;
    s32 blockY = py / 16;
    s32 block = static_cast<s32>(Block8[blockX + blockY * 8]);
    s32 bx = px - blockX * 16;
    s32 by = py - blockY * 16;
    s32 column = by / 4;
    s32 cx = bx;
    s32 cy = by - column * 4;
    s32 cw = static_cast<s32>(ColumnWord8[column & 1][cx + cy * 16]);
    *byte = static_cast<s32>(ColumnByte8[cx + cy * 16]);
    return page * 2048 + block * 64 + column * 16 + cw;
}

// The packet's image as the 32 bit image it was transferred as: every source word goes where the 32 bit swizzle
// holds its pixel. The GS stores every retail texture this way (BITBLTBUF's DPSM is 0, PSMCT32), then reads it back
// through the indexed format's swizzle.
void WritePsmct32(u8* destination, s32 words, const u8* source, u32 sourceBytes, s32 rrw, s32 rrh)
{
    s32 src = 0;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 dst = Map32(x, y, 1);
            if (4 * dst + 3 < words * 4 && static_cast<u32>(src + 3) < sourceBytes)
            {
                std::memcpy(destination + 4 * dst, source + src, 4);
            }
            src += 4;
        }
    }
}

// The 8 bit image's indices read out of the 32 bit image (dbp its word offset, dbw its width in 64 pixel units)
void ReadPsmt8(u8* destination, s32 dbp, s32 dbw, s32 rrw, s32 rrh, const u8* source, s32 words)
{
    dbw >>= 1;
    s32 src = 0;
    s32 start = dbp * 64;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 byte;
            s32 dst = start + Map8(x, y, dbw, &byte);
            if (byte >= 0 && static_cast<u64>(4 * dst + byte) < static_cast<u64>(words) * 4)
            {
                destination[src] = source[4 * dst + byte];
            }
            src++;
        }
    }
}

// The 32 bit image's words read out of it, one a pixel: the palette's sixteen by sixteen colours come back red,
// green, blue and alpha in order the 32 bit swizzle puts them in
void ReadPsmct32(u8* destination, s32 dbp, s32 dbw, s32 rrw, s32 rrh, const u8* source, s32 words)
{
    s32 src = 0;
    s32 start = dbp * 64;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 dst = start + Map32(x, y, dbw);
            if (4 * dst + 3 < words * 4)
            {
                std::memcpy(destination + src, source + 4 * dst, 4);
            }
            else
            {
                std::memset(destination + src, 0, 4);
            }
            src += 4;
        }
    }
}

// A colour of the file's channels doubled (the tools write them halved, so this is the inverse of what wrote them)
void ScaleUp(u8* colour)
{
    colour[0] = static_cast<u8>(colour[0] << 1);
    colour[1] = static_cast<u8>(colour[1] << 1);
    colour[2] = static_cast<u8>(colour[2] << 1);
    colour[3] = static_cast<u8>(colour[3] << 1);
}
}

bool DecodePs2Texture(const Ps2TextureHeader* header, const u8* packet, size_t packetSize, Ps2DecodedTexture* out)
{
    if (header == nullptr || packet == nullptr || out == nullptr)
    {
        return false;
    }

    u8 format = header->pixelFormat;
    u32 width = 1u << header->widthPower;
    u32 height = 1u << header->heightPower;
    if (header->widthPower > 12 || header->heightPower > 12 || width == 0 || height == 0 || width > 4096 ||
        height > 4096)
    {
        return false;
    }

    // The DMA tag, then the GIF tag of the transfer's registers (its loops and registers a write: three pairs of
    // address and data, TRXPOS, TRXREG and TRXDIR, the middle one saying the image's size)
    if (packetSize < 32)
    {
        return false;
    }

    u64 headerTag = ReadU64(packet + 16);
    u32 headerLoops = static_cast<u32>(headerTag & 0x7FFF);
    u32 headerRegs = static_cast<u32>((headerTag >> 60) & 0xF);
    if (headerRegs == 0)
    {
        headerRegs = 16;
    }
    u32 headerBytes = headerLoops * headerRegs * 16;
    if (32u + headerBytes + 16u > packetSize || headerBytes < 32)
    {
        return false;
    }
    // TRXREG's value (the second pair) holds the image's width and height as the transfer sent it
    u64 sizeRegister = ReadU64(packet + 32 + 16);
    u32 rrw = static_cast<u32>(sizeRegister & 0xFFFFFFFF);
    u32 rrh = static_cast<u32>(sizeRegister >> 32);
    if (rrw == 0 || rrh == 0 || rrw > 4096 || rrh > 4096)
    {
        return false;
    }

    // The image's GIF tag, then its data
    const u8* imageTag = packet + 32 + headerBytes;
    u32 imageLoops = static_cast<u32>(ReadU64(imageTag) & 0x7FFF);
    const u8* gifData = imageTag + 16;
    u32 gifBytes = imageLoops * 16;
    if (32u + headerBytes + 16u + gifBytes > packetSize)
    {
        return false;
    }

    out->width = width;
    out->height = height;
    out->rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    // The transferred image's words are the pixels themselves: the palette is already the colours
    if (format == FormatPsmct32)
    {
        u32 pixels = width * height;
        u32 copy = gifBytes < pixels * 4 ? gifBytes : pixels * 4;
        std::memcpy(out->rgba.data(), gifData, copy);
        for (u32 pixel = 0; pixel < pixels; pixel++)
        {
            ScaleUp(out->rgba.data() + pixel * 4);
        }

        return true;
    }

    if (format != FormatPsmt8)
    {
        return false;
    }

    // The transferred image as the 32 bit image it was written as, then its indices and its palette read out of
    // that: the image bytes are laid into GS memory as PSMCT32 words (BITBLTBUF's width 64 pixels) and the texture
    // reads them back through the 8 bit swizzle, the palette included (its CLUT is the end of the same image)
    s32 words = static_cast<s32>(rrw * rrh);
    std::vector<u8> raw(static_cast<size_t>(words) * 4, 0);
    WritePsmct32(raw.data(), words, gifData, gifBytes, static_cast<s32>(rrw), static_cast<s32>(rrh));

    u32 tbw = header->levelWidths[0];
    u32 cbp = header->paletteOffset;

    std::vector<u8> indices(static_cast<size_t>(width) * height, 0);
    ReadPsmt8(indices.data(), 0, static_cast<s32>(tbw), static_cast<s32>(width), static_cast<s32>(height), raw.data(),
              words);

    u8 colours[256 * 4];
    ReadPsmct32(colours, static_cast<s32>(cbp), 1, 16, 16, raw.data(), words);
    // The GS reads a CLUT in CSM1 order: the transfer's word for entry i sits at the bits 3 and 4 swap of i (entry
    // index's bits 3 and 4 exchanged), which is the same swap TT-Lab's decoder does over the sixteen by sixteen
    for (s32 i = 0; i < 8; i++)
    {
        for (s32 j = 8; j < 16; j++)
        {
            s32 a = j + i * 32;
            s32 b = a + 8;
            for (s32 channel = 0; channel < 4; channel++)
            {
                u8 swap = colours[a * 4 + channel];
                colours[a * 4 + channel] = colours[b * 4 + channel];
                colours[b * 4 + channel] = swap;
            }
        }
    }
    for (s32 entry = 0; entry < 256; entry++)
    {
        ScaleUp(colours + entry * 4);
    }

    u32 pixels = width * height;
    for (u32 pixel = 0; pixel < pixels; pixel++)
    {
        std::memcpy(out->rgba.data() + static_cast<size_t>(pixel) * 4, colours + static_cast<u32>(indices[pixel]) * 4, 4);
    }

    // The mip levels the file declares, read out of the same transferred image: level k's data starts at its own
    // levelOffsets[k] (64-word units) and is as wide as its levelWidths[k] says, its image half the size of the one
    // above. The palette is the texture's own, so the indices go through the colours already read
    out->mips.clear();
    for (u32 level = 1; level < header->levels && level < 7; level++)
    {
        const u32 levelWidth = width >> level;
        const u32 levelHeight = height >> level;
        if (levelWidth == 0 || levelHeight == 0)
        {
            break;
        }
        Ps2DecodedLevel mip;
        mip.width = levelWidth;
        mip.height = levelHeight;
        mip.rgba.assign(static_cast<size_t>(levelWidth) * levelHeight * 4, 0);
        std::vector<u8> levelIndices(static_cast<size_t>(levelWidth) * levelHeight, 0);
        ReadPsmt8(levelIndices.data(), static_cast<s32>(header->levelOffsets[level]),
                  static_cast<s32>(header->levelWidths[level]), static_cast<s32>(levelWidth),
                  static_cast<s32>(levelHeight), raw.data(), words);
        for (u32 pixel = 0; pixel < levelWidth * levelHeight; pixel++)
        {
            std::memcpy(mip.rgba.data() + static_cast<size_t>(pixel) * 4,
                        colours + static_cast<u32>(levelIndices[pixel]) * 4, 4);
        }
        out->mips.push_back(std::move(mip));
    }

    return true;
}
