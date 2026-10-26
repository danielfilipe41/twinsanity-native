// Motion-prediction scalars: the 16 hand-written MMI prediction routines of src/platform/ps2/movie/mpegmotion.cpp
// (called through jtbl_002E8238) as portable C++, plus StorePixels' and AddPrediction's clamp-and-pack.
//
// Semantics match retail: half pixels average (a + b + 1) / 2, quarter pixels (a + b + c + d + 2) / 4, and stores
// clamp to 0-255. The column-major macroblock layout is gone: the host uses linear Y/Cb/Cr planes, so a block is a
// base pointer and a stride. The luma/chroma width split folds into the block width, and all 16 entries share one
// core.
#pragma once

#include "common.h"

namespace HostMovie
{
// A prediction block over a linear reference plane: width by height pixels
// at (x, y), read with stride between rows. average set averages into out
// ((p + q + 1) / 2, the dual-prime and interpolative second reference);
// halfX/halfY shift the source by half a pixel across/down.
struct PredictionBlock
{
    const u8* plane;
    u32 stride;
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    s16* out;
    u32 outStride;
    u32 average;
    u32 halfX;
    u32 halfY;
};

// A prediction routine, by average << 2 | half across << 1 | half down (the
// table's index); the chroma's 8 follow the luma's 8 and behave the same,
// their blocks narrower.
using PredictionRoutine = void (*)(const PredictionBlock* block);
extern const PredictionRoutine g_PredictionRoutines[16];

// Where a finished macroblock's pixels go: the luma's 16 by 16 pixels, then
// its chroma's Cb and Cr of 8 by 8 each, in linear planes.
struct MacroblockDestination
{
    u8* y;
    u8* cb;
    u8* cr;
    u32 stride;
    u32 chromaStride;
};

// Six times 64 pixels of prediction plus residual (16 bit each), clamped to
// 0-255 and packed (retail AddPrediction's pminh/pmaxh/ppacb)
void AddPrediction(const MacroblockDestination* destination, const s16* prediction, const s16* residual);
// The same without the residual (retail StorePixels)
void StorePixels(const MacroblockDestination* destination, const s16* values);
}
