// C library: the retail functions the host build needs that neither the host's library nor the game's
// own code provides. The game's own libc (malloc, qsort, rand, the casing table) and libgcc2's runtime come from the PS2
// platform's libc.cpp, malloc.cpp and libm.cpp; here are their semantics in portable C++, by the same retail names, so the
// host links. memcpy/strlen/sprintf and friends come straight from the host's library (see include/retail/libc.h).
#include "common.h"

#include "game/math.h"
#include "retail/libc.h"

#include <cstdlib>

// The casing table's bytes: the retail data provides them (asm/data)
extern "C"
{
extern const u8 CasingTable[];
}

namespace
{
// How far a lower case letter is from its capital
constexpr s32 CaseDistance = 'a' - 'A';

// rand's linear congruential generator, and its results' bits (RAND_MAX)
constexpr u32 RandomMultiplier = 1103515245;
constexpr u32 RandomIncrement = 12345;
constexpr u32 RandomMax = 0x7FFFFFFF;

// qsort sorts fewer elements than this by insertion, takes its pivot from three elements when there are more, and from three
// medians of three past the other
constexpr u32 InsertionSortBelow = 7;
constexpr u32 MedianOfThreeAbove = 7;
constexpr u32 NintherAbove = 40;

using Compare = s32 (*)(const void*, const void*);

// qsort's ways of swapping elements (SWAPINIT, long being 64 bits): one long, longs, or bytes when the array or the size
// isn't a multiple of a long
enum SwapKind : s32
{
    SwapLong = 0,
    SwapLongs = 1,
    SwapBytes = 2,
};

// _LIB_VERSION's values
constexpr s32 IeeeLibrary = -1;
constexpr s32 SvidLibrary = 0;
// swapfunc: the size in bytes, taken as unsigned for the count of longs
void SwapRange(u8* first, u8* second, s32 size, s32 kind)
{
    if (kind <= SwapLongs)
    {
        u64* firstLong = reinterpret_cast<u64*>(first);
        u64* secondLong = reinterpret_cast<u64*>(second);
        s64 count = static_cast<u32>(size) / sizeof(u64);
        do
        {
            u64 kept = *firstLong;
            *firstLong++ = *secondLong;
            *secondLong++ = kept;
        } while (--count > 0);
    }
    else
    {
        s64 count = static_cast<u32>(size);
        do
        {
            u8 kept = *first;
            *first++ = *second;
            *second++ = kept;
        } while (--count > 0);
    }
}

void Swap(u8* first, u8* second, u32 size, s32 kind)
{
    if (kind == SwapLong)
    {
        u64 kept = *reinterpret_cast<u64*>(first);
        *reinterpret_cast<u64*>(first) = *reinterpret_cast<u64*>(second);
        *reinterpret_cast<u64*>(second) = kept;
    }
    else
    {
        SwapRange(first, second, static_cast<s32>(size), kind);
    }
}

u8* MedianOfThree(u8* a, u8* b, u8* c, Compare compare)
{
    return compare(a, b) < 0 ? (compare(b, c) < 0 ? b : (compare(a, c) < 0 ? c : a))
                             : (compare(b, c) > 0 ? b : (compare(a, c) < 0 ? a : c));
}

void InsertionSort(u8* items, u32 count, u32 size, Compare compare, s32 kind)
{
    for (u8* placed = items + size; placed < items + count * size; placed += size)
    {
        for (u8* item = placed; item > items && compare(item - size, item) > 0; item -= size)
        {
            Swap(item, item - size, size, kind);
        }
    }
}

// The table's byte for any int: negative chars read the bytes before it (the rodata's text before _ctype_)
u8 Casing(s32 character)
{
    return *reinterpret_cast<const u8*>(reinterpret_cast<u32>(CasingTable) + character);
}

// The bits of |x| (a float's magnitude): past which there's no result (|x| >= 88.72), of an infinity (exponent 255, which
// fdlibm takes for an infinity or a NaN, a number on the R5900), of 0.5 ln2 and 1.5 ln2 (the argument's reductions) and of
// 2^-28 (below it e^x is 1 + x)
constexpr u32 MagnitudeMask = 0x7FFFFFFF;
constexpr u32 LargeArgumentBits = 0x42B17217;
constexpr u32 InfinityBits = 0x7F800000;
constexpr u32 HalfLn2Bits = 0x3EB17218;
constexpr u32 ThreeHalvesLn2Bits = 0x3F851592;
constexpr u32 TinyArgumentBits = 0x31800000;
// k goes into the result's exponent, or k + 100 and the result times 2^-100 when k alone would leave the normal floats
constexpr s32 SmallestScale = -125;
constexpr s32 ScaleBias = 100;
constexpr u32 ExponentShift = 23;

// ERANGE
constexpr s32 RangeError = 34;

constexpr f32 Bits(u32 bits)
{
    return __builtin_bit_cast(f32, bits);
}

constexpr f32 Half[2] = {Bits(0x3F000000), Bits(0xBF000000)};
constexpr f32 Ln2High[2] = {Bits(0x3F317180), Bits(0xBF317180)};
constexpr f32 Ln2Low[2] = {Bits(0x3717F7D1), Bits(0xB717F7D1)};
constexpr f32 Huge = Bits(0x7149F2CA);
constexpr f32 TwoToMinus100 = Bits(0x0D800000);
constexpr f32 InverseLn2 = Bits(0x3FB8AA3B);
constexpr f32 P1 = Bits(0x3E2AAAAB);
constexpr f32 P2 = Bits(0xBB360B61);
constexpr f32 P3 = Bits(0x388AB355);
constexpr f32 P4 = Bits(0xB5DDEA0E);
constexpr f32 P5 = Bits(0x3331BB4C);
// The largest and smallest arguments with a result
constexpr f32 OverflowThreshold = Bits(0x42B17180);
constexpr f32 UnderflowThreshold = Bits(0xC2CFF1B5);
// What retail's compiler folded huge * huge to (an IEEE infinity, the R5900's 2^128)
constexpr f32 HugeSquared = Bits(0x7F800000);
// HUGE_VAL and SVID's HUGE as floats
constexpr f32 HugeValue = Bits(0x7F800000);
constexpr f32 SvidHuge = Bits(0x7F7FFFFF);

u32 BitsOf(f32 value)
{
    return __builtin_bit_cast(u32, value);
}
}

extern "C"
{
// _LIB_VERSION: X/Open's
extern const s32 MathLibraryVersion RETAIL(D_00307E80);
}

// The difference's sign bit: set below the infinity's bits
s32 IsFiniteFloat(f32 x)
{
    return static_cast<s32>(((BitsOf(x) & MagnitudeMask) - InfinityBits) >> 31);
}

f32 ExpFloatCore(f32 x)
{
    FloatBits bits;
    bits.value = BitsOf(x);
    s32 negative = static_cast<s32>(bits.sign);
    u32 magnitude = bits.value & MagnitudeMask;
    if (magnitude > LargeArgumentBits)
    {
        if (magnitude > InfinityBits)
        {
            return x + x;
        }

        if (magnitude == InfinityBits)
        {
            return negative == 0 ? x : 0.0f;
        }

        if (x > OverflowThreshold)
        {
            return HugeSquared;
        }

        if (x < UnderflowThreshold)
        {
            return 0.0f;
        }
    }

    // The argument as k * ln2 + high - low, |high - low| <= ln2 / 2
    s32 k;
    f32 high = 0.0f;
    f32 low = 0.0f;
    if (magnitude > HalfLn2Bits)
    {
        if (magnitude < ThreeHalvesLn2Bits)
        {
            high = x - Ln2High[negative];
            low = Ln2Low[negative];
            k = 1 - negative - negative;
        }
        else
        {
            k = static_cast<s32>(InverseLn2 * x + Half[negative]);
            f32 t = static_cast<f32>(k);
            high = x - t * Ln2High[0];
            low = t * Ln2Low[0];
        }

        x = high - low;
    }
    else if (magnitude < TinyArgumentBits)
    {
        if (Huge + x > 1.0f)
        {
            return 1.0f + x;
        }

        k = 0;
    }
    else
    {
        k = 0;
    }

    f32 t = x * x;
    f32 c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0)
    {
        return 1.0f - ((x * c) / (c - 2.0f) - x);
    }

    f32 y = 1.0f - ((low - (x * c) / (2.0f - c)) - high);
    if (k >= SmallestScale)
    {
        return Bits(BitsOf(y) + (static_cast<u32>(k) << ExponentShift));
    }

    return Bits(BitsOf(y) + (static_cast<u32>(k + ScaleBias) << ExponentShift)) * TwoToMinus100;
}

s32 RetailLibc::ToUpper(s32 character)
{
    return (Casing(character) & CasingLowerCase) != 0 ? character - CaseDistance : character;
}

s32 RetailLibc::ToLower(s32 character)
{
    return (Casing(character) & CasingUpperCase) != 0 ? character + CaseDistance : character;
}

void* RetailLibc::Malloc(u32 size)
{
    return std::malloc(size);
}

void RetailLibc::Free(void* memory)
{
    std::free(memory);
}

int RetailLibc::AtExit(void (*function)())
{
    AtExitBlock* block = g_Impure->atExit;
    if (block == nullptr)
    {
        block = &g_Impure->atExit0;
        g_Impure->atExit = block;
    }

    if (block->count >= AtExitBlockSize)
    {
        block = static_cast<AtExitBlock*>(Malloc(sizeof(AtExitBlock)));
        if (block == nullptr)
        {
            return -1;
        }

        block->count = 0;
        block->next = g_Impure->atExit;
        g_Impure->atExit = block;
    }

    // The function goes in before the count (a count of -1 puts it over the count)
    s32 index = block->count;
    *reinterpret_cast<void (**)()>(reinterpret_cast<u32>(block->functions) + sizeof(block->functions[0]) * index) = function;
    block->count = index + 1;
    return 0;
}

// qsort: Bentley and McIlroy's (4.4BSD's), insertion sort when a partition swapped nothing, recursing on the left part and
// iterating on the right one
void RetailLibc::QuickSort(void* items, u32 count, u32 size, Compare compare)
{
    u8* base = static_cast<u8*>(items);
    for (;;)
    {
        s32 kind = (reinterpret_cast<u32>(base) % sizeof(u64) != 0 || size % sizeof(u64) != 0) ? SwapBytes
                   : size == sizeof(u64)                                                       ? SwapLong
                                                                                               : SwapLongs;
        bool swapped = false;
        if (count < InsertionSortBelow)
        {
            InsertionSort(base, count, size, compare, kind);
            return;
        }

        u8* middle = base + (count / 2) * size;
        if (count > MedianOfThreeAbove)
        {
            u8* low = base;
            u8* high = base + (count - 1) * size;
            if (count > NintherAbove)
            {
                s32 step = static_cast<s32>((count / 8) * size);
                low = MedianOfThree(low, low + step, low + 2 * step, compare);
                middle = MedianOfThree(middle - step, middle, middle + step, compare);
                high = MedianOfThree(high - 2 * step, high - step, high, compare);
            }

            middle = MedianOfThree(low, middle, high, compare);
        }

        Swap(base, middle, size, kind);
        u8* lessEnd = base + size;
        u8* left = lessEnd;
        u8* right = base + (count - 1) * size;
        u8* greaterStart = right;
        for (;;)
        {
            s32 order;
            while (left <= right && (order = compare(left, base)) <= 0)
            {
                if (order == 0)
                {
                    swapped = true;
                    Swap(lessEnd, left, size, kind);
                    lessEnd += size;
                }

                left += size;
            }

            while (left <= right && (order = compare(right, base)) >= 0)
            {
                if (order == 0)
                {
                    swapped = true;
                    Swap(right, greaterStart, size, kind);
                    greaterStart -= size;
                }

                right -= size;
            }

            if (left > right)
            {
                break;
            }

            Swap(left, right, size, kind);
            swapped = true;
            left += size;
            right -= size;
        }

        if (!swapped)
        {
            InsertionSort(base, count, size, compare, kind);
            return;
        }

        // The equal elements at both ends go to the middle: min() of an int and of a size_t's difference compares unsigned
        u8* end = base + count * size;
        s32 moved = lessEnd - base < left - lessEnd ? lessEnd - base : left - lessEnd;
        if (moved > 0)
        {
            SwapRange(base, left - moved, moved, kind);
        }

        u32 equalHigh = greaterStart - right;
        u32 rest = static_cast<u32>(end - greaterStart) - size;
        moved = static_cast<s32>(equalHigh < rest ? equalHigh : rest);
        if (moved > 0)
        {
            SwapRange(left, end - moved, moved, kind);
        }

        s32 part = left - lessEnd;
        if (static_cast<u32>(part) > size)
        {
            QuickSort(base, static_cast<u32>(part) / size, size, compare);
        }

        part = greaterStart - right;
        if (static_cast<u32>(part) <= size)
        {
            return;
        }

        base = end - part;
        count = static_cast<u32>(part) / size;
    }
}

extern "C"
{
// __errno: errno is the reentrancy block's first word
s32* ErrorNumber() RETAIL(FUN_002c7040);

// rand: old newlib's 32 bit generator, its state in the reentrancy block
s32 GetRand()
{
    u32 next = g_Impure->randomNext * RandomMultiplier + RandomIncrement;
    g_Impure->randomNext = next;
    return static_cast<s32>(next & RandomMax);
}

// strncasecmp: the loop lowers the characters as signed chars, the result as unsigned ones
s32 CompareStrings(const char* first, const char* second, u32 count)
{
    if (count == 0)
    {
        return 0;
    }

    while (count-- != 0 && RetailLibc::ToLower(static_cast<s8>(*first)) == RetailLibc::ToLower(static_cast<s8>(*second)))
    {
        if (count == 0 || *first == '\0' || *second == '\0')
        {
            break;
        }

        first++;
        second++;
    }

    return RetailLibc::ToLower(static_cast<u8>(*first)) - RetailLibc::ToLower(static_cast<u8>(*second));
}

// abort
[[noreturn]] void Abort() RETAIL(FUN_002c79d8);

// expf: matherr (newlib's default) never handles an error, so X/Open's errno is always ERANGE
f32 Exponential(f32 x) RETAIL(FUN_002c1e98);

// The movie decoder's MPEG extension readers the retail headers' dispatch table calls. The decoder itself (its bit reader,
// its error reporting, the IPU registers they drive) is the movie port's; until it lands these keep the table linked.
// They are only reached from the decoder's extension parsing, which is not ported yet.
struct MpegSystem;
void ReadUnknownExtension(MpegSystem* sys) RETAIL(FUN_002c1060);
void ReadPictureCodingExtension(MpegSystem* sys) RETAIL(FUN_002c1070);
void ReadPictureTemporalScalableExtension(MpegSystem* sys) RETAIL(FUN_002c1268);

// The retail boot's EE functions the retail data's tables point at (its entry, the loader's copies and the system call
// table's set-up): PS2 start-up only, never run on the host.
void BootEntry() RETAIL(FUN_00100000);
void CopyWords() RETAIL(kCopy);
void FindSyscallAddress() RETAIL(kFindAddress);
void CopyWordsUncached() RETAIL(kCopy_002D5610);
void SetupSyscalls() RETAIL(FUN_002d5428);
void SetupLoader() RETAIL(func_002D5588);
void InitTlbFunctions() RETAIL(FUN_002d6828);
void RunLoader() RETAIL(FUN_002d6c00);
}

s32* ErrorNumber()
{
    return &g_Impure->errorNumber;
}

void Abort()
{
    std::abort();
}

// expf: matherr (newlib's default) never handles an error, so X/Open's errno is always ERANGE
f32 Exponential(f32 x)
{
    f32 result = ExpFloatCore(x);
    if (MathLibraryVersion == IeeeLibrary || IsFiniteFloat(x) == 0)
    {
        return result;
    }

    if (x > OverflowThreshold)
    {
        *ErrorNumber() = RangeError;
        return MathLibraryVersion == SvidLibrary ? SvidHuge : HugeValue;
    }

    if (x < UnderflowThreshold)
    {
        *ErrorNumber() = RangeError;
        return 0.0f;
    }

    return result;
}

void ReadUnknownExtension(MpegSystem*)
{
}

void ReadPictureCodingExtension(MpegSystem*)
{
}

void ReadPictureTemporalScalableExtension(MpegSystem*)
{
}

void BootEntry()
{
}

void CopyWords()
{
}

void FindSyscallAddress()
{
}

void CopyWordsUncached()
{
}

void SetupSyscalls()
{
}

void SetupLoader()
{
}

void InitTlbFunctions()
{
}

void RunLoader()
{
}
