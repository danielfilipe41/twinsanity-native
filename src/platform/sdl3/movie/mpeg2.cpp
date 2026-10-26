// Software MPEG-2 video core (see mpeg2.h): a portable
// decoder for the retail movies' format (MPEG-2 4:2:0 frame pictures,
// I/P/B), replacing the IPU's VDEC/BDEC/CSC. Tables are MPEG-2 spec
// section 7 (B.12-B.15); the IDCT is the separable reference form;
// motion uses predict.h's scalars over linear planes.
#include "mpeg2.h"
#include "predict.h"

#include <cmath>
#include <cstring>

namespace HostMovie
{
namespace
{
// Start-code ids (MPEG-2 6.1/6.2/6.3)
constexpr u32 SequenceHeaderCode = 0xB3;
constexpr u32 SequenceExtensionCode = 0xB5;
constexpr u32 ExtensionStartCode = 0xB5;
constexpr u32 SequenceEndCode = 0xB7;
constexpr u32 GroupStartCode = 0xB8;
constexpr u32 SliceStartCodeMin = 0x01;
constexpr u32 SliceStartCodeMax = 0xAF;

constexpr u32 PictureStartCode = 0x00;
constexpr u32 PictureCodingExtensionId = 8;

constexpr u8 ZigZag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
constexpr u8 AlternateScan[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};
constexpr u8 DefaultIntra[64] = {
    8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

// A VLC entry: code bits of length, decoded value (the MSSG flash-table
// shape: the cascade picks the table by lookahead width, the entry by the
// top bits. Length 0 never matches.)
struct VlcEntry
{
    s16 value;
    u8 length;
};

// A DCT coefficient entry: run of zeros then the level, over length bits
// (DCTtab of the MSSG reference). run 64 ends the block, 65 escapes to
// fixed-length run/level.
struct DctEntry
{
    s16 run;
    s16 level;
    u8 length;
};

// Macroblock types (B.12 Tables B-2/B-3/B-4), as (value, length) pairs
// indexed by the lookahead cascade the MSSG reference uses (getvlc.c's
// Get_P/B_macroblock_type). Flags: 0x10 quant, 0x08 forward, 0x04
// backward, 0x02 pattern, 0x01 intra. Length 0 never matches.
constexpr VlcEntry PMBTable0[] = {
    {0, 0}, {8, 3}, {2, 2}, {2, 2}, {10, 1}, {10, 1}, {10, 1}, {10, 1},
};
constexpr VlcEntry PMBTable1[] = {
    {0, 0}, {17, 6}, {18, 5}, {18, 5}, {26, 5}, {26, 5}, {1, 5}, {1, 5},
};
constexpr VlcEntry BMBTable0[] = {
    {0, 0}, {0, 0}, {8, 4}, {10, 4}, {4, 3}, {4, 3}, {6, 3}, {6, 3},
    {12, 2}, {12, 2}, {12, 2}, {12, 2}, {14, 2}, {14, 2}, {14, 2}, {14, 2},
};
constexpr VlcEntry BMBTable1[] = {
    {0, 0}, {17, 6}, {22, 6}, {26, 6}, {30, 5}, {30, 5}, {1, 5}, {1, 5},
};

// Motion codes (B.12 Table B-10) in the MSSG flash shape (MVtab0/1/2):
// code magnitude over length bits, sign bit follows separately.
constexpr VlcEntry MVTable0[] = {
    {0, 0}, {3, 3}, {2, 2}, {2, 2}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
};
constexpr VlcEntry MVTable1[] = {
    {0, 0}, {0, 0}, {0, 0}, {7, 6}, {6, 6}, {5, 6}, {4, 5}, {4, 5},
};
constexpr VlcEntry MVTable2[] = {
    {16, 9}, {15, 9}, {14, 9}, {13, 9}, {12, 9}, {11, 9}, {10, 8}, {10, 8},
    {9, 8}, {9, 8}, {8, 8}, {8, 8},
};

// Coded block pattern (B.12 Table B-9) in the MSSG flash shape
constexpr VlcEntry CBPTable0[] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {62, 5}, {2, 5}, {61, 5}, {1, 5}, {56, 5}, {52, 5}, {44, 5}, {28, 5},
    {40, 5}, {20, 5}, {48, 5}, {12, 5}, {32, 4}, {32, 4}, {16, 4}, {16, 4},
    {8, 4}, {8, 4}, {4, 4}, {4, 4}, {60, 3}, {60, 3}, {60, 3}, {60, 3},
};
constexpr VlcEntry CBPTable1[] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {58, 8}, {54, 8}, {46, 8}, {30, 8},
    {57, 8}, {53, 8}, {45, 8}, {29, 8}, {38, 8}, {26, 8}, {37, 8}, {25, 8},
    {43, 8}, {23, 8}, {51, 8}, {15, 8}, {42, 8}, {22, 8}, {50, 8}, {14, 8},
    {41, 8}, {21, 8}, {49, 8}, {13, 8}, {35, 8}, {19, 8}, {11, 8}, {7, 8},
    {34, 7}, {34, 7}, {18, 7}, {18, 7}, {10, 7}, {10, 7}, {6, 7}, {6, 7},
    {33, 7}, {33, 7}, {17, 7}, {17, 7}, {9, 7}, {9, 7}, {5, 7}, {5, 7},
    {63, 6}, {63, 6}, {63, 6}, {63, 6}, {3, 6}, {3, 6}, {3, 6}, {3, 6},
    {36, 6}, {36, 6}, {36, 6}, {36, 6}, {24, 6}, {24, 6}, {24, 6}, {24, 6},
};
constexpr VlcEntry CBPTable2[] = {
    {0, 0}, {0, 9}, {39, 9}, {27, 9}, {59, 9}, {55, 9}, {47, 9}, {31, 9},
};

// Macroblock address increment (B.12 Table B-1) in the MSSG flash shape,
// plus the two fixed codes: stuffing (11 zero bits then 15) and escape
// (11 zero bits then 8, adding 33 to the next increment).
constexpr VlcEntry MBATable1[] = {
    {0, 0}, {0, 0}, {7, 5}, {6, 5}, {5, 4}, {5, 4}, {4, 4}, {4, 4},
    {3, 3}, {3, 3}, {3, 3}, {3, 3}, {2, 3}, {2, 3}, {2, 3}, {2, 3},
};
constexpr VlcEntry MBATable2[] = {
    {33, 11}, {32, 11}, {31, 11}, {30, 11}, {29, 11}, {28, 11}, {27, 11}, {26, 11},
    {25, 11}, {24, 11}, {23, 11}, {22, 11}, {21, 10}, {21, 10}, {20, 10}, {20, 10},
    {19, 10}, {19, 10}, {18, 10}, {18, 10}, {17, 10}, {17, 10}, {16, 10}, {16, 10},
    {15, 8}, {15, 8}, {15, 8}, {15, 8}, {15, 8}, {15, 8}, {15, 8}, {15, 8},
    {14, 8}, {14, 8}, {14, 8}, {14, 8}, {14, 8}, {14, 8}, {14, 8}, {14, 8},
    {13, 8}, {13, 8}, {13, 8}, {13, 8}, {13, 8}, {13, 8}, {13, 8}, {13, 8},
    {12, 8}, {12, 8}, {12, 8}, {12, 8}, {12, 8}, {12, 8}, {12, 8}, {12, 8},
    {11, 8}, {11, 8}, {11, 8}, {11, 8}, {11, 8}, {11, 8}, {11, 8}, {11, 8},
    {10, 8}, {10, 8}, {10, 8}, {10, 8}, {10, 8}, {10, 8}, {10, 8}, {10, 8},
    {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7},
    {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7}, {9, 7},
    {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7},
    {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7}, {8, 7},
};

// DCT DC sizes (B.12 Tables B-12/B-13) in the MSSG flash shape
constexpr VlcEntry DCLumTable0[] = {
    {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2},
    {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {0, 3}, {0, 3}, {0, 3}, {0, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3},
    {4, 3}, {4, 3}, {4, 3}, {4, 3}, {5, 4}, {5, 4}, {6, 5}, {0, 0},
};
constexpr VlcEntry DCLumTable1[] = {
    {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6},
    {8, 7}, {8, 7}, {8, 7}, {8, 7}, {9, 8}, {9, 8}, {10, 9}, {11, 9},
};
constexpr VlcEntry DCChromTable0[] = {
    {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2},
    {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2},
    {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {3, 3}, {3, 3}, {3, 3}, {3, 3}, {4, 4}, {4, 4}, {5, 5}, {0, 0},
};
constexpr VlcEntry DCChromTable1[] = {
    {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
    {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
    {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
    {8, 8}, {8, 8}, {8, 8}, {8, 8}, {9, 9}, {9, 9}, {10, 10}, {11, 10},
};
// DCT coefficient tables (B.12 Tables B-14/B-15) in the MSSG flash shape:
// the cascade picks the table by 16-bit lookahead width, the entry by the
// top bits. run 64 ends the block, 65 escapes to fixed-length run/level.
constexpr DctEntry DctTablefirst[] = {
    {0, 2, 4}, {2, 1, 4}, {1, 1, 3}, {1, 1, 3}, {0, 1, 1}, {0, 1, 1},
    {0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {0, 1, 1},
};
constexpr DctEntry DctTablenext[] = {
    {0, 2, 4}, {2, 1, 4}, {1, 1, 3}, {1, 1, 3}, {64, 0, 2}, {64, 0, 2},
    {64, 0, 2}, {64, 0, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
};
constexpr DctEntry DctTable0[] = {
    {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {2, 2, 7}, {2, 2, 7},
    {9, 1, 7}, {9, 1, 7}, {0, 4, 7}, {0, 4, 7}, {8, 1, 7}, {8, 1, 7},
    {7, 1, 6}, {7, 1, 6}, {7, 1, 6}, {7, 1, 6}, {6, 1, 6}, {6, 1, 6},
    {6, 1, 6}, {6, 1, 6}, {1, 2, 6}, {1, 2, 6}, {1, 2, 6}, {1, 2, 6},
    {5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {13, 1, 8}, {0, 6, 8},
    {12, 1, 8}, {11, 1, 8}, {3, 2, 8}, {1, 3, 8}, {0, 5, 8}, {10, 1, 8},
    {0, 3, 5}, {0, 3, 5}, {0, 3, 5}, {0, 3, 5}, {0, 3, 5}, {0, 3, 5},
    {0, 3, 5}, {0, 3, 5}, {4, 1, 5}, {4, 1, 5}, {4, 1, 5}, {4, 1, 5},
    {4, 1, 5}, {4, 1, 5}, {4, 1, 5}, {4, 1, 5}, {3, 1, 5}, {3, 1, 5},
    {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5},
};
constexpr DctEntry DctTable0a[] = {
    {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {7, 1, 7}, {7, 1, 7},
    {8, 1, 7}, {8, 1, 7}, {6, 1, 7}, {6, 1, 7}, {2, 2, 7}, {2, 2, 7},
    {0, 7, 6}, {0, 7, 6}, {0, 7, 6}, {0, 7, 6}, {0, 6, 6}, {0, 6, 6},
    {0, 6, 6}, {0, 6, 6}, {4, 1, 6}, {4, 1, 6}, {4, 1, 6}, {4, 1, 6},
    {5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {1, 5, 8}, {11, 1, 8},
    {0, 11, 8}, {0, 10, 8}, {13, 1, 8}, {12, 1, 8}, {3, 2, 8}, {1, 4, 8},
    {2, 1, 5}, {2, 1, 5}, {2, 1, 5}, {2, 1, 5}, {2, 1, 5}, {2, 1, 5},
    {2, 1, 5}, {2, 1, 5}, {1, 2, 5}, {1, 2, 5}, {1, 2, 5}, {1, 2, 5},
    {1, 2, 5}, {1, 2, 5}, {1, 2, 5}, {1, 2, 5}, {3, 1, 5}, {3, 1, 5},
    {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5},
    {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
    {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
    {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
    {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
    {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
    {1, 1, 3}, {1, 1, 3}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
    {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
    {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
    {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
    {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
    {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
    {0, 1, 2}, {0, 1, 2}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
    {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
    {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
    {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
    {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
    {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 4, 5}, {0, 4, 5},
    {0, 4, 5}, {0, 4, 5}, {0, 4, 5}, {0, 4, 5}, {0, 4, 5}, {0, 4, 5},
    {0, 5, 5}, {0, 5, 5}, {0, 5, 5}, {0, 5, 5}, {0, 5, 5}, {0, 5, 5},
    {0, 5, 5}, {0, 5, 5}, {9, 1, 7}, {9, 1, 7}, {1, 3, 7}, {1, 3, 7},
    {10, 1, 7}, {10, 1, 7}, {0, 8, 7}, {0, 8, 7}, {0, 9, 7}, {0, 9, 7},
    {0, 12, 8}, {0, 13, 8}, {2, 3, 8}, {4, 2, 8}, {0, 14, 8}, {0, 15, 8},
};
constexpr DctEntry DctTable1[] = {
    {16, 1, 10}, {5, 2, 10}, {0, 7, 10}, {2, 3, 10}, {1, 4, 10}, {15, 1, 10},
    {14, 1, 10}, {4, 2, 10},
};
constexpr DctEntry DctTable1a[] = {
    {5, 2, 9}, {5, 2, 9}, {14, 1, 9}, {14, 1, 9}, {2, 4, 10}, {16, 1, 10},
    {15, 1, 9}, {15, 1, 9},
};
constexpr DctEntry DctTable2[] = {
    {0, 11, 12}, {8, 2, 12}, {4, 3, 12}, {0, 10, 12}, {2, 4, 12}, {7, 2, 12},
    {21, 1, 12}, {20, 1, 12}, {0, 9, 12}, {19, 1, 12}, {18, 1, 12}, {1, 5, 12},
    {3, 3, 12}, {0, 8, 12}, {6, 2, 12}, {17, 1, 12},
};
constexpr DctEntry DctTable3[] = {
    {10, 2, 13}, {9, 2, 13}, {5, 3, 13}, {3, 4, 13}, {2, 5, 13}, {1, 7, 13},
    {1, 6, 13}, {0, 15, 13}, {0, 14, 13}, {0, 13, 13}, {0, 12, 13}, {26, 1, 13},
    {25, 1, 13}, {24, 1, 13}, {23, 1, 13}, {22, 1, 13},
};
constexpr DctEntry DctTable4[] = {
    {0, 31, 14}, {0, 30, 14}, {0, 29, 14}, {0, 28, 14}, {0, 27, 14}, {0, 26, 14},
    {0, 25, 14}, {0, 24, 14}, {0, 23, 14}, {0, 22, 14}, {0, 21, 14}, {0, 20, 14},
    {0, 19, 14}, {0, 18, 14}, {0, 17, 14}, {0, 16, 14},
};
constexpr DctEntry DctTable5[] = {
    {0, 40, 15}, {0, 39, 15}, {0, 38, 15}, {0, 37, 15}, {0, 36, 15}, {0, 35, 15},
    {0, 34, 15}, {0, 33, 15}, {0, 32, 15}, {1, 14, 15}, {1, 13, 15}, {1, 12, 15},
    {1, 11, 15}, {1, 10, 15}, {1, 9, 15}, {1, 8, 15},
};
constexpr DctEntry DctTable6[] = {
    {1, 18, 16}, {1, 17, 16}, {1, 16, 16}, {1, 15, 16}, {6, 3, 16}, {16, 2, 16},
    {15, 2, 16}, {14, 2, 16}, {13, 2, 16}, {12, 2, 16}, {11, 2, 16}, {31, 1, 16},
    {30, 1, 16}, {29, 1, 16}, {28, 1, 16}, {27, 1, 16},
};

// The non-linear quantiser scale (7.4.2.2, the MSSG/libmpeg2 table)
constexpr u8 NonLinearScale[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112,
};

// Macroblock address increment (B.12 Table B-1): the MSSG cascade
// (Get_macroblock_address_increment). Stuffing adds nothing, escape 33.
// Returns the total increment, -1 on bad bits or a short buffer.
s32 DecodeIncrement(BitReader* reader)
{
    s32 total = 0;
    while (reader->ShowBits(11) < 24)
    {
        u32 code = reader->ShowBits(11);
        if (code == 15)
        {
            reader->SkipBits(11); // macroblock_stuffing
        }
        else if (code == 8)
        {
            reader->SkipBits(11); // macroblock_escape
            total += 33;
        }
        else
        {
            reader->error = true;
            return -1;
        }
        if (reader->error)
        {
            return -1;
        }
    }
    if (reader->error)
    {
        return -1;
    }
    u32 code = reader->ShowBits(11);
    if (code >= 1024)
    {
        reader->SkipBits(1);
        return total + 1;
    }
    if (code >= 128)
    {
        u32 index = code >> 6;
        reader->SkipBits(MBATable1[index].length);
        return total + MBATable1[index].value;
    }
    u32 index = code - 24;
    reader->SkipBits(MBATable2[index].length);
    return total + MBATable2[index].value;
}

// Macroblock types (B.12 Tables B-2/B-3/B-4): the MSSG cascade
// (Get_I/P/B_macroblock_type). Flags: 0x10 quant, 0x08 forward, 0x04
// backward, 0x02 pattern, 0x01 intra. Returns -1 on bad bits.
s32 DecodeMbType(BitReader* reader, u32 pictureType)
{
    if (pictureType == 1)
    {
        if (reader->GetBit() != 0)
        {
            return 1;
        }
        if (reader->error || reader->GetBit() == 0)
        {
            reader->error = true;
            return -1;
        }
        return 17;
    }
    if (pictureType == 2)
    {
        u32 code = reader->ShowBits(6);
        if (code >= 8)
        {
            const VlcEntry& entry = PMBTable0[code >> 3];
            reader->SkipBits(entry.length);
            return entry.value;
        }
        if (code == 0)
        {
            reader->error = true;
            return -1;
        }
        const VlcEntry& entry = PMBTable1[code];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    u32 code = reader->ShowBits(6);
    if (code >= 8)
    {
        const VlcEntry& entry = BMBTable0[code >> 2];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    if (code == 0)
    {
        reader->error = true;
        return -1;
    }
    const VlcEntry& entry = BMBTable1[code];
    reader->SkipBits(entry.length);
    return entry.value;
}

// Motion code (B.12 Table B-10): the MSSG cascade (Get_motion_code). The
// single 1 bit means zero; else the magnitude table plus a sign bit.
// Returns -1 on bad bits (0 and -0 both decode to 0, never -1).
s32 DecodeMotionCode(BitReader* reader)
{
    if (reader->GetBit() != 0)
    {
        return 0;
    }
    u32 code = reader->ShowBits(9);
    s32 magnitude = 0;
    if (code >= 64)
    {
        const VlcEntry& entry = MVTable0[code >> 6];
        reader->SkipBits(entry.length);
        magnitude = entry.value;
    }
    else if (code >= 24)
    {
        const VlcEntry& entry = MVTable1[(code >> 3)];
        reader->SkipBits(entry.length);
        magnitude = entry.value;
    }
    else if (code >= 12)
    {
        const VlcEntry& entry = MVTable2[code - 12];
        reader->SkipBits(entry.length);
        magnitude = entry.value;
    }
    else
    {
        reader->error = true;
        return -1;
    }
    if (reader->error)
    {
        return -1;
    }
    return reader->GetBit() != 0 ? -magnitude : magnitude;
}

// Differential motion vector for dual prime (7.6.5.2, Get_dmvector):
// 0, or 1/-1 by the sign bit.
s32 DecodeDmVector(BitReader* reader)
{
    if (reader->GetBit() == 0)
    {
        return 0;
    }
    return reader->GetBit() != 0 ? -1 : 1;
}

// Coded block pattern (B.12 Table B-9): the MSSG cascade
// (Get_coded_block_pattern). Returns -1 on bad bits.
s32 DecodeCbp(BitReader* reader)
{
    u32 code = reader->ShowBits(9);
    if (code >= 128)
    {
        const VlcEntry& entry = CBPTable0[code >> 4];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    if (code >= 8)
    {
        const VlcEntry& entry = CBPTable1[code >> 1];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    if (code < 1)
    {
        reader->error = true;
        return -1;
    }
    const VlcEntry& entry = CBPTable2[code];
    reader->SkipBits(entry.length);
    return entry.value;
}

// DCT DC size (B.12 Tables B-12/B-13): the MSSG cascade
// (Get_Luma/Chroma_DC_dct_diff without the differential). Returns -1 on
// bad bits.
s32 DecodeDcSize(BitReader* reader, bool chroma)
{
    u32 code = reader->ShowBits(5);
    if (!chroma)
    {
        if (code < 31)
        {
            const VlcEntry& entry = DCLumTable0[code];
            reader->SkipBits(entry.length);
            return entry.value;
        }
        u32 index = reader->ShowBits(9) - 0x1F0;
        const VlcEntry& entry = DCLumTable1[index];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    if (code < 31)
    {
        const VlcEntry& entry = DCChromTable0[code];
        reader->SkipBits(entry.length);
        return entry.value;
    }
    u32 index = reader->ShowBits(10) - 0x3E0;
    const VlcEntry& entry = DCChromTable1[index];
    reader->SkipBits(entry.length);
    return entry.value;
}

// The DC differential for a size (7.2.1): size bits, the top bit clear
// meaning a negative value offset by (1 << size) - 1.
s32 DecodeDcDiff(BitReader* reader, s32 size)
{
    if (size == 0)
    {
        return 0;
    }
    s32 diff = static_cast<s32>(reader->GetBits(static_cast<u32>(size)));
    if (reader->error)
    {
        return 0;
    }
    if ((diff & (1 << (size - 1))) == 0)
    {
        diff -= (1 << size) - 1;
    }
    return diff;
}

// DCT AC coefficients (B.12 Tables B-14/B-15): the MSSG cascade
// (Decode_MPEG2_Intra/Non_Intra_Block). run 64 ends the block, 65 escapes
// to a 6-bit run and a 12-bit level (non-intra meanings for the first
// coefficient, picked by first). Quantised levels land in scan order;
// dequantisation happens in the caller. Returns false on bad bits.
bool DecodeAcCoefficients(BitReader* reader, s16* block, bool intraVlcFormat, bool first)
{
    u32 index = first ? 0 : 1;
    while (true)
    {
        u32 code = reader->ShowBits(16);
        const DctEntry* tab = nullptr;
        if (code >= 16384 && !intraVlcFormat)
        {
            tab = (first ? DctTablefirst : DctTablenext) + ((code >> 12) - 4);
        }
        else if (code >= 1024)
        {
            tab = (intraVlcFormat ? DctTable0a : DctTable0) + ((code >> 8) - 4);
        }
        else if (code >= 512)
        {
            tab = (intraVlcFormat ? DctTable1a : DctTable1) + ((code >> 6) - 8);
        }
        else if (code >= 256)
        {
            tab = &DctTable2[(code >> 4) - 16];
        }
        else if (code >= 128)
        {
            tab = &DctTable3[(code >> 3) - 16];
        }
        else if (code >= 64)
        {
            tab = &DctTable4[(code >> 2) - 16];
        }
        else if (code >= 32)
        {
            tab = &DctTable5[(code >> 1) - 16];
        }
        else if (code >= 16)
        {
            tab = &DctTable6[code - 16];
        }
        else
        {
            reader->error = true;
            return false;
        }
        reader->SkipBits(tab->length);
        if (reader->error)
        {
            return false;
        }
        if (tab->run == 64) // End of block
        {
            return true;
        }
        s32 run = 0;
        s32 level = 0;
        s32 sign = 0;
        if (tab->run == 65) // Escape
        {
            run = static_cast<s32>(reader->GetBits(6));
            s32 value = static_cast<s32>(reader->GetBits(12));
            if (reader->error)
            {
                reader->error = true;
                return false;
            }
            // 7.2.2.1: the escape's 12-bit level is signed; 0 and -2048 are
            // legal values (libmpeg2/ffmpeg accept them, the block just
            // stores a zero), so only a read fault aborts.
            if (value >= 2048)
            {
                sign = 1;
                value = 4096 - value;
            }
            level = value;
        }
        else
        {
            run = tab->run;
            level = tab->level;
            sign = static_cast<s32>(reader->GetBit());
            if (reader->error)
            {
                return false;
            }
        }
        index += static_cast<u32>(run);
        if (index >= 64)
        {
            reader->error = true;
            return false;
        }
        block[index] = static_cast<s16>(sign != 0 ? -level : level);
        index++;
        first = false;
    }
}

// Separable reference IDCT (S.2.5.2 form): float, rounded at the end
void InverseDct(const s16* block, s16* pixels)
{
    static double cosTable[8][8];
    static bool made = false;
    if (!made)
    {
        for (u32 i = 0; i < 8; i++)
        {
            for (u32 j = 0; j < 8; j++)
            {
                cosTable[i][j] = ::std::cos((2 * j + 1) * i * 3.141592653589793 / 16.0);
            }
        }
        made = true;
    }
    double tmp[64];
    for (u32 y = 0; y < 8; y++)
    {
        for (u32 x = 0; x < 8; x++)
        {
            double sum = 0;
            for (u32 u = 0; u < 8; u++)
            {
                double cu = u == 0 ? 0.7071067811865476 : 1.0;
                sum += cu * block[y * 8 + u] * cosTable[u][x];
            }
            tmp[y * 8 + x] = sum * 0.5;
        }
    }
    for (u32 y = 0; y < 8; y++)
    {
        for (u32 x = 0; x < 8; x++)
        {
            double sum = 0;
            for (u32 v = 0; v < 8; v++)
            {
                double cv = v == 0 ? 0.7071067811865476 : 1.0;
                sum += cv * tmp[v * 8 + x] * cosTable[v][y];
            }
            pixels[y * 8 + x] = static_cast<s16>(::std::lround(sum * 0.5));
        }
    }
}

// Finds the next 0x000001 xx at or after from; returns the offset of the
// prefix, size when none, with the code byte in code.
u32 FindStart(const u8* data, u32 from, u32 size, u32* code)
{
    for (u32 i = from; i + 3 < size + 1 && i + 3 < size + 4; i++)
    {
        if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            if (i + 3 >= size)
            {
                return size;
            }
            *code = data[i + 3];
            return i;
        }
        if (i + 3 >= size)
        {
            break;
        }
    }
    return size;
}
}

Demux::Demux(const u8* data, u32 size) : m_data(data), m_size(size), m_position(0)
{
}

void Demux::SelectAudioChannel(u32 channel)
{
    m_audioChannel = channel;
}

bool Demux::Next(const ::std::function<void(const VideoPacket&)>& video,
                 const ::std::function<void(const u8*, u32)>& audio)
{
    if (m_position + 4 > m_size)
    {
        return false;
    }
    // Expect a pack header 0x000001BA, else scan to the next start code
    u32 code = 0;
    u32 at = FindStart(m_data, m_position, m_size, &code);
    if (at >= m_size)
    {
        m_position = m_size;
        return false;
    }
    m_position = at;
    if (code != 0xBA)
    {
        // Skip unknown start codes' packets: advance past the code
        m_position += 4;
        return true;
    }
    // Pack header: 0x000001BA + 10 bytes (MPEG-2), optional system header
    m_position += 4 + 10;
    if (m_position > m_size)
    {
        m_position = m_size;
        return false;
    }
    // Stuffing bytes (0xFF) after the pack header
    while (m_position < m_size && m_data[m_position] == 0xFF)
    {
        m_position++;
    }
    // System header 0x000001BB: skip by its header_length
    {
        u32 next = 0;
        u32 found = FindStart(m_data, m_position, m_size, &next);
        if (found == m_position && next == 0xBB)
        {
            if (m_position + 6 > m_size)
            {
                return false;
            }
            u32 headerLength = (static_cast<u32>(m_data[m_position + 4]) << 8) | m_data[m_position + 5];
            m_position += 6 + headerLength;
        }
    }
    // PES packets until the next pack
    while (true)
    {
        if (m_position + 4 > m_size)
        {
            return false;
        }
        u32 id = 0;
        u32 start = FindStart(m_data, m_position, m_size, &id);
        if (start >= m_size)
        {
            m_position = m_size;
            return false;
        }
        m_position = start;
        if (id == 0xBA || id == 0xB9)
        {
            return true; // next pack or program end: caller calls Next again
        }
        if (id == 0xBB)
        {
            // Late system header: skip
            if (m_position + 6 > m_size)
            {
                return false;
            }
            u32 headerLength = (static_cast<u32>(m_data[m_position + 4]) << 8) | m_data[m_position + 5];
            m_position += 6 + headerLength;
            continue;
        }
        if (m_position + 6 > m_size)
        {
            return false;
        }
        u32 streamId = id;
        u32 packetLength = (static_cast<u32>(m_data[m_position + 4]) << 8) | m_data[m_position + 5];
        u32 headerAt = m_position + 6;
        u32 packetEnd = headerAt + packetLength;
        if (packetEnd > m_size)
        {
            packetEnd = m_size;
        }
        if (streamId == 0xBC || streamId == 0xBE || streamId == 0xBF || streamId == 0xF0 || streamId == 0xF1 ||
            streamId == 0xF2 || streamId == 0xF8 || streamId == 0xFF)
        {
            m_position = packetEnd; // non-AV PES: skip
            continue;
        }
        if (headerAt + 3 > packetEnd)
        {
            m_position = packetEnd;
            continue;
        }
        // PES header flags (MPEG-2 path; MPEG-1 packets lack them and the
        // retail demuxer's branch would misread them; retail movies are
        // MPEG-2 packs so this matches the lifted path)
        u32 flags = m_data[headerAt + 1];
        u32 headerLength = m_data[headerAt + 2];
        u32 payloadAt = headerAt + 3 + headerLength;
        if (payloadAt > packetEnd)
        {
            m_position = packetEnd;
            continue;
        }
        // PTS/DTS parse kept minimal (timestamps ride along)
        s64 pts = -1;
        s64 dts = -1;
        if ((flags & 0xC0) != 0 && headerAt + 8 <= packetEnd)
        {
            u32 b0 = m_data[headerAt + 3];
            if ((b0 & 0xC0) == 0x80 || (b0 & 0xC0) == 0x00)
            {
                pts = (static_cast<s64>(b0 & 0x0E) << 29) | (static_cast<s64>(m_data[headerAt + 4]) << 22) |
                      (static_cast<s64>(m_data[headerAt + 5] & 0xFE) << 14) |
                      (static_cast<s64>(m_data[headerAt + 6]) << 7) | (m_data[headerAt + 7] >> 1);
            }
        }
        (void)dts;
        if (streamId == 0xE0)
        {
            VideoPacket packet{m_data + payloadAt, packetEnd - payloadAt, pts, dts};
            video(packet);
        }
        else if (streamId == 0xBD)
        {
            // Private stream 1: the first 4 bytes pick the sub-stream
            // (retail MpegStreamId: the sub-stream below the PES stream_id;
            // StoreAudio skips these SubStreamBytes, the DATA catch-all takes
            // FF 00 00 00, PCM takes FF A0 00 channel)
            if (payloadAt + 4 <= packetEnd)
            {
                u32 first = m_data[payloadAt];
                u32 second = m_data[payloadAt + 1];
                u32 third = m_data[payloadAt + 2];
                u32 channel = m_data[payloadAt + 3];
                if (first == 0xFF && second == 0xA0 && third == 0x00 && channel == m_audioChannel)
                {
                    audio(m_data + payloadAt + 4, packetEnd - payloadAt - 4);
                }
                else if (first == 0xFF && second == 0x00 && third == 0x00 && m_data[payloadAt + 3] == 0x00)
                {
                    VideoPacket packet{m_data + payloadAt + 4, packetEnd - payloadAt - 4, pts, dts};
                    video(packet);
                }
            }
        }
        else if (streamId >= 0xC0 && streamId <= 0xDF)
        {
            audio(m_data + payloadAt, packetEnd - payloadAt);
        }
        m_position = packetEnd;
    }
}
// The bit reader's operations (see mpeg2.h): big-endian bits straight off
// the bytes. Past-the-end reads set error and hitEnd (a short buffer);
// bad codes set error alone.
u32 BitReader::ShowBits(u32 n)
{
    u32 value = 0;
    u32 b = byte;
    u32 bt = bit;
    for (u32 i = 0; i < n; i++)
    {
        if (b >= size)
        {
            error = true;
            hitEnd = true;
            return 0;
        }
        value = (value << 1) | ((data[b] >> (7 - bt)) & 1);
        if (++bt == 8)
        {
            bt = 0;
            b++;
        }
    }
    return value;
}

void BitReader::SkipBits(u32 n)
{
    bit += n;
    byte += bit >> 3;
    bit &= 7;
    if (byte > size || (byte == size && bit != 0))
    {
        error = true;
        hitEnd = true;
    }
}

u32 BitReader::GetBits(u32 n)
{
    u32 value = ShowBits(n);
    SkipBits(n);
    return value;
}

u32 BitReader::GetBit()
{
    return GetBits(1);
}

void BitReader::Align()
{
    if ((bit & 7) != 0)
    {
        SkipBits(8 - (bit & 7));
    }
}

bool BitReader::AtEnd() const
{
    return byte >= size;
}

bool BitReader::NextIsStartCode()
{
    u32 b = byte + (bit == 0 ? 0 : 1);
    return b + 3 <= size && data[b] == 0 && data[b + 1] == 0 && data[b + 2] == 1;
}

u32 BitReader::ReadStartCode()
{
    Align();
    u32 code = 0;
    if (byte + 4 <= size && data[byte] == 0 && data[byte + 1] == 0 && data[byte + 2] == 1)
    {
        code = data[byte + 3];
        byte += 4;
    }
    else
    {
        error = true;
    }
    return code;
}

// Saturates dequantised coefficients and keeps the IDCT mismatch control
// (7.4.3/7.4.4, the MSSG Saturate): clamps to -2048..2047, then flips the
// last coefficient's low bit when the sum is even.
void SaturateBlock(s16* coefficients)
{
    s32 sum = 0;
    for (u32 i = 0; i < 64; i++)
    {
        s32 value = coefficients[i];
        if (value > 2047)
        {
            value = 2047;
        }
        else if (value < -2048)
        {
            value = -2048;
        }
        coefficients[i] = static_cast<s16>(value);
        sum += value;
    }
    if ((sum & 1) == 0)
    {
        coefficients[63] ^= 1;
    }
}

// Dual-prime derived vectors (7.6.3.6, the MSSG Dual_Prime_Arithmetic with
// retail's rounding): the opposite-parity field's prediction from the
// same-parity vector plus the differential, shifted a line.
void DualPrimeVectors(s32 topFirst, s32* derived, const s32* differential, s32 vectorX, s32 vectorY)
{
    if (topFirst != 0)
    {
        derived[0] = ((vectorX + (vectorX > 0 ? 1 : 0)) >> 1) + differential[0];
        derived[1] = ((vectorY + (vectorY > 0 ? 1 : 0)) >> 1) + differential[1] - 1;
        derived[2] = ((3 * vectorX + (vectorX > 0 ? 1 : 0)) >> 1) + differential[0];
        derived[3] = ((3 * vectorY + (vectorY > 0 ? 1 : 0)) >> 1) + differential[1] + 1;
    }
    else
    {
        derived[0] = ((3 * vectorX + (vectorX > 0 ? 1 : 0)) >> 1) + differential[0];
        derived[1] = ((3 * vectorY + (vectorY > 0 ? 1 : 0)) >> 1) + differential[1] - 1;
        derived[2] = ((vectorX + (vectorX > 0 ? 1 : 0)) >> 1) + differential[0];
        derived[3] = ((vectorY + (vectorY > 0 ? 1 : 0)) >> 1) + differential[1] + 1;
    }
}

// Scans newly fed bytes for start-code chunks (Get_Hdr's shape over the
// byte stream): sequence headers set the coded size and matrices, picture
// headers queue with their slices, the sequence end stops the scan. Slice
// bytes join the picture being built back to back, so a start-code look-
// alike inside slice data only cuts and rejoins the same bytes.
void Decoder::ScanStream()
{
    while (!m_sequenceEnded)
    {
        u32 code = 0;
        u32 at = FindStart(m_stream.data(), m_scan, static_cast<u32>(m_stream.size()), &code);
        if (at >= m_stream.size())
        {
            m_scan = static_cast<u32>(m_stream.size());
            return;
        }
        u32 next = 0;
        u32 follow = FindStart(m_stream.data(), at + 4, static_cast<u32>(m_stream.size()), &next);
        if (follow >= m_stream.size() && code != SequenceEndCode)
        {
            m_scan = at;
            return; // mid-chunk: waits for more bytes
        }
        u32 end = follow >= m_stream.size() ? static_cast<u32>(m_stream.size()) : follow;
        BitReader reader{m_stream.data() + at + 4, end - at - 4, 0, 0, false, false};
        if (code == SequenceHeaderCode)
        {
            FlushBuilding();
            m_scan = end;
            if (ParseSequence(&reader))
            {
                m_extContext = 1;
            }
            else if (reader.hitEnd)
            {
                m_scan = at;
                return;
            }
            continue;
        }
        if (code == ExtensionStartCode)
        {
            m_scan = end;
            if (!ParseExtension(&reader) && reader.hitEnd)
            {
                m_scan = at;
                return;
            }
            continue;
        }
        if (code == GroupStartCode)
        {
            // Time code and link flags: nothing the host shows
            FlushBuilding();
            m_scan = end;
            m_extContext = 0;
            continue;
        }
        if (code == PictureStartCode)
        {
            FlushBuilding();
            QueuedPicture picture;
            if (!ParsePictureHeader(&reader, &picture.info))
            {
                m_scan = reader.hitEnd ? at : end;
                m_extContext = 0;
                if (reader.hitEnd)
                {
                    return;
                }
                continue;
            }
            m_scan = end;
            m_building = picture;
            m_buildingBytes.clear();
            m_extContext = 2;
            continue;
        }
        if (code == SequenceEndCode)
        {
            m_scan = end;
            FlushBuilding();
            m_sequenceEnded = true;
            m_extContext = 0;
            return;
        }
        if (code >= SliceStartCodeMin && code <= SliceStartCodeMax)
        {
            m_scan = end;
            if (m_extContext == 2)
            {
                m_buildingBytes.insert(m_buildingBytes.end(), m_stream.data() + at, m_stream.data() + end);
            }
            continue;
        }
        // User data and anything else: skip
        m_scan = end;
    }
}

void Decoder::FlushBuilding()
{
    if (m_extContext != 2)
    {
        return;
    }
    m_extContext = 0;
    m_building.bytes = m_buildingBytes;
    m_buildingBytes.clear();
    // Zero lookahead room (the MSSG bit buffer reads past the picture's
    // end into zeros at the stream's end): the last block's 16-bit VLC
    // peek and escape reads stay in bounds, and the slice-end zero run
    // still resyncs before them
    m_building.bytes.insert(m_building.bytes.end(), 8, 0);
    m_queued.push_back(m_building);
    m_building = QueuedPicture();
}

// The sequence header (6.2.2.1, the MSSG sequence_header): coded size,
// grid and quantiser matrices. The display size starts at the coded size;
// the display extension may shrink it later.
bool Decoder::ParseSequence(BitReader* reader)
{
    u32 horizontal = reader->GetBits(12);
    u32 vertical = reader->GetBits(12);
    reader->SkipBits(4); // aspect_ratio_information
    u32 frameRateCode = reader->GetBits(4);
    reader->SkipBits(18);    // bit_rate_value
    reader->SkipBits(1);     // marker_bit
    reader->SkipBits(10);    // vbv_buffer_size_value
    reader->SkipBits(1);     // constrained_parameters_flag
    if (reader->error || horizontal == 0 || vertical == 0 || horizontal > 4096 || vertical > 4096)
    {
        reader->error = true;
        return false;
    }
    bool loadIntra = reader->GetBit() != 0;
    u8 intra[64];
    if (loadIntra)
    {
        for (u32 i = 0; i < 64; i++)
        {
            intra[ZigZag[i]] = static_cast<u8>(reader->GetBits(8));
        }
    }
    bool loadNonIntra = reader->GetBit() != 0;
    u8 nonIntra[64];
    if (loadNonIntra)
    {
        for (u32 i = 0; i < 64; i++)
        {
            nonIntra[ZigZag[i]] = static_cast<u8>(reader->GetBits(8));
        }
    }
    if (reader->error)
    {
        return false;
    }
    // A second sequence reuses the reference slots at the new size
    m_codedWidth = (horizontal + 15) & ~15u;
    m_codedHeight = (vertical + 15) & ~15u;
    m_mbWidth = m_codedWidth >> 4;
    m_mbHeight = m_codedHeight >> 4;
    if (!loadIntra)
    {
        for (u32 i = 0; i < 64; i++)
        {
            intra[i] = DefaultIntra[i];
        }
    }
    if (!loadNonIntra)
    {
        for (u32 i = 0; i < 64; i++)
        {
            nonIntra[i] = 16;
        }
    }
    for (u32 i = 0; i < 64; i++)
    {
        m_intraMatrix[i] = intra[i];
        m_nonIntraMatrix[i] = nonIntra[i];
    }
    u32 planeBytes = m_codedWidth * m_codedHeight * 3 / 2;
    m_past.assign(planeBytes, 0);
    m_future.assign(planeBytes, 0);
    m_current.assign(planeBytes, 0);
    m_havePast = false;
    m_haveFuture = false;
    m_width = horizontal;
    m_height = vertical;
    m_progressiveSequence = true;
    m_frameRateCode = frameRateCode;
    return true;
}

// An extension after a sequence or picture header (6.2.2.2): sequence ids
// set the profile flags and display size, picture ids the coding switches.
// m_extContext picks whose. Returns false on bad bits.
bool Decoder::ParseExtension(BitReader* reader)
{
    u32 id = reader->GetBits(4);
    if (reader->error)
    {
        return false;
    }
    if (m_extContext == 2)
    {
        if (id == PictureCodingExtensionId)
        {
            PictureInfo* info = &m_building.info;
            info->fCode[0][0] = static_cast<s32>(reader->GetBits(4));
            info->fCode[0][1] = static_cast<s32>(reader->GetBits(4));
            info->fCode[1][0] = static_cast<s32>(reader->GetBits(4));
            info->fCode[1][1] = static_cast<s32>(reader->GetBits(4));
            info->intraDcPrecision = reader->GetBits(2);
            u32 structure = reader->GetBits(2);
            info->topFieldFirst = reader->GetBit() != 0;
            info->framePredFrameDct = reader->GetBit() != 0;
            info->concealmentVectors = reader->GetBit() != 0;
            info->qScaleType = reader->GetBit() != 0;
            info->intraVlcFormat = reader->GetBit() != 0;
            info->alternateScan = reader->GetBit() != 0;
            info->repeatFirstField = reader->GetBit() != 0;
            reader->SkipBits(1); // chroma_420_type
            info->progressiveFrame = reader->GetBit() != 0;
            bool composite = reader->GetBit() != 0;
            if (composite)
            {
                reader->SkipBits(1 + 3 + 1 + 7 + 8);
            }
            if (reader->error || structure != 3)
            {
                reader->error = true;
                return false; // frame pictures only
            }
            return true;
        }
        if (id == 7)
        {
            // Picture display extension (6.2.3.5): frame centre offsets the
            // host never shows; their count comes from the coding switches
            u32 offsets = 1;
            if (m_progressiveSequence)
            {
                offsets = m_building.info.repeatFirstField ? (m_building.info.topFieldFirst ? 3 : 2) : 1;
            }
            else if (m_building.info.repeatFirstField)
            {
                offsets = 3;
            }
            else
            {
                offsets = 2;
            }
            reader->SkipBits(offsets * 32);
            return !reader->error;
        }
        return !reader->error; // anything else: no payload the host reads
    }
    if (id == 1)
    {
        u32 profileLevel = reader->GetBits(8);
        m_progressiveSequence = reader->GetBit() != 0;
        u32 chroma = reader->GetBits(2);
        u32 widthExt = reader->GetBits(2);
        u32 heightExt = reader->GetBits(2);
        reader->SkipBits(12); // bit_rate_extension
        reader->SkipBits(1); // marker_bit
        reader->SkipBits(8); // vbv_buffer_size_extension
        reader->SkipBits(1 + 2 + 5); // low_delay, frame rate extensions
        if (reader->error || chroma != 1 || profileLevel == 0)
        {
            reader->error = true;
            return false; // 4:2:0 only
        }
        u32 horizontal = (widthExt << 12) | m_width;
        u32 vertical = (heightExt << 12) | m_height;
        if (horizontal != m_width || vertical != m_height)
        {
            reader->error = true;
            return false;
        }
        return true;
    }
    if (id == 2)
    {
        reader->SkipBits(3); // video_format
        if (reader->GetBit() != 0)
        {
            reader->SkipBits(24); // colour description
        }
        u32 horizontal = reader->GetBits(14);
        reader->SkipBits(1); // marker_bit
        u32 vertical = reader->GetBits(14);
        if (reader->error)
        {
            return false;
        }
        if (horizontal != 0 && horizontal <= m_codedWidth)
        {
            m_width = horizontal;
        }
        if (vertical != 0 && vertical <= m_codedHeight)
        {
            m_height = vertical;
        }
        return true;
    }
    if (id == 3)
    {
        // Quant matrix extension (6.2.3.2 / 6.2.2.5): reloads either matrix.
        // id 4 is the copyright extension, not a matrix, so it is not read
        // as one (its payload, like every extension's, is bounded to the
        // next start code).
        for (u32 m = 0; m < 2; m++)
        {
            if (reader->GetBit() == 0)
            {
                continue;
            }
            u8* matrix = (m == 0) ? m_intraMatrix : m_nonIntraMatrix;
            for (u32 i = 0; i < 64; i++)
            {
                matrix[ZigZag[i]] = static_cast<u8>(reader->GetBits(8));
            }
        }
        return !reader->error;
    }
    return !reader->error;
}

// The picture header (6.2.3.1, the MSSG picture_header): coding type and
// the P/B vector flags. Extensions arrive as later chunks.
bool Decoder::ParsePictureHeader(BitReader* reader, PictureInfo* info)
{
    reader->SkipBits(10); // temporal_reference
    u32 type = reader->GetBits(3);
    reader->SkipBits(16); // vbv_delay
    if (reader->error || type < 1 || type > 3)
    {
        reader->error = true;
        return false; // I/P/B only
    }
    info->pictureType = type;
    if (type == 2 || type == 3)
    {
        // One forward_f_code shared by both components (6.2.3.1); the
        // coding extension splits horizontal/vertical later
        info->fullPelForward = reader->GetBit() != 0;
        s32 forward = static_cast<s32>(reader->GetBits(3));
        info->fCode[0][0] = info->fCode[0][1] = forward;
    }
    if (type == 3)
    {
        info->fullPelBackward = reader->GetBit() != 0;
        s32 backward = static_cast<s32>(reader->GetBits(3));
        info->fCode[1][0] = info->fCode[1][1] = backward;
    }
    while (reader->GetBit() != 0)
    {
        reader->SkipBits(8); // extra_information_picture
        if (reader->error)
        {
            return false;
        }
    }
    return !reader->error;
}

Decoder::Decoder()
{
    m_blocks.resize(384);
    m_idct.resize(384);
    m_prediction.resize(384);
    for (u32 i = 0; i < 64; i++)
    {
        m_intraMatrix[i] = DefaultIntra[i];
        m_nonIntraMatrix[i] = 16;
    }
}
void Decoder::Feed(const u8* data, u32 length)
{
    m_stream.insert(m_stream.end(), data, data + length);
}


// All slices of a picture (6.2.3.7 and 6.3.16, the MSSG picture_data and
// slice): each slice header sets the row and quantiser, then macroblocks
// run to the picture's end. Skipped addresses predict without residual.
// Returns false when no macroblock decoded (a bad picture to skip).
bool Decoder::DecodeSlices(BitReader* reader, const PictureInfo* info, u8* target)
{
    u32 coded = m_mbWidth * m_mbHeight;
    ::std::memset(target, 0, m_codedWidth * m_codedHeight * 3 / 2);
    ::std::memset(m_prediction.data(), 0, 384 * sizeof(s16));
    u32 decoded = 0;
    u32 mbType = 0;
    while (true)
    {
        reader->Align();
        if (!reader->NextIsStartCode())
        {
            break;
        }
        u32 code = reader->ReadStartCode();
        if (code < SliceStartCodeMin || code > SliceStartCodeMax)
        {
            break; // next picture or sequence end
        }
        if (reader->error)
        {
            break;
        }
        u32 row = code - SliceStartCodeMin;
        if (row >= m_mbHeight)
        {
            reader->error = true;
            break;
        }
        s32 quantScale = static_cast<s32>(reader->GetBits(5));
        quantScale = info->qScaleType ? NonLinearScale[quantScale] : quantScale << 1;
        if (reader->GetBit() != 0)
        {
            reader->SkipBits(1 + 1 + 6); // intra_slice, id enable, id
            while (reader->GetBit() != 0)
            {
                reader->SkipBits(8); // extra_information_slice
                if (reader->error)
                {
                    break;
                }
            }
        }
        if (reader->error)
        {
            break;
        }
        s32 increment = DecodeIncrement(reader);
        if (reader->error || increment < 1)
        {
            reader->error = true;
            break;
        }
        u32 address = row * m_mbWidth + static_cast<u32>(increment) - 1;
        if (address >= coded)
        {
            break; // past the picture: done
        }
        s32 predictors[2][2][2] = {};
        s32 dcPredictors[3] = {};
        s32 fieldSelect[2][2] = {};
        increment = 1; // the header's own macroblock is never skipped
        while (address < coded)
        {
            if (increment == 0)
            {
                // Slice end check (6.3.16, the MSSG slice loop's
                // Show_Bits(23)): 23 zero bits resync to the next slice,
                // as does zero padding running to the chunk's end (chunks
                // stop where the next start code begins)
                if (reader->error)
                {
                    break; // resync: next slice
                }
                u32 zeroRun = 0;
                u32 peekByte = reader->byte;
                u32 peekBit = reader->bit;
                while (zeroRun < 23)
                {
                    if (peekByte >= reader->size)
                    {
                        break;
                    }
                    if (((reader->data[peekByte] >> (7 - peekBit)) & 1) != 0)
                    {
                        break;
                    }
                    zeroRun++;
                    if (++peekBit == 8)
                    {
                        peekBit = 0;
                        peekByte++;
                    }
                }
                if (zeroRun == 23 || peekByte >= reader->size)
                {
                    break; // resync: next slice
                }
                increment = DecodeIncrement(reader);
                if (reader->error || increment < 0)
                {
                    reader->error = true;
                    break;
                }
                if (address + static_cast<u32>(increment) > coded)
                {
                    address = coded;
                    break;
                }
            }
            if (increment == 1)
            {
                u32 motionType = 2;
                u32 dctType = 0;
                if (!DecodeMacroblock(reader, info, address, &quantScale, predictors, dcPredictors, fieldSelect,
                                      &motionType, &dctType, &mbType))
                {
                    break; // fault: resync to the next slice
                }
                FormPrediction(info, address, mbType, motionType, predictors, fieldSelect, m_dmVector);
                StoreMacroblock(target, address, mbType, dctType);
                decoded++;
            }
            else
            {
                // Skipped (7.6.6, the MSSG skipped_macroblock): no
                // residual, vectors kept for B and cleared for P
                ::std::memset(m_blocks.data(), 0, 384 * sizeof(s16));
                dcPredictors[0] = dcPredictors[1] = dcPredictors[2] = 0;
                if (info->pictureType == 2)
                {
                    predictors[0][0][0] = predictors[0][0][1] = predictors[1][0][0] = predictors[1][0][1] = 0;
                }
                mbType &= ~1u; // clears intra, keeps the prediction flags
                FormPrediction(info, address, mbType, 2, predictors, fieldSelect, m_dmVector);
                StoreMacroblock(target, address, mbType, 0);
                decoded++;
            }
            address++;
            increment--;
            if (address >= coded)
            {
                return decoded > 0;
            }
        }
        if (reader->error)
        {
            break;
        }
    }
    return decoded > 0 && !reader->error;
}

// One macroblock (6.3.17 and 7.6, the MSSG decode_macroblock): modes,
// quantiser, vectors, pattern and the six blocks. motionType and dctType
// come back for the prediction and store stages. Returns false on bad bits.
bool Decoder::DecodeMacroblock(BitReader* reader, const PictureInfo* info, u32 address, s32* quantScale,
                              s32 predictors[2][2][2], s32 dcPredictors[3], s32 fieldSelect[2][2], u32* motionType,
                              u32* dctType, u32* mbType)
{
    // address is the caller's loop counter; this stage only reads the type and vectors from the bitstream
    (void)address;
    s32 type = DecodeMbType(reader, info->pictureType);
    if (reader->error || type < 0)
    {
        reader->error = true;
        return false;
    }
    bool intra = (type & 1) != 0;
    bool pattern = (type & 2) != 0;
    bool backward = (type & 4) != 0;
    bool forward = (type & 8) != 0;
    bool quant = (type & 16) != 0;
    u32 motion = 2;
    if (forward || backward)
    {
        motion = info->framePredFrameDct ? 2 : reader->GetBits(2);
        if (reader->error || motion < 1 || motion > 3)
        {
            reader->error = true;
            return false;
        }
    }
    else if (intra && info->concealmentVectors)
    {
        motion = 2;
    }
    u32 dct = 0;
    if (!info->framePredFrameDct && (pattern || intra))
    {
        dct = reader->GetBit();
    }
    if (quant)
    {
        s32 code = static_cast<s32>(reader->GetBits(5));
        *quantScale = info->qScaleType ? NonLinearScale[code] : code << 1;
    }
    if (reader->error)
    {
        return false;
    }
    bool fieldFormat = motion != 2;
    u32 count = motion == 1 ? 2 : 1;
    bool dualPrime = motion == 3;
    bool scale = fieldFormat;
    if (forward || (intra && info->concealmentVectors))
    {
        DecodeVectors(reader, predictors, m_dmVector, fieldSelect, 0, count, fieldFormat ? 1 : 0, info->fCode[0],
                      dualPrime, scale, info->fullPelForward);
    }
    if (reader->error)
    {
        return false;
    }
    if (backward)
    {
        DecodeVectors(reader, predictors, m_dmVector, fieldSelect, 1, count, fieldFormat ? 1 : 0, info->fCode[1],
                      false, scale, info->fullPelBackward);
    }
    if (reader->error)
    {
        return false;
    }
    if (intra && info->concealmentVectors)
    {
        reader->SkipBits(1); // marker_bit
    }
    s32 patternCode = 0;
    if (pattern)
    {
        patternCode = DecodeCbp(reader);
        if (reader->error || patternCode < 0)
        {
            reader->error = true;
            return false;
        }
    }
    else
    {
        patternCode = intra ? 0x3F : 0;
    }
    ::std::memset(m_blocks.data(), 0, 384 * sizeof(s16));
    for (u32 comp = 0; comp < 6; comp++)
    {
        if ((patternCode & (1 << (5 - comp))) == 0)
        {
            continue;
        }
        u32 component = comp < 4 ? 0 : comp - 3;
        s32* dcPredictor = &dcPredictors[component == 0 ? 0 : component];
        if (!DecodeBlock(reader, info, m_blocks.data() + comp * 64, intra, dcPredictor, component, *quantScale))
        {
            return false;
        }
    }
    if (!intra)
    {
        dcPredictors[0] = dcPredictors[1] = dcPredictors[2] = 0;
    }
    else if (!info->concealmentVectors)
    {
        for (u32 r = 0; r < 2; r++)
        {
            for (u32 s = 0; s < 2; s++)
            {
                predictors[r][s][0] = predictors[r][s][1] = 0;
            }
        }
    }
    if (info->pictureType == 2 && !intra && !forward)
    {
        // No-MC (7.6.3.5): a P macroblock without vectors copies the past
        predictors[0][0][0] = predictors[0][0][1] = predictors[1][0][0] = predictors[1][0][1] = 0;
        motion = 2;
    }
    *motionType = motion;
    *dctType = dct;
    *mbType = static_cast<u32>(type);
    return !reader->error;
}

// A direction's vectors (7.6.3 and 6.3.17.2, the MSSG motion_vectors):
// field selects first for two-vector predictions, then each vector. One
// vector updates both predictors of its direction.
void Decoder::DecodeVectors(BitReader* reader, s32 predictors[2][2][2], s32* differential, s32 fieldSelect[2][2],
                           u32 direction, u32 count, u32 fieldFormat, const s32* fCode, bool dualPrime, bool scale,
                           bool fullPel)
{
    if (count == 1)
    {
        if (fieldFormat != 0 && !dualPrime)
        {
            fieldSelect[direction][0] = fieldSelect[direction][1] = static_cast<s32>(reader->GetBit());
        }
        DecodeVector(reader, predictors[0][direction], differential, fCode[0], fCode[1], dualPrime, scale, fullPel);
        predictors[1][direction][0] = predictors[0][direction][0];
        predictors[1][direction][1] = predictors[0][direction][1];
        return;
    }
    fieldSelect[direction][0] = static_cast<s32>(reader->GetBit());
    DecodeVector(reader, predictors[0][direction], differential, fCode[0], fCode[1], dualPrime, scale, fullPel);
    fieldSelect[direction][1] = static_cast<s32>(reader->GetBit());
    DecodeVector(reader, predictors[1][direction], differential, fCode[0], fCode[1], dualPrime, scale, fullPel);
}

// One vector's components (7.6.3.1, the MSSG motion_vector with retail's
// DecodeComponent arithmetic): code plus residual, wrapped into range.
// Field vectors in frame pictures scale around the vertical decode.
void Decoder::DecodeVector(BitReader* reader, s32* predictor, s32* differential, s32 fCodeH, s32 fCodeV,
                          bool dualPrime, bool scale, bool fullPel)
{
    s32 code = DecodeMotionCode(reader);
    s32 residual = 0;
    if (fCodeH > 1 && code != 0)
    {
        residual = static_cast<s32>(reader->GetBits(static_cast<u32>(fCodeH - 1)));
    }
    DecodeComponent(predictor, 0, fCodeH - 1, code, residual, fullPel);
    if (dualPrime)
    {
        differential[0] = DecodeDmVector(reader);
    }
    code = DecodeMotionCode(reader);
    residual = 0;
    if (fCodeV > 1 && code != 0)
    {
        residual = static_cast<s32>(reader->GetBits(static_cast<u32>(fCodeV - 1)));
    }
    if (scale)
    {
        predictor[1] >>= 1;
    }
    DecodeComponent(predictor, 1, fCodeV - 1, code, residual, fullPel);
    if (scale)
    {
        predictor[1] <<= 1;
    }
    if (dualPrime)
    {
        differential[1] = DecodeDmVector(reader);
    }
}

// A vector component from its predictor, code and residual (7.6.3.1,
// retail mpegdecode.cpp's DecodeComponent): full-pel counts whole pixels.
void Decoder::DecodeComponent(s32* predictor, u32 component, s32 residualSize, s32 code, s32 residual, bool fullPel)
{
    s32 limit = 16 << residualSize;
    s32 value = fullPel ? predictor[component] >> 1 : predictor[component];
    if (code > 0)
    {
        value += ((code - 1) << residualSize) + residual + 1;
        if (value >= limit)
        {
            value -= limit * 2;
        }
    }
    else if (code < 0)
    {
        value -= ((-code - 1) << residualSize) + residual + 1;
        if (value < -limit)
        {
            value += limit * 2;
        }
    }
    predictor[component] = fullPel ? value << 1 : value;
}

// One block (7.2 through 7.4, the MSSG Decode_MPEG2_Intra/Non_Intra_Block):
// DC prediction for intra, quantised AC levels in scan order, then
// dequantisation and mismatch control. Coefficients land raster-ordered.
bool Decoder::DecodeBlock(BitReader* reader, const PictureInfo* info, s16* coefficients, bool intra,
                         s32* dcPredictor, u32 component, s32 quantScale)
{
    const u8* matrix = intra ? m_intraMatrix : m_nonIntraMatrix;
    const u8* scan = info->alternateScan ? AlternateScan : ZigZag;
    if (intra)
    {
        s32 size = DecodeDcSize(reader, component != 0);
        if (reader->error || size < 0)
        {
            reader->error = true;
            return false;
        }
        *dcPredictor += DecodeDcDiff(reader, size);
        if (reader->error)
        {
            return false;
        }
        coefficients[0] = static_cast<s16>(*dcPredictor << (3 - info->intraDcPrecision));
    }
    s16 levels[64] = {};
    if (!DecodeAcCoefficients(reader, levels, info->intraVlcFormat && intra, !intra))
    {
        return false;
    }
    for (u32 i = (intra ? 1u : 0u); i < 64; i++)
    {
        if (levels[i] == 0)
        {
            continue;
        }
        u32 j = scan[i];
        s32 magnitude = levels[i] < 0 ? -levels[i] : levels[i];
        s32 value = 0;
        if (intra)
        {
            value = (magnitude * quantScale * matrix[j]) >> 4;
        }
        else
        {
            value = (((magnitude << 1) + 1) * quantScale * matrix[j]) >> 5;
        }
        coefficients[j] = static_cast<s16>(levels[i] < 0 ? -value : value);
    }
    SaturateBlock(coefficients);
    return !reader->error;
}
// One clamped prediction call through predict.h's scalars: the source
// position in half-pels clamped so the half-pixel reads stay in the plane
// (libmpeg2's limit clamp, folded into the position).
void Decoder::PredictPlane(const u8* plane, u32 stride, u32 planeWidth, u32 planeHeight, u32 x0, u32 y0, s32 vectorX,
                          s32 vectorY, u32 width, u32 height, s16* out, u32 outStride, bool average)
{
    s32 maxX = static_cast<s32>(2 * (planeWidth - width));
    s32 maxY = static_cast<s32>(2 * (planeHeight - height));
    s32 posX = static_cast<s32>(2 * x0) + vectorX;
    s32 posY = static_cast<s32>(2 * y0) + vectorY;
    posX = posX < 0 ? 0 : posX > maxX ? maxX : posX;
    posY = posY < 0 ? 0 : posY > maxY ? maxY : posY;
    u32 halfX = static_cast<u32>(posX & 1);
    u32 halfY = static_cast<u32>(posY & 1);
    PredictionBlock block{plane,
                          stride,
                          static_cast<u32>(posX >> 1),
                          static_cast<u32>(posY >> 1),
                          width,
                          height,
                          out,
                          outStride,
                          average ? 1u : 0u,
                          halfX,
                          halfY};
    g_PredictionRoutines[(average ? 4 : 0) | (halfX << 1) | halfY](&block);
}

// A macroblock's prediction (7.6 and 7.6.4, retail GetReferences over
// linear planes): frame vectors predict the whole block, field vectors
// each field from its selected field. Backward predictions average onto
// the forward one; dual-prime averages the opposite-parity fields.
void Decoder::FormPrediction(const PictureInfo* info, u32 address, u32 mbType, u32 motionType,
                            s32 predictors[2][2][2], s32 fieldSelect[2][2], s32 differential[2])
{
    (void)info;
    if ((mbType & 1) != 0)
    {
        return; // intra: no prediction
    }
    bool forward = (mbType & 8) != 0 || m_pictureType == 2;
    bool backward = (mbType & 4) != 0;
    u32 mbX = address % m_mbWidth;
    u32 mbY = address / m_mbWidth;
    if (forward)
    {
        PredictDirection(mbX, mbY, m_past.data(), motionType, predictors[0][0], predictors[1][0], fieldSelect[0],
                         differential, false);
    }
    if (backward)
    {
        PredictDirection(mbX, mbY, m_future.data(), motionType, predictors[0][1], predictors[1][1], fieldSelect[1],
                         differential, forward);
    }
}

// One direction's prediction into m_prediction (average set for the second
// of two): frame, field or dual-prime motion.
void Decoder::PredictDirection(u32 mbX, u32 mbY, const u8* ref, u32 motionType, const s32* first, const s32* second,
                              const s32* fieldSelect, const s32* differential, bool average)
{
    u32 width = m_codedWidth;
    u32 height = m_codedHeight;
    u32 chromaWidth = width >> 1;
    u32 chromaHeight = height >> 1;
    const u8* refCb = ref + width * height;
    const u8* refCr = refCb + chromaWidth * chromaHeight;
    s16* outCb = m_prediction.data() + 256;
    s16* outCr = outCb + 64;
    if (motionType == 2)
    {
        // Frame prediction: the whole block at once
        PredictPlane(ref, width, width, height, mbX * 16, mbY * 16, first[0], first[1], 16, 16, m_prediction.data(),
                     16, average);
        s32 chromaX = first[0] / 2;
        s32 chromaY = first[1] / 2;
        PredictPlane(refCb, chromaWidth, chromaWidth, chromaHeight, mbX * 8, mbY * 8, chromaX, chromaY, 8, 8, outCb,
                     8, average);
        PredictPlane(refCr, chromaWidth, chromaWidth, chromaHeight, mbX * 8, mbY * 8, chromaX, chromaY, 8, 8, outCr,
                     8, average);
        return;
    }
    if (motionType == 3 && !average)
    {
        // Dual prime (7.6.4): each field from the reference field of its own parity at the coded vector, averaged
        // with the other parity's field at the derived one. First directions only: a second one (average set)
        // is a B picture's backward prediction, which retail never runs as dual prime - GetReferences sends every
        // non-frame motion type of a B picture's future frame down the field path below ("B pictures have no dual
        // prime") - so it takes the field selects and the second vector there and still averages onto the first
        s32 derived[4];
        DualPrimeVectors(m_topFieldFirst ? 1 : 0, derived, differential, first[0], first[1] >> 1);
        PredictField(ref, width, height, mbX, mbY, 0, 0, first[0], first[1] >> 1, m_prediction.data(), false);
        PredictField(ref, width, height, mbX, mbY, 1, 0, derived[0], derived[1], m_prediction.data(), true);
        PredictField(ref, width, height, mbX, mbY, 1, 1, first[0], first[1] >> 1, m_prediction.data(), false);
        PredictField(ref, width, height, mbX, mbY, 0, 1, derived[2], derived[3], m_prediction.data(), true);
        return;
    }
    // Field prediction: each destination field from its selected field
    PredictField(ref, width, height, mbX, mbY, fieldSelect[0] != 0 ? 1 : 0, 0, first[0], first[1] >> 1,
                 m_prediction.data(), average);
    PredictField(ref, width, height, mbX, mbY, fieldSelect[1] != 0 ? 1 : 0, 1, second[0], second[1] >> 1,
                 m_prediction.data(), average);
}

// One field's 16x8 luma plus 8x4 chroma from the source field rows
// (stride doubled), written to the destination field's rows.
void Decoder::PredictField(const u8* ref, u32 width, u32 height, u32 mbX, u32 mbY, u32 srcField, u32 dstField,
                          s32 vectorX, s32 vectorY, s16* out, bool average)
{
    u32 chromaWidth = width >> 1;
    u32 chromaHeight = height >> 1;
    const u8* refCb = ref + width * height;
    const u8* refCr = refCb + chromaWidth * chromaHeight;
    // Luma field rows live doubled apart; the field's height is halved
    s32 maxX = static_cast<s32>(2 * (width - 16));
    s32 maxY = static_cast<s32>(2 * (height / 2 - 8));
    s32 posX = static_cast<s32>(2 * mbX * 16) + vectorX;
    s32 posY = static_cast<s32>(2 * mbY * 8) + vectorY;
    posX = posX < 0 ? 0 : posX > maxX ? maxX : posX;
    posY = posY < 0 ? 0 : posY > maxY ? maxY : posY;
    u32 halfX = static_cast<u32>(posX & 1);
    u32 halfY = static_cast<u32>(posY & 1);
    PredictionBlock luma{ref + srcField * width,
                         width * 2,
                         static_cast<u32>(posX >> 1),
                         static_cast<u32>(posY >> 1),
                         16,
                         8,
                         out + dstField * 16,
                         32,
                         average ? 1u : 0u,
                         halfX,
                         halfY};
    g_PredictionRoutines[(average ? 4 : 0) | (halfX << 1) | halfY](&luma);
    // Chroma halves the vectors toward zero (7.6.4, retail's GetReference)
    s32 chromaX = vectorX / 2;
    s32 chromaY = vectorY / 2;
    s32 chromaMaxX = static_cast<s32>(2 * (chromaWidth - 8));
    s32 chromaMaxY = static_cast<s32>(2 * (chromaHeight / 2 - 4));
    s32 chromaPosX = static_cast<s32>(2 * mbX * 8) + chromaX;
    s32 chromaPosY = static_cast<s32>(2 * mbY * 4) + chromaY;
    chromaPosX = chromaPosX < 0 ? 0 : chromaPosX > chromaMaxX ? chromaMaxX : chromaPosX;
    chromaPosY = chromaPosY < 0 ? 0 : chromaPosY > chromaMaxY ? chromaMaxY : chromaPosY;
    u32 chromaHalfX = static_cast<u32>(chromaPosX & 1);
    u32 chromaHalfY = static_cast<u32>(chromaPosY & 1);
    for (u32 plane = 0; plane < 2; plane++)
    {
        const u8* src = (plane == 0 ? refCb : refCr) + srcField * chromaWidth;
        PredictionBlock chroma{src,
                               chromaWidth * 2,
                               static_cast<u32>(chromaPosX >> 1),
                               static_cast<u32>(chromaPosY >> 1),
                               8,
                               4,
                               out + 256 + plane * 64 + dstField * 8,
                               16,
                               average ? 1u : 0u,
                               chromaHalfX,
                               chromaHalfY};
        g_PredictionRoutines[(average ? 4 : 0) | (chromaHalfX << 1) | chromaHalfY](&chroma);
    }
}

// The macroblock's pixels (7.6.7 and 7.6.8, retail StoreMacroblock over
// linear planes): IDCT each block into frame layout (field DCT blocks
// take their field's rows), then store intra results shifted to 0-255 or
// add the prediction to the residual.
void Decoder::StoreMacroblock(u8* target, u32 address, u32 mbType, u32 dctType)
{
    s16 pixels[64];
    s16* idct = m_idct.data();
    for (u32 comp = 0; comp < 6; comp++)
    {
        InverseDct(m_blocks.data() + comp * 64, pixels);
        s16* plane = nullptr;
        u32 base = 0;
        u32 stride = 16;
        if (comp < 4)
        {
            plane = idct;
            if (dctType == 0)
            {
                base = (comp >> 1) * 8 * 16 + (comp & 1) * 8;
            }
            else
            {
                // Field DCT: each block takes its field's rows
                base = ((comp & 2) >> 1) * 16 + (comp & 1) * 8;
                stride = 32;
            }
        }
        else
        {
            // Chroma stays frame-arranged for 4:2:0
            plane = idct + 256 + (comp - 4) * 64;
            base = 0;
            stride = 8;
        }
        for (u32 row = 0; row < 8; row++)
        {
            for (u32 column = 0; column < 8; column++)
            {
                plane[base + row * stride + column] = pixels[row * 8 + column];
            }
        }
    }
    u32 mbX = address % m_mbWidth;
    u32 mbY = address / m_mbWidth;
    MacroblockDestination destination{target + mbY * 16 * m_codedWidth + mbX * 16,
                                      target + m_codedWidth * m_codedHeight + mbY * 8 * (m_codedWidth / 2) + mbX * 8,
                                      target + m_codedWidth * m_codedHeight * 5 / 4 +
                                          mbY * 8 * (m_codedWidth / 2) + mbX * 8,
                                      m_codedWidth,
                                      m_codedWidth / 2};
    if ((mbType & 1) != 0)
    {
        for (u32 i = 0; i < 384; i++)
        {
            idct[i] += 128; // the IDCT's zero-centred range to pixels
        }
        StorePixels(&destination, idct);
    }
    else
    {
        AddPrediction(&destination, m_prediction.data(), idct);
    }
}

// An anchor picture: rotates the references (the MSSG
// Update_Picture_Buffers) and decodes into the future plane.
bool Decoder::DecodeAnchor(QueuedPicture* picture)
{
    if (m_mbWidth == 0)
    {
        return false;
    }
    m_past.swap(m_future);
    m_havePast = m_anchorsDecoded >= 1;
    BitReader reader{picture->bytes.data(), static_cast<u32>(picture->bytes.size()), 0, 0, false, false};
    m_pictureType = picture->info.pictureType;
    m_topFieldFirst = picture->info.topFieldFirst;
    bool ok = DecodeSlices(&reader, &picture->info, m_future.data());
    m_haveFuture = ok;
    if (ok)
    {
        m_anchorsDecoded++;
        m_heldAnchor = YuvFrame();
        CropFrame(m_future, &m_heldAnchor);
        m_haveAnchor = true;
    }
    return ok;
}

// A B picture into the current plane, shown at once
bool Decoder::DecodeBPicture(QueuedPicture* picture, YuvFrame* frame)
{
    if (m_mbWidth == 0)
    {
        return false;
    }
    BitReader reader{picture->bytes.data(), static_cast<u32>(picture->bytes.size()), 0, 0, false, false};
    m_pictureType = picture->info.pictureType;
    m_topFieldFirst = picture->info.topFieldFirst;
    if (!DecodeSlices(&reader, &picture->info, m_current.data()))
    {
        return false;
    }
    CropFrame(m_current, frame);
    return true;
}

// Crops the coded planes to the display size (the sequence display
// extension's size when the stream sets one)
void Decoder::CropFrame(const ::std::vector<u8>& planes, YuvFrame* frame)
{
    frame->width = m_width;
    frame->height = m_height;
    u32 chromaWidth = m_width >> 1;
    u32 chromaHeight = m_height >> 1;
    frame->pixels.resize(m_width * m_height + chromaWidth * chromaHeight * 2);
    const u8* y = planes.data();
    for (u32 row = 0; row < m_height; row++)
    {
        ::std::memcpy(frame->pixels.data() + row * m_width, y + row * m_codedWidth, m_width);
    }
    const u8* cb = y + m_codedWidth * m_codedHeight;
    u8* outCb = frame->pixels.data() + m_width * m_height;
    for (u32 row = 0; row < chromaHeight; row++)
    {
        ::std::memcpy(outCb + row * chromaWidth, cb + row * (m_codedWidth / 2), chromaWidth);
    }
    const u8* cr = cb + (m_codedWidth / 2) * (m_codedHeight / 2);
    u8* outCr = outCb + chromaWidth * chromaHeight;
    for (u32 row = 0; row < chromaHeight; row++)
    {
        ::std::memcpy(outCr + row * chromaWidth, cr + row * (m_codedWidth / 2), chromaWidth);
    }
}

// The next display-order frame (mpegs DecodeOrSkip frame reordering over
// the queue): anchors decode ahead and hold while their B pictures show
// first; the held anchor shows once completed or at the stream's end.
s32 Decoder::NextFrame(YuvFrame* frame)
{
    while (true)
    {
        ScanStream();
        if (!m_queued.empty())
        {
            // Pictures decode in coded order (the queue's order) and show in
            // display order: an anchor (I/P) is held until the next one
            // decodes, so the B pictures coded between them (which display
            // before it) go out first; a B shows as soon as it decodes,
            // both references being ready (its future anchor is coded before
            // it).
            QueuedPicture picture = m_queued.front();
            m_queued.erase(m_queued.begin());
            if (picture.info.pictureType == 3)
            {
                if (!m_havePast || !m_haveFuture)
                {
                    continue; // no references (a broken stream): skip it
                }
                if (!DecodeBPicture(&picture, frame))
                {
                    continue;
                }
                m_decoded++;
                return 1;
            }
            // The anchor just decoded takes the held slot; the one it
            // displaces has had every B after it shown and goes out now
            bool hadAnchor = m_haveAnchor;
            YuvFrame previous;
            if (hadAnchor)
            {
                previous = m_heldAnchor;
            }
            if (!DecodeAnchor(&picture))
            {
                continue;
            }
            if (hadAnchor)
            {
                *frame = previous;
                m_decoded++;
                return 1;
            }
            continue;
        }
        if (m_haveAnchor && (m_sequenceEnded || m_scan >= m_stream.size()))
        {
            *frame = m_heldAnchor;
            m_haveAnchor = false;
            m_decoded++;
            return 1;
        }
        if (m_sequenceEnded || m_scanError)
        {
            return -1;
        }
        return 0;
    }
}
}
