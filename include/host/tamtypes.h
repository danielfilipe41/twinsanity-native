// Host stand-in for PS2SDK's tamtypes.h: the exact-width integer types the game's
// headers use. The PS2's are the same widths, so layouts don't change.
#pragma once

#include <stdint.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;
