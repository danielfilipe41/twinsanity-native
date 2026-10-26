// Retail symbols the host link needs that no host code implements yet. The renderer's translation units are not
// compiled on the host, but the retail data still refers to their functions by name. The movie decoder's routines
// are abort stubs until it is ported. The font's drawing (FUN_001acf30/1acf50/1ab090) is implemented here through
// the host 2D quad path, laid out as retail's WriteTextPackets does.
#include "common.h"

#include "game/font.h"
#include "game/graphicstables.h"
#include "game/renderer.h"
#include "platform/graphics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
[[noreturn]] void AbortStub(const char* name)
{
    std::fprintf(stderr, "null platform: %s not implemented\n", name);
    std::abort();
}
}

struct MpegPredictionBlock;
struct MpegSystem;

extern "C"
{
// The movie decoder's MPEG extension readers (src/platform/ps2/movie/mpeg.h: called through the table D_00307928): declared
// with their retail labels here, defined below without them (an asm label goes on the declaration, not the definition).
void ReadSequenceExtension(MpegSystem* sys) RETAIL(_sequenceExtension);
void ReadSequenceDisplayExtension(MpegSystem* sys) RETAIL(_sequenceDisplayExtension);
void ReadQuantMatrixExtension(MpegSystem* sys) RETAIL(_quantMatrixExtension);
void ReadCopyrightExtension(MpegSystem* sys) RETAIL(_copyrightExtension);
void ReadPictureDisplayExtension(MpegSystem* sys) RETAIL(_pictureDisplayExtension);

// GetButtonPressure's switch cases (asm/text/GetButtonPressure.s: one case per button, kept as jlabel branches into each
// other): the retail data's switch table (G_ButtonPressureSwitchTable) points at them, but the switch itself is C++ on the host
// (src/game/pads.cpp), so they are never reached. Addresses only, never read by the compiled game.
u8 UpArrowPressure = 0;
u8 DownArrowPressure = 0;
u8 LeftArrowPressure = 0;
u8 RightArrowPressure = 0;
u8 TriangleButtonPressure = 0;
u8 CrossButtonPressure = 0;
u8 SquareButtonPressure = 0;
u8 CircleButtonPressure = 0;
u8 L1ButtonPressure = 0;
u8 L2ButtonPressure = 0;
u8 R1ButtonPressure = 0;
u8 R2ButtonPressure = 0;

// The renderer's and the VU0 microcode sets' static constructors (src/platform/ps2/renderer/buckets.cpp's RendererStaticInit
// and vu0programs.cpp's ConstructVu0ProgramsModule: each runs its module's Init with (1, DefaultInitPriority)). The retail
// static-constructor table calls them at start-up, so they must not abort; on the null platform there is nothing to set up.
void FUN_001a6678()
{
}

void FUN_002b2230()
{
}
} // extern "C"

// The font's vtable methods (include/game/font.h: Begin, Draw, End). The host has no GS, so each text is drawn as it is
// given rather than batched into packets. Draw lays the text out as retail's WriteTextPackets does (game/font.cpp's
// FontScale and RenderText, then the alignment) and submits each glyph as a flipped 2D quad over its page texture.
// End needs nothing: glyphs are submitted with their texts.
namespace
{
// A glyph's page: its index in the font's pages, in the low bits of its first byte (0 or 1 the first page, 2 the second...)
u32 GlyphPageIndex(u8 mask)
{
    return mask < 2 ? 0u : static_cast<u32>(mask - 1);
}

// A game colour's byte made fit for the window: the game's red, green and blue are full at 192, its alpha at 128
u8 ScaleChannel(u8 channel, u32 full)
{
    u32 scaled = static_cast<u32>(channel) * 255 / full;
    return static_cast<u8>(scaled > 255 ? 255 : scaled);
}
}

void Font::Begin()
{
    // The PS2 batches the frame's texts into packets; the host draws each text as it is given
}

void Font::Draw(const TextItem* item, TextPackets* /*packets*/)
{
    if (item == nullptr || item->text == nullptr || glyphs == nullptr || glyphCount <= 0 || pageCount <= 0)
    {
        return;
    }

    Vector2 scale;
    FontScale(this, &scale);
    Vector2 size;
    size.x = item->scale.x * scale.x;
    size.y = item->scale.y * scale.y;
    TextLayout layout;
    std::memset(&layout, 0, sizeof(layout));
    RenderText(this, item->text, &size, &layout);

    // The layout's units are sixteenths of the renderer's pixels (the PS2's text.cpp works in the GS's coordinates, whose
    // subpixel is 1/16 of a pixel over a frame the renderer's size)
    const f32 rw = static_cast<f32>(g_RendererWidth != 0 ? g_RendererWidth : 512) * 16.0f;
    const f32 rh = static_cast<f32>(g_RendererHeight != 0 ? g_RendererHeight : 512) * 16.0f;

    // The text's place (fractions of the screen) and the alignment about it
    Vector2 cursor = item->position;
    const TextAlignment alignment = item->alignment;
    if (alignment.top == 0)
    {
        cursor.y -= alignment.bottom != 0 ? layout.height / rh : layout.height / rh * 0.5f;
    }

    const u8 red = ScaleChannel(static_cast<u8>(item->colour & 0xFF), 192);
    const u8 green = ScaleChannel(static_cast<u8>((item->colour >> 8) & 0xFF), 192);
    const u8 blue = ScaleChannel(static_cast<u8>((item->colour >> 16) & 0xFF), 192);
    const u8 alpha = ScaleChannel(static_cast<u8>((item->colour >> 24) & 0xFF), 128);
    // The colour the glyph quads are submitted with carries the channels the way MultiplyTextured reads them back (128 is
    // the identity, so the scaled bytes return to the renderer's own convention of full at 192/128)
    const u32 glyphColour = static_cast<u32>(red >> 1) | static_cast<u32>(green >> 1) << 8 |
                            static_cast<u32>(blue >> 1) << 16 | static_cast<u32>(alpha >> 1) << 24;

    s32 first = 0;
    for (s32 line = 0; line < layout.lineCount; line++)
    {
        const f32 extentX = layout.lines[line].x / rw;
        const f32 extentY = layout.lines[line].y / rh;
        f32 start = cursor.x;
        if (alignment.left == 0)
        {
            start -= alignment.right != 0 ? extentX : extentX * 0.5f;
        }

        f32 pen = start;
        for (s32 at = 0; at < layout.counts[line]; at++)
        {
            Vector4 glyphValue;
            std::memcpy(&glyphValue, &glyphs[layout.glyphs[first + at]], sizeof(glyphValue));
            const f32 advance = glyphValue.z * size.x / rw;
            u8 mask = 0;
            std::memcpy(&mask, &glyphValue.x, sizeof(mask));
            mask &= 3u;
            u32 page = GlyphPageIndex(mask);
            if (page >= static_cast<u32>(pageCount))
            {
                page = 0;
            }

            MaterialResource* resource = materials[page];
            if (resource == nullptr || resource->material == nullptr)
            {
                pen += advance;
                continue;
            }

            // The glyph's area of its page: its left texel and bottom row (the page bits cleared), and its size in texels
            u32 bits = 0;
            std::memcpy(&bits, &glyphValue.x, sizeof(bits));
            bits &= ~3u;
            f32 texelLeft = 0.0f;
            std::memcpy(&texelLeft, &bits, sizeof(texelLeft));
            texelLeft *= 0.0625f;
            const f32 texelBottom = glyphValue.y * 0.0625f;
            const f32 texelWidth = glyphValue.z * 0.0625f;
            const f32 texelHeight = glyphValue.w * 0.0625f;

            const f32 height = glyphValue.w * size.y / rh;
            const Platform::Graphics::Rectangle place = {pen, cursor.y, advance, height};
            // The area is given in the page's texels; the flipped draw takes its rows the other way up
            const Platform::Graphics::Rectangle area = {texelLeft, texelBottom - texelHeight, texelWidth, texelHeight};
            if (advance > 0.0f && height > 0.0f)
            {
                Platform::Graphics::DrawGlyph(resource->material, glyphColour, place, area);
            }

            pen += advance;
        }

        first += layout.counts[line];
        cursor.y += extentY;
    }
}

void Font::End(TextPackets* /*packets*/)
{
    // The PS2 sends its packets for the other pages here; the host drew each text's glyphs directly
}

extern "C"
{
// The collision and physics solvers' VU0 halves (src/platform/ps2/collisionmaths.cpp) live in
// src/platform/null/vumath.cpp.
//
// Movie decoder stubs: libmpeg's hand-written MMI prediction routines (src/platform/ps2/movie/mpegmotion.cpp, called
// through jtbl_002E8238) and the scalable-extension readers (mpeg.h, called through D_00307928). These keep the
// tables linked until the decoder is ported. Only motion compensation and extension parsing reach them, and neither
// is ported yet.
void func_002BF748(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BF748");
}

void func_002BF858(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BF858");
}

void func_002BF9E0(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BF9E0");
}

void func_002BFB48(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFB48");
}

void func_002BFD40(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFD40");
}

void func_002BFE90(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFE90");
}

void func_002C0058(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002C0058");
}

void func_002C0200(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002C0200");
}

void func_002BF7C0(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BF7C0");
}

void func_002BF910(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BF910");
}

void func_002BFA90(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFA90");
}

void func_002BFC40(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFC40");
}

void func_002BFDE0(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFDE0");
}

void func_002BFF70(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002BFF70");
}

void func_002C0130(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002C0130");
}

void func_002C0320(const MpegPredictionBlock* block)
{
    (void)block;
    AbortStub("func_002C0320");
}

void func_002C1050(MpegSystem* sys)
{
    (void)sys;
    AbortStub("func_002C1050");
}

void func_002C1258(MpegSystem* sys)
{
    (void)sys;
    AbortStub("func_002C1258");
}

void ReadSequenceExtension(MpegSystem* sys)
{
    (void)sys;
    AbortStub("_sequenceExtension");
}

void ReadSequenceDisplayExtension(MpegSystem* sys)
{
    (void)sys;
    AbortStub("_sequenceDisplayExtension");
}

void ReadQuantMatrixExtension(MpegSystem* sys)
{
    (void)sys;
    AbortStub("_quantMatrixExtension");
}

void ReadCopyrightExtension(MpegSystem* sys)
{
    (void)sys;
    AbortStub("_copyrightExtension");
}

void ReadPictureDisplayExtension(MpegSystem* sys)
{
    (void)sys;
    AbortStub("_pictureDisplayExtension");
}

// spimdisasm's mislabeled constants (0x1000 and 0x1C0, used as plain words in the MemoryAllocSizes table). Fallbacks
// for references the converter does not fold; tools/host_data.py normally emits the values directly.
u32 D_1000 = 0x1000;
u32 D_1C0 = 0x1C0;
}
