#define _FILE_OFFSET_BITS 64

// The disc image as a patch leaves it (platform/discpatch.h).
//
// A patch is read once, into the pieces it is made of -- the patch's own bytes, ranges of the disc image, zeroes, and ranges of
// what the patch has already made -- and those pieces are what the game reads as it asks for the image. Nothing is written
// anywhere: patching a 930 MB CRASH.BD with a patch that changes a text costs the text's bytes, which is all a page's memory
// can afford anyway.
#include "platform/discpatch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// 64-bit seeks on every host (a 32-bit `long` would wrap a 2.6 GB disc on Windows, and the host build is 32 bits)
#if defined(_WIN32)
#define PATCH_SEEK(file, offset, whence) _fseeki64((file), static_cast<__int64>(offset), (whence))
#define PATCH_TELL(file) _ftelli64(file)
#else
#define PATCH_SEEK(file, offset, whence) fseeko((file), static_cast<off_t>(offset), (whence))
#define PATCH_TELL(file) ftello(file)
#endif

namespace
{
// The patch's own bytes (what it adds and its runs, not what it copies from the image) are kept in memory: a page shares its
// with the game, so a patch holding more than this is refused rather than loaded and taking the game down with it
constexpr u64 MaxPatchBytes = 384ull << 20;
// The largest patch file this reads at all
constexpr u64 MaxFileBytes = 1ull << 30;

std::FILE* g_PatchFile = nullptr; // the patch, while it is read (its bytes are kept, its handle is not)
std::FILE* g_ImageFile = nullptr; // the disc image the patch is made over
u64 g_ImageBytes = 0;
u64 g_Size = 0;
u64 g_OwnBytes = 0;
bool g_Loaded = false;
bool g_Parsing = false;
std::string g_Label;

// A piece of the patched image
struct Piece
{
    enum class Kind
    {
        Literal, // the patch's own bytes
        Source,  // the disc image's, at `at`
        Target,  // the patched image's own, at `at` (always behind it: what the patch has made)
    };

    u64 start = 0;
    u64 length = 0;
    Kind kind = Kind::Literal;
    u64 at = 0;
    std::shared_ptr<std::vector<u8>> bytes;
};

std::vector<Piece> g_Pieces;

// ---- the image ---------------------------------------------------------------------------------------------------------

s32 RawImage(u64 offset, u8* out, s32 size)
{
    if (g_ImageFile == nullptr || size <= 0 || offset >= g_ImageBytes)
    {
        return 0;
    }
    s32 done = 0;
    while (done < size)
    {
        const u64 at = offset + static_cast<u64>(done);
        if (at >= g_ImageBytes)
        {
            break;
        }
        if (PATCH_SEEK(g_ImageFile, at, SEEK_SET) != 0)
        {
            break;
        }
        const u64 left = g_ImageBytes - at;
        const size_t wanted = std::min<u64>(static_cast<u64>(size - done), left);
        const size_t got = std::fread(out + done, 1, wanted, g_ImageFile);
        done += static_cast<s32>(got);
        if (got < wanted)
        {
            break;
        }
    }
    return done;
}

// The piece holding an offset (the pieces are in order and do not overlap: a binary search)
const Piece* PieceAt(u64 offset)
{
    size_t low = 0;
    size_t high = g_Pieces.size();
    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (offset < g_Pieces[middle].start)
        {
            high = middle;
        }
        else if (offset >= g_Pieces[middle].start + g_Pieces[middle].length)
        {
            low = middle + 1;
        }
        else
        {
            return &g_Pieces[middle];
        }
    }
    return nullptr;
}

u64 NextPieceStart(u64 offset)
{
    size_t low = 0;
    size_t high = g_Pieces.size();
    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (g_Pieces[middle].start <= offset)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < g_Pieces.size() ? g_Pieces[low].start : g_Size;
}

// The patched image's bytes: its pieces where it has one, the image's own between them
s32 ReadRange(u64 offset, u8* out, s32 size, u32 depth)
{
    if (depth > 8 || size <= 0 || offset >= g_Size)
    {
        return 0;
    }
    const u64 end = std::min<u64>(offset + static_cast<u64>(size), g_Size);
    s32 done = 0;
    while (offset + static_cast<u64>(done) < end)
    {
        const u64 at = offset + static_cast<u64>(done);
        const Piece* piece = PieceAt(at);
        if (piece == nullptr)
        {
            const u64 gap = std::min<u64>(NextPieceStart(at), end) - at;
            const s32 got = RawImage(at, out + done, static_cast<s32>(gap));
            if (got <= 0)
            {
                return done;
            }
            done += got;
            if (static_cast<u64>(got) < gap)
            {
                return done;
            }
            continue;
        }

        const u64 within = at - piece->start;
        const u64 want = std::min<u64>(piece->length - within, end - at);
        s32 got = 0;
        if (piece->kind == Piece::Kind::Literal)
        {
            std::memcpy(out + done, piece->bytes->data() + within, static_cast<size_t>(want));
            got = static_cast<s32>(want);
        }
        else if (piece->kind == Piece::Kind::Source)
        {
            got = RawImage(piece->at + within, out + done, static_cast<s32>(want));
        }
        else
        {
            got = ReadRange(piece->at + within, out + done, static_cast<s32>(want), depth + 1);
        }
        if (got <= 0)
        {
            return done;
        }
        done += got;
        if (static_cast<u64>(got) < want)
        {
            return done;
        }
    }
    return done;
}

// ---- the patch's own reading --------------------------------------------------------------------------------------------

struct Reader
{
    const std::vector<u8>* bytes = nullptr;
    size_t at = 0;

    bool Empty() const
    {
        return bytes == nullptr || at >= bytes->size();
    }

    bool Byte(u8* value)
    {
        if (bytes == nullptr || at >= bytes->size())
        {
            return false;
        }
        *value = (*bytes)[at++];
        return true;
    }

    bool Bytes(u32 count, const u8** data)
    {
        if (bytes == nullptr || count > bytes->size() - at)
        {
            return false;
        }
        *data = bytes->data() + at;
        at += count;
        return true;
    }

    // VCDIFF's integer: little-endian base 128, the top bit of a byte saying another follows
    bool Vcd(u64* value)
    {
        u64 result = 0;
        u32 shift = 0;
        while (true)
        {
            u8 byte = 0;
            if (!Byte(&byte) || shift > 63)
            {
                return false;
            }
            result |= static_cast<u64>(byte & 0x7F) << shift;
            shift += 7;
            if ((byte & 0x80) == 0)
            {
                break;
            }
        }
        *value = result;
        return true;
    }

    // BPS's integer: base 128 the other way round (a byte's top bit says the next is still added, and every step past the first
    // adds its own shift)
    bool Bps(u64* value)
    {
        u64 result = 0;
        u64 shift = 1;
        while (true)
        {
            u8 byte = 0;
            if (!Byte(&byte))
            {
                return false;
            }
            result += static_cast<u64>(byte & 0x7F) * shift;
            if ((byte & 0x80) != 0)
            {
                break;
            }
            shift <<= 7;
            if (shift == 0 || result + shift < result)
            {
                return false;
            }
            result += shift;
        }
        *value = result;
        return true;
    }
};

std::string g_Error;

bool Fail(const std::string& why)
{
    g_Error = why;
    return false;
}

// A piece of the image at `start`: its bytes are the patch's own (`bytes`), the image's at `at`, or the patched image's at `at`
bool AddPiece(Piece::Kind kind, u64 start, u64 length, u64 at, const std::shared_ptr<std::vector<u8>>& bytes)
{
    if (length == 0)
    {
        return true;
    }
    if (kind == Piece::Kind::Literal)
    {
        if (g_OwnBytes + length > MaxPatchBytes)
        {
            return Fail("the patch holds more of its own bytes than this port reads (384 MB): patch your disc image with "
                        "xdelta3, Delta Patcher or Flips, then play the patched image");
        }
        g_OwnBytes += length;
    }
    if (start + length < start || start + length > 0xFFFFFFFFull)
    {
        return Fail("the patch makes an image of more than 4 GB");
    }
    Piece piece;
    piece.start = start;
    piece.length = length;
    piece.kind = kind;
    piece.at = at;
    piece.bytes = bytes;
    g_Pieces.push_back(std::move(piece));
    return true;
}

std::shared_ptr<std::vector<u8>> LiteralBytes(const u8* data, u64 length)
{
    auto bytes = std::make_shared<std::vector<u8>>(static_cast<size_t>(length));
    if (length != 0)
    {
        std::memcpy(bytes->data(), data, static_cast<size_t>(length));
    }
    return bytes;
}

// The pieces in order, with none overlapping another: what the reads go by
bool FinishPieces(u64 size)
{
    std::sort(g_Pieces.begin(), g_Pieces.end(), [](const Piece& left, const Piece& right) { return left.start < right.start; });
    for (size_t index = 1; index < g_Pieces.size(); index++)
    {
        if (g_Pieces[index].start < g_Pieces[index - 1].start + g_Pieces[index - 1].length)
        {
            return Fail("the patch is damaged (two of its pieces change the same bytes)");
        }
    }
    g_Size = size;
    return size != 0;
}

// ---- xdelta3's VCDIFF ---------------------------------------------------------------------------------------------------

struct Code
{
    u8 type1 = 0, size1 = 0, mode1 = 0, type2 = 0, size2 = 0, mode2 = 0;
};

constexpr u8 Noop = 0, Add = 1, Run = 2, Copy = 3;

// The table VCDIFF defines by default (RFC 3284 §5.6), built rather than written out
void DefaultTable(Code* table)
{
    size_t at = 0;
    table[at++] = {Run, 0, 0, Noop, 0, 0};
    table[at++] = {Add, 0, 0, Noop, 0, 0};
    for (u32 size = 1; size <= 17; size++)
    {
        table[at++] = {Add, static_cast<u8>(size), 0, Noop, 0, 0};
    }
    for (u32 mode = 0; mode <= 8; mode++)
    {
        table[at++] = {Copy, 0, static_cast<u8>(mode), Noop, 0, 0};
        for (u32 size = 4; size <= 18; size++)
        {
            table[at++] = {Copy, static_cast<u8>(size), static_cast<u8>(mode), Noop, 0, 0};
        }
    }
    for (u32 mode = 0; mode <= 5; mode++)
    {
        for (u32 add = 1; add <= 4; add++)
        {
            for (u32 copy = 4; copy <= 6; copy++)
            {
                table[at++] = {Add, static_cast<u8>(add), 0, Copy, static_cast<u8>(copy), static_cast<u8>(mode)};
            }
        }
    }
    for (u32 mode = 6; mode <= 8; mode++)
    {
        for (u32 add = 1; add <= 4; add++)
        {
            table[at++] = {Add, static_cast<u8>(add), 0, Copy, 4, static_cast<u8>(mode)};
        }
    }
    for (u32 mode = 0; mode <= 8; mode++)
    {
        table[at++] = {Copy, 4, static_cast<u8>(mode), Add, 1, 0};
    }
}

u32 Adler32(u64 start, u64 length)
{
    // xdelta3's checksum of a window's own bytes: the RFC's two sums, kept under 65521
    u32 low = 1;
    u32 high = 0;
    std::vector<u8> bytes(1u << 20);
    for (u64 done = 0; done < length;)
    {
        const u64 chunk = std::min<u64>(bytes.size(), length - done);
        if (ReadRange(start + done, bytes.data(), static_cast<s32>(chunk), 0) != static_cast<s32>(chunk))
        {
            return 0;
        }
        for (u64 index = 0; index < chunk; index++)
        {
            low += bytes[static_cast<size_t>(index)];
            if (low >= 65521)
            {
                low -= 65521;
            }
            high += low;
            if (high >= 65521)
            {
                high %= 65521;
            }
        }
        done += chunk;
    }
    return (high << 16) | low;
}

bool ReadWindow(Reader& patch, u8 winIndicator, const Code* table)
{
    const bool fromSource = (winIndicator & 0x01) != 0;
    const bool fromTarget = (winIndicator & 0x02) != 0;
    if (fromSource && fromTarget)
    {
        return Fail("the patch is damaged (a window made over both the image and itself)");
    }
    if ((winIndicator & ~0x07u) != 0)
    {
        return Fail("the patch has a window flag this reader does not know");
    }

    u64 segmentLength = 0;
    u64 segmentPosition = 0;
    if (fromSource || fromTarget)
    {
        if (!patch.Vcd(&segmentLength) || !patch.Vcd(&segmentPosition))
        {
            return Fail("the patch ends inside a window");
        }
        if (fromSource && (segmentPosition > g_ImageBytes || segmentLength > g_ImageBytes - segmentPosition))
        {
            return Fail("the patch copies from past the end of your disc image: it was made for a larger one (another disc)");
        }
        if (fromTarget && (segmentPosition > g_Size || segmentLength > g_Size - segmentPosition))
        {
            return Fail("the patch is damaged (a copy from past the end of the image it makes)");
        }
    }

    u64 encodingLength = 0, targetLength = 0, dataLength = 0, instructionLength = 0, addressLength = 0;
    u8 deltaIndicator = 0;
    if (!patch.Vcd(&encodingLength) || !patch.Vcd(&targetLength) || !patch.Byte(&deltaIndicator) || !patch.Vcd(&dataLength) ||
        !patch.Vcd(&instructionLength) || !patch.Vcd(&addressLength))
    {
        return Fail("the patch ends inside a window");
    }
    (void)encodingLength;
    if (deltaIndicator != 0)
    {
        return Fail("the patch was made with xdelta3's secondary compression, which this port does not read: apply it to your "
                    "disc image with xdelta3 or Delta Patcher, then play the patched image");
    }
    // A window may make the whole image (its copies cost nothing): it is the sections, which are in memory, that are bounded
    if (targetLength > 0xFFFFFFFFull || dataLength > MaxPatchBytes || instructionLength > (64u << 20) ||
        addressLength > (64u << 20))
    {
        return Fail("the patch is damaged (a window no disc has)");
    }

    // xdelta3's Adler-32 of the window, when it wrote one (checked once the window is made): a patch of another disc is what
    // that catches
    u32 adler = 0;
    const bool checked = (winIndicator & 0x04) != 0;
    if (checked)
    {
        const u8* digest = nullptr;
        if (!patch.Bytes(4, &digest))
        {
            return Fail("the patch ends inside a window");
        }
        adler = (static_cast<u32>(digest[0]) << 24) | (static_cast<u32>(digest[1]) << 16) |
                (static_cast<u32>(digest[2]) << 8) | static_cast<u32>(digest[3]);
    }

    const u8* dataBytes = nullptr;
    const u8* instructionBytes = nullptr;
    const u8* addressBytes = nullptr;
    if (!patch.Bytes(static_cast<u32>(dataLength), &dataBytes))
    {
        return Fail("the patch ends inside a window");
    }
    const size_t dataStart = patch.at - dataLength;
    if (!patch.Bytes(static_cast<u32>(instructionLength), &instructionBytes))
    {
        return Fail("the patch ends inside a window");
    }
    const size_t instructionStart = patch.at - instructionLength;
    if (!patch.Bytes(static_cast<u32>(addressLength), &addressBytes))
    {
        return Fail("the patch ends inside a window");
    }

    std::vector<u8> data(patch.bytes->begin() + dataStart, patch.bytes->begin() + dataStart + dataLength);
    std::vector<u8> instructions(patch.bytes->begin() + instructionStart,
                                 patch.bytes->begin() + instructionStart + instructionLength);
    std::vector<u8> addresses(patch.bytes->begin() + (patch.at - addressLength), patch.bytes->begin() + patch.at);
    (void)dataBytes;
    (void)instructionBytes;
    (void)addressBytes;

    Reader dataReader;
    dataReader.bytes = &data;
    Reader instructionReader;
    instructionReader.bytes = &instructions;
    Reader addressReader;
    addressReader.bytes = &addresses;

    const u64 windowStart = g_Size;
    const u64 segment = segmentLength;
    u64 nearSlots[4] = {0, 0, 0, 0};
    std::vector<u64> sameSlots(3 * 256, 0);
    u32 nextNear = 0;
    u64 made = 0;

    while (!instructionReader.Empty())
    {
        u8 index = 0;
        if (!instructionReader.Byte(&index))
        {
            return Fail("the patch is damaged (an instruction code)");
        }
        const Code& code = table[index];
        const u8 types[2] = {code.type1, code.type2};
        const u8 sizes[2] = {code.size1, code.size2};
        const u8 modes[2] = {code.mode1, code.mode2};
        for (u32 half = 0; half < 2; half++)
        {
            if (types[half] == Noop)
            {
                continue;
            }
            u64 size = sizes[half];
            if (size == 0 && !instructionReader.Vcd(&size))
            {
                return Fail("the patch is damaged (an instruction's size)");
            }
            if (size > targetLength - made)
            {
                return Fail("the patch is damaged (an instruction past its window)");
            }
            if (types[half] == Add)
            {
                const u8* added = nullptr;
                if (size > 0xFFFFFFFFull || !dataReader.Bytes(static_cast<u32>(size), &added))
                {
                    return Fail("the patch is damaged (an addition past its data)");
                }
                if (!AddPiece(Piece::Kind::Literal, windowStart + made, size, 0, LiteralBytes(added, size)))
                {
                    return false;
                }
                made += size;
            }
            else if (types[half] == Run)
            {
                u8 value = 0;
                if (!dataReader.Byte(&value))
                {
                    return Fail("the patch is damaged (a run past its data)");
                }
                auto filled = std::make_shared<std::vector<u8>>();
                if (size > MaxPatchBytes)
                {
                    return Fail("the patch is damaged (a run longer than a disc)");
                }
                filled->assign(static_cast<size_t>(size), value);
                if (!AddPiece(Piece::Kind::Literal, windowStart + made, size, 0, filled))
                {
                    return false;
                }
                made += size;
            }
            else
            {
                const u64 here = segment + made;
                u64 address = 0;
                const u32 mode = modes[half];
                if (mode == 0)
                {
                    if (!addressReader.Vcd(&address))
                    {
                        return Fail("the patch is damaged (an address)");
                    }
                }
                else if (mode == 1)
                {
                    u64 distance = 0;
                    if (!addressReader.Vcd(&distance) || distance > here)
                    {
                        return Fail("the patch is damaged (an address)");
                    }
                    address = here - distance;
                }
                else if (mode < 6)
                {
                    u64 distance = 0;
                    if (!addressReader.Vcd(&distance) || nearSlots[mode - 2] + distance < nearSlots[mode - 2])
                    {
                        return Fail("the patch is damaged (an address)");
                    }
                    address = nearSlots[mode - 2] + distance;
                }
                else
                {
                    u8 slot = 0;
                    if (!addressReader.Byte(&slot))
                    {
                        return Fail("the patch is damaged (an address)");
                    }
                    address = sameSlots[(mode - 6) * 256 + slot];
                }
                nearSlots[nextNear] = address;
                nextNear = (nextNear + 1) % 4;
                sameSlots[address % (3 * 256)] = address;

                if (address >= here && size != 0)
                {
                    return Fail("the patch is damaged (a copy from ahead of itself)");
                }

                // The part in the segment the window names: the image's own, or the image the patch has already made
                u64 left = size;
                if (address < segment)
                {
                    const u64 portion = std::min<u64>(left, segment - address);
                    if (!AddPiece(fromSource ? Piece::Kind::Source : Piece::Kind::Target, windowStart + made, portion,
                                  segmentPosition + address, nullptr))
                    {
                        return false;
                    }
                    address += portion;
                    left -= portion;
                    made += portion;
                }
                // The part in this window's own data (which is the patched image's, at the place it has made it)
                while (left != 0)
                {
                    const u64 self = windowStart + (address - segment);
                    if (self >= windowStart + made)
                    {
                        return Fail("the patch is damaged (a copy from ahead of itself)");
                    }
                    const u64 portion = std::min<u64>(left, windowStart + made - self);
                    if (!AddPiece(Piece::Kind::Target, windowStart + made, portion, self, nullptr))
                    {
                        return false;
                    }
                    address += portion;
                    left -= portion;
                    made += portion;
                }
            }
        }
    }

    if (made != targetLength)
    {
        return Fail("the patch is damaged (a window does not add up)");
    }
    g_Size += targetLength;
    if (checked && Adler32(windowStart, targetLength) != adler)
    {
        return Fail("the patched image does not check out: the patch was made for a different disc image than yours");
    }
    return true;
}

bool ReadVcdiff(const std::vector<u8>& bytes)
{
    Reader patch;
    patch.bytes = &bytes;
    patch.at = 5;
    if (bytes.size() < 5)
    {
        return Fail("the patch is too short to be one");
    }
    if (bytes[3] != 0)
    {
        return Fail("this VCDIFF patch is of a version other than the standard one, which this port does not read");
    }
    const u8 indicator = bytes[4];
    if ((indicator & 0x01) != 0)
    {
        return Fail("the patch was made with xdelta3's secondary compression, which this port does not read: apply it to your "
                    "disc image with xdelta3 or Delta Patcher, then play the patched image");
    }
    if ((indicator & 0x02) != 0)
    {
        return Fail("the patch carries its own code table, which this port does not read: apply it to your disc image with "
                    "xdelta3 or Delta Patcher, then play the patched image");
    }
    if ((indicator & 0x04) != 0)
    {
        u64 length = 0;
        const u8* skipped = nullptr;
        if (!patch.Vcd(&length) || length > (1u << 20) || !patch.Bytes(static_cast<u32>(length), &skipped))
        {
            return Fail("the patch's header does not read");
        }
    }
    if ((indicator & ~0x07u) != 0)
    {
        return Fail("the patch's header has flags this reader does not know");
    }

    Code table[256];
    DefaultTable(table);
    u32 windows = 0;
    while (!patch.Empty())
    {
        u8 winIndicator = 0;
        if (!patch.Byte(&winIndicator))
        {
            break;
        }
        if (!ReadWindow(patch, winIndicator, table))
        {
            return false;
        }
        windows++;
    }
    if (windows == 0)
    {
        return Fail("the patch has no windows in it");
    }
    g_Label = "xdelta3 (VCDIFF)";
    return FinishPieces(g_Size);
}

// ---- PPF ----------------------------------------------------------------------------------------------------------------

bool ReadPpf(const std::vector<u8>& bytes)
{
    if (bytes.size() < 60 || std::memcmp(bytes.data(), "PPF", 3) != 0)
    {
        return Fail("it is not a PPF patch");
    }
    u32 version = 0;
    if (bytes[3] == '1' && bytes[4] == '0')
    {
        version = 1;
    }
    else if (bytes[3] == '2' && bytes[4] == '0')
    {
        version = 2;
    }
    else if (bytes[3] == '3' && bytes[4] == '0')
    {
        version = 3;
    }
    else
    {
        return Fail("this PPF patch is of a version this reader does not know");
    }

    u64 recordsAt = 0;
    bool undo = false;
    bool block = false;
    u64 blockAt = 0x9320;
    u64 imageBytes = 0;
    if (version == 1)
    {
        recordsAt = 56;
    }
    else if (version == 2)
    {
        if (bytes.size() < 1084)
        {
            return Fail("the PPF patch is damaged (shorter than its header)");
        }
        imageBytes = static_cast<u64>(bytes[56]) | (static_cast<u64>(bytes[57]) << 8) | (static_cast<u64>(bytes[58]) << 16) |
                    (static_cast<u64>(bytes[59]) << 24);
        block = true;
        recordsAt = 1084;
    }
    else
    {
        block = bytes[57] != 0;
        undo = bytes[58] != 0;
        blockAt = bytes[56] != 0 ? 0x80A0 : 0x9320;
        recordsAt = block ? 1084 : 60;
        if (bytes.size() < recordsAt)
        {
            return Fail("the PPF patch is damaged (shorter than its header)");
        }
    }

    if (imageBytes != 0 && imageBytes != g_ImageBytes)
    {
        return Fail("the patch was made for a disc image of " + std::to_string(imageBytes) + " bytes; yours has " +
                    std::to_string(g_ImageBytes) + ": it is for another disc");
    }
    if (block)
    {
        // The 1 KB of the image the patch records, so a patch of another disc is not applied to this one
        if (60 + 1024 > bytes.size())
        {
            return Fail("the PPF patch is damaged (its validation block)");
        }
        std::vector<u8> have(1024);
        if (blockAt >= g_ImageBytes || blockAt + have.size() > g_ImageBytes ||
            RawImage(blockAt, have.data(), static_cast<s32>(have.size())) != static_cast<s32>(have.size()))
        {
            return Fail("this PPF patch was made for a different disc image than yours");
        }
        if (std::memcmp(bytes.data() + 60, have.data(), have.size()) != 0)
        {
            return Fail("this PPF patch was made for a different disc image than yours (its check of the image fails)");
        }
    }

    // A record's offset is 8 bytes in a 3.0 patch. A 1.0 patch is written with 4-byte offsets by some tools and 5-byte ones by
    // others (the format's own notes say 5): the walk that lands exactly on the patch's end is the one read
    // A 3.0 patch ends with the size of the image it makes (4 bytes), which the records must not be read as
    auto walks = [&](size_t width, size_t trailing) {
        size_t cursor = static_cast<size_t>(recordsAt);
        if (bytes.size() < cursor + trailing)
        {
            return false;
        }
        const size_t end = bytes.size() - trailing;
        while (cursor < end)
        {
            if (end - cursor < width + 1)
            {
                return false;
            }
            const u32 length = bytes[cursor + width];
            cursor += width + 1;
            if (end - cursor < static_cast<size_t>(length) * (undo ? 2 : 1))
            {
                return false;
            }
            cursor += static_cast<size_t>(length) * (undo ? 2 : 1);
        }
        return cursor == end && cursor != static_cast<size_t>(recordsAt);
    };
    size_t offsetBytes = 0;
    size_t trailing = 0;
    const size_t widths[2] = {version == 3 ? 8u : 4u, version == 1 ? 5u : (version == 3 ? 8u : 4u)};
    for (size_t candidate = 0; candidate < 2 && offsetBytes == 0; candidate++)
    {
        for (size_t tail = 0; tail < 2; tail++)
        {
            const size_t bytesAfter = tail == 0 ? 0 : 4;
            if (walks(widths[candidate], bytesAfter))
            {
                offsetBytes = widths[candidate];
                trailing = bytesAfter;
                break;
            }
        }
    }
    if (offsetBytes == 0)
    {
        return Fail("the PPF patch is damaged (its records do not add up to its length)");
    }
    if (trailing != 0)
    {
        const size_t sizeAt = bytes.size() - trailing;
        u64 leaves = 0;
        for (size_t index = 0; index < trailing; index++)
        {
            leaves |= static_cast<u64>(bytes[sizeAt + index]) << (8 * index);
        }
        if (leaves != 0 && leaves != g_ImageBytes)
        {
            return Fail("the patch leaves an image of " + std::to_string(leaves) + " bytes; yours has " +
                        std::to_string(g_ImageBytes) + ": it is for another disc");
        }
    }

    const size_t recordsEnd = bytes.size() - trailing;
    size_t at = static_cast<size_t>(recordsAt);
    u32 records = 0;
    while (at < recordsEnd)
    {
        if (recordsEnd - at < offsetBytes + 1)
        {
            return Fail("the PPF patch is damaged (a record is cut short)");
        }
        u64 offset = 0;
        for (size_t index = 0; index < offsetBytes; index++)
        {
            offset |= static_cast<u64>(bytes[at + index]) << (8 * index);
        }
        const u32 count = bytes[at + offsetBytes];
        at += offsetBytes + 1;
        if (recordsEnd - at < static_cast<size_t>(count) + (undo ? count : 0))
        {
            return Fail("the PPF patch is damaged (a record is cut short)");
        }
        if (offset > g_ImageBytes || count > g_ImageBytes - offset)
        {
            return Fail("this PPF patch writes past the end of your disc image: it is for another disc");
        }
        if (!AddPiece(Piece::Kind::Literal, offset, count, 0, LiteralBytes(bytes.data() + at, count)))
        {
            return false;
        }
        at += static_cast<size_t>(count) + (undo ? count : 0);
        records++;
    }
    if (records == 0)
    {
        return Fail("the patch has no records in it");
    }
    g_Label = "PPF " + std::to_string(version) + ".0";
    return FinishPieces(g_ImageBytes);
}

// ---- BPS ----------------------------------------------------------------------------------------------------------------

bool ReadBps(const std::vector<u8>& bytes)
{
    if (bytes.size() < 19 || std::memcmp(bytes.data(), "BPS1", 4) != 0)
    {
        return Fail("it is not a BPS patch");
    }
    Reader reader;
    reader.bytes = &bytes;
    reader.at = 4;

    u64 sourceBytes = 0, targetBytes = 0, metadataBytes = 0;
    if (!reader.Bps(&sourceBytes) || !reader.Bps(&targetBytes) || !reader.Bps(&metadataBytes))
    {
        return Fail("the BPS patch is damaged (its header)");
    }
    if (sourceBytes != g_ImageBytes)
    {
        return Fail("the patch was made for a disc image of " + std::to_string(sourceBytes) + " bytes; yours has " +
                    std::to_string(g_ImageBytes) + ": it is for another disc");
    }
    const u8* metadata = nullptr;
    if (metadataBytes > (1u << 20) || !reader.Bytes(static_cast<u32>(metadataBytes), &metadata))
    {
        return Fail("the BPS patch is damaged (its header)");
    }
    if (targetBytes > 0xFFFFFFFFull)
    {
        return Fail("the patch makes an image of more than 4 GB");
    }

    // The actions run to the end of the patch bar its three trailing checksums
    Reader actions;
    actions.bytes = &bytes;
    actions.at = reader.at;
    const size_t actionsEnd = bytes.size() - 12;
    u64 made = 0;
    u64 sourceAt = 0;
    u64 sourceRelative = 0;
    while (actions.at < actionsEnd)
    {
        u64 packed = 0;
        if (!actions.Bps(&packed))
        {
            return Fail("the BPS patch is damaged (an action)");
        }
        const u32 type = static_cast<u32>(packed & 0x03);
        u64 length = (packed >> 2) + 1;
        if (made + length > targetBytes)
        {
            return Fail("the patch is damaged (an action past the image it makes)");
        }
        if (type == 0)
        {
            if (sourceAt > g_ImageBytes || length > g_ImageBytes - sourceAt)
            {
                return Fail("the patch copies from past the end of your disc image: it is for another disc");
            }
            if (!AddPiece(Piece::Kind::Source, made, length, sourceAt, nullptr))
            {
                return false;
            }
            sourceAt += length;
        }
        else if (type == 1)
        {
            const u8* literal = nullptr;
            if (length > 0xFFFFFFFFull || actions.at + length > actionsEnd ||
                !actions.Bytes(static_cast<u32>(length), &literal))
            {
                return Fail("the BPS patch is damaged (its own bytes are cut short)");
            }
            if (!AddPiece(Piece::Kind::Literal, made, length, 0, LiteralBytes(literal, length)))
            {
                return false;
            }
        }
        else if (type == 2)
        {
            u64 offset = 0;
            if (!actions.Bps(&offset) || offset > made || length > made - offset)
            {
                return Fail("the BPS patch is damaged (a copy from ahead of itself)");
            }
            if (!AddPiece(Piece::Kind::Target, made, length, made - offset, nullptr))
            {
                return false;
            }
        }
        else
        {
            u64 offset = 0;
            if (!actions.Bps(&offset))
            {
                return Fail("the BPS patch is damaged (a copy of the image)");
            }
            // BPS's own cursor for these copies (its sequential one is another): the step is half the value, plus one, and
            // the low bit says which way
            const u64 distance = (offset >> 1) + 1;
            if ((offset & 1) != 0)
            {
                if (distance > sourceRelative)
                {
                    return Fail("the BPS patch is damaged (a copy from before the image)");
                }
                sourceRelative -= distance;
            }
            else
            {
                sourceRelative += distance;
            }
            if (sourceRelative > g_ImageBytes || length > g_ImageBytes - sourceRelative)
            {
                return Fail("the patch copies from past the end of your disc image: it is for another disc");
            }
            if (!AddPiece(Piece::Kind::Source, made, length, sourceRelative, nullptr))
            {
                return false;
            }
            sourceRelative += length;
        }
        made += length;
    }
    if (made != targetBytes)
    {
        return Fail("the BPS patch does not make the image it says it does");
    }
    g_Label = "BPS";
    return FinishPieces(targetBytes);
}
}

bool Platform::DiscPatch::Load(const char* patch, const char* image)
{
    Forget();
    if (patch == nullptr || patch[0] == '\0' || image == nullptr || image[0] == '\0')
    {
        return false;
    }

    g_PatchFile = std::fopen(patch, "rb");
    if (g_PatchFile == nullptr)
    {
        std::fprintf(stderr, "discpatch: %s does not open: no patch\n", patch);
        Forget();
        return false;
    }
    g_ImageFile = std::fopen(image, "rb");
    if (g_ImageFile == nullptr)
    {
        std::fprintf(stderr, "discpatch: %s does not open: no patch\n", image);
        Forget();
        return false;
    }
    g_Parsing = true;

    // The image's own size, and the sync pattern that says it is a raw dump rather than a plain ISO
    if (PATCH_SEEK(g_ImageFile, 0, SEEK_END) != 0)
    {
        Forget();
        return false;
    }
    const s64 imageEnd = PATCH_TELL(g_ImageFile);
    if (imageEnd <= 0)
    {
        std::fprintf(stderr, "discpatch: %s does not read: no patch\n", image);
        Forget();
        return false;
    }
    g_ImageBytes = static_cast<u64>(imageEnd);
    u8 header[16] = {};
    std::rewind(g_ImageFile);
    if (std::fread(header, 1, sizeof(header), g_ImageFile) == sizeof(header))
    {
        static const u8 Sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        if (std::memcmp(header, Sync, sizeof(Sync)) == 0)
        {
            std::fprintf(stderr, "discpatch: %s is a raw dump, whose patch's offsets are its sectors' own: only a plain ISO's "
                                 "patch is read, so no patch\n",
                         image);
            Forget();
            return false;
        }
    }

    // The patch, whole
    std::vector<u8> bytes;
    if (PATCH_SEEK(g_PatchFile, 0, SEEK_END) != 0)
    {
        Forget();
        return false;
    }
    const s64 patchEnd = PATCH_TELL(g_PatchFile);
    if (patchEnd <= 0 || static_cast<u64>(patchEnd) > MaxFileBytes)
    {
        std::fprintf(stderr, "discpatch: %s is not a patch this reader takes (it is larger than 1 GB)\n", patch);
        Forget();
        return false;
    }
    bytes.resize(static_cast<size_t>(patchEnd));
    std::rewind(g_PatchFile);
    if (std::fread(bytes.data(), 1, bytes.size(), g_PatchFile) != bytes.size())
    {
        std::fprintf(stderr, "discpatch: %s does not read\n", patch);
        Forget();
        return false;
    }

    bool ok = false;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "BPS1", 4) == 0)
    {
        ok = ReadBps(bytes);
    }
    else if (bytes.size() >= 5 && static_cast<u8>(bytes[0]) == 0xD6 && static_cast<u8>(bytes[1]) == 0xC3 &&
             static_cast<u8>(bytes[2]) == 0xC4)
    {
        ok = ReadVcdiff(bytes);
    }
    else if (bytes.size() >= 5 && std::memcmp(bytes.data(), "PPF", 3) == 0)
    {
        ok = ReadPpf(bytes);
    }
    else
    {
        Fail("it is not a patch this port reads (an xdelta3/VCDIFF, PPF or BPS one)");
    }

    if (!ok)
    {
        std::fprintf(stderr, "discpatch: %s: %s\n", patch, g_Error.c_str());
        Forget();
        return false;
    }

    if (g_PatchFile != nullptr)
    {
        std::fclose(g_PatchFile);
        g_PatchFile = nullptr;
    }
    g_Loaded = true;
    g_Parsing = false;
    std::fprintf(stderr, "discpatch: %s: a %s patch, %llu %s, the image %llu bytes (yours %llu)%s\n", patch,
                 g_Label.c_str(), static_cast<unsigned long long>(g_Pieces.size()),
                 g_Pieces.size() == 1 ? "piece" : "pieces", static_cast<unsigned long long>(g_Size),
                 static_cast<unsigned long long>(g_ImageBytes),
                 g_Size == g_ImageBytes ? "" : ", its size as the patch leaves it");
    return true;
}

void Platform::DiscPatch::Forget()
{
    g_Pieces.clear();
    if (g_PatchFile != nullptr)
    {
        std::fclose(g_PatchFile);
        g_PatchFile = nullptr;
    }
    if (g_ImageFile != nullptr)
    {
        std::fclose(g_ImageFile);
        g_ImageFile = nullptr;
    }
    g_ImageBytes = 0;
    g_Size = 0;
    g_OwnBytes = 0;
    g_Loaded = false;
    g_Parsing = false;
    g_Label.clear();
    g_Error.clear();
}

bool Platform::DiscPatch::Loaded()
{
    return g_Loaded;
}

const char* Platform::DiscPatch::Label()
{
    return g_Label.c_str();
}

u64 Platform::DiscPatch::Size()
{
    return g_Loaded ? g_Size : 0;
}

s32 Platform::DiscPatch::ReadAt(u64 offset, void* buffer, s32 size)
{
    if (!g_Loaded || buffer == nullptr || size <= 0)
    {
        return 0;
    }
    return ReadRange(offset, static_cast<u8*>(buffer), size, 0);
}
