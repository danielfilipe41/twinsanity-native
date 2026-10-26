// The SDL3 platform's motion-prediction scalars (see predict.h).
#include "predict.h"

namespace HostMovie
{
namespace
{
u8 ClampPixel(s32 value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 0xFF)
    {
        return 0xFF;
    }
    return static_cast<u8>(value);
}

void Predict(const PredictionBlock* block)
{
    for (u32 row = 0; row < block->height; row++)
    {
        const u8* top = block->plane + (block->y + row) * block->stride + block->x;
        const u8* bottom = top + (block->halfY != 0 ? block->stride : 0);
        s16* out = block->out + row * block->outStride;
        for (u32 column = 0; column < block->width; column++)
        {
            u32 across = column + (block->halfX != 0 ? 1 : 0);
            s32 value;
            if (block->halfX != 0 && block->halfY != 0)
            {
                value = (top[column] + top[across] + bottom[column] + bottom[across] + 2) >> 2;
            }
            else if (block->halfX != 0)
            {
                value = (top[column] + top[across] + 1) >> 1;
            }
            else if (block->halfY != 0)
            {
                value = (top[column] + bottom[column] + 1) >> 1;
            }
            else
            {
                value = top[column];
            }
            if (block->average != 0)
            {
                value = (out[column] + value + 1) >> 1;
            }
            out[column] = static_cast<s16>(value);
        }
    }
}

void PredictAverage(const PredictionBlock* block)
{
    PredictionBlock plain = *block;
    plain.average = 1;
    Predict(&plain);
}

void PredictHalfDown(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfY = 1;
    Predict(&shifted);
}

void PredictHalfDownAverage(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfY = 1;
    shifted.average = 1;
    Predict(&shifted);
}

void PredictHalfAcross(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfX = 1;
    Predict(&shifted);
}

void PredictHalfAcrossAverage(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfX = 1;
    shifted.average = 1;
    Predict(&shifted);
}

void PredictQuarter(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfX = 1;
    shifted.halfY = 1;
    Predict(&shifted);
}

void PredictQuarterAverage(const PredictionBlock* block)
{
    PredictionBlock shifted = *block;
    shifted.halfX = 1;
    shifted.halfY = 1;
    shifted.average = 1;
    Predict(&shifted);
}
}

// Indexed average << 2 | half across << 1 | half down (predict.h's packing, the call sites' and retail
// mpegmotion.cpp:363's g_MpegPredictionRoutines[average << 2 | halfAcross << 1 | halfDown]); the chroma's 8
// repeat the luma's, their blocks narrower.
const PredictionRoutine g_PredictionRoutines[16] = {
    Predict,           PredictHalfDown,      PredictHalfAcross,      PredictQuarter,
    PredictAverage,    PredictHalfDownAverage, PredictHalfAcrossAverage, PredictQuarterAverage,
    Predict,           PredictHalfDown,      PredictHalfAcross,      PredictQuarter,
    PredictAverage,    PredictHalfDownAverage, PredictHalfAcrossAverage, PredictQuarterAverage,
};

void AddPrediction(const MacroblockDestination* destination, const s16* prediction, const s16* residual)
{
    const s16* luma = prediction;
    const s16* lumaResidual = residual;
    for (u32 row = 0; row < 16; row++)
    {
        u8* out = destination->y + row * destination->stride;
        for (u32 column = 0; column < 16; column++)
        {
            out[column] = ClampPixel(luma[column] + lumaResidual[column]);
        }
        luma += 16;
        lumaResidual += 16;
    }
    for (u32 plane = 0; plane < 2; plane++)
    {
        u8* base = plane == 0 ? destination->cb : destination->cr;
        for (u32 row = 0; row < 8; row++)
        {
            u8* out = base + row * destination->chromaStride;
            for (u32 column = 0; column < 8; column++)
            {
                out[column] = ClampPixel(prediction[256 + plane * 64 + row * 8 + column] +
                                         residual[256 + plane * 64 + row * 8 + column]);
            }
        }
    }
}

void StorePixels(const MacroblockDestination* destination, const s16* values)
{
    for (u32 row = 0; row < 16; row++)
    {
        u8* out = destination->y + row * destination->stride;
        for (u32 column = 0; column < 16; column++)
        {
            out[column] = ClampPixel(values[row * 16 + column]);
        }
    }
    for (u32 plane = 0; plane < 2; plane++)
    {
        u8* base = plane == 0 ? destination->cb : destination->cr;
        for (u32 row = 0; row < 8; row++)
        {
            u8* out = base + row * destination->chromaStride;
            for (u32 column = 0; column < 8; column++)
            {
                out[column] = ClampPixel(values[256 + plane * 64 + row * 8 + column]);
            }
        }
    }
}
}
