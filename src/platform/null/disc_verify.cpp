// --verify-disc: the whole image's SHA-1 and CRC-32 against the dumps redump.org publishes, in one pass over the file.
//
// SHA-1 is FIPS 180-4 in its plain form; CRC-32 is the reflected 0xEDB88320 (the zlib / PKZIP one) with a table built once.
// Both stream, so the image is never held in memory; a 2.5 GB image takes a few seconds and the disc read is usually the
// slower half of it.
// A disc image is over 2 GB and the host build is 32 bits: without this the C library's file calls carry a 32-bit off_t and
// refuse to open the image at all (EOVERFLOW). It has to be set before any header of this file is read
#define _FILE_OFFSET_BITS 64

#include "platform/discverify.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace
{
// The releases of this disc that are known, from redump.org: the whole image's bytes, CRC-32 and SHA-1 (their disc pages'
// figures; no byte of any disc is in here)
struct KnownDump
{
    const char* serial;
    const char* region;
    const char* version;
    u64 bytes;
    u32 crc32;
    const char* sha1;
    // False for a demo: a part of the game on a disc of its own, not one of the four releases
    bool release;
};

constexpr KnownDump KnownDumps[] = {
    {"SLES-52568", "the European (PAL) release", "1.01", 2558263296u, 0x9055a7bcu,
     "7cd787a8cb35d1bcde63ad742746d2222813145e", true},
    {"SLUS-20909", "the North American release", "1.00", 2108096512u, 0x4a1487dfu,
     "0d0252b6656422b75c37f325cb0002705f13bada", true},
    {"SLUS-20909", "the North American release", "2.00", 2108489728u, 0x06c5bb4au,
     "4f1a56f633c1b500bf3e39a0df1fdc8f101018ff", true},
    {"SLPM-65801", "the Japanese release", "1.00", 1954578432u, 0xc563818bu,
     "53fd7c66e14fb3f24896164e5861b5a27858df36", true},
    // The demo discs: known dumps too, but a part of the game rather than one of its releases
    {"SLED-52574", "the European demo disc", "1.01", 1228832768u, 0x227f06f2u,
     "ba24fe72f623ed16bb23155cb3b0fb1e8b1e5cf5", false},
    {"SLUS-29101", "the North American demo disc", "1.00", 1228832768u, 0xb2da83c7u,
     "1d2a05d871c867223885f0a3a52f1ab07bd52658", false},
};

constexpr u32 Crc32TableCount = 256;
u32 g_CrcTable[Crc32TableCount] = {};

void BuildCrcTable()
{
    if (g_CrcTable[1] != 0)
    {
        return;
    }
    for (u32 index = 0; index < Crc32TableCount; index++)
    {
        u32 value = index;
        for (u32 bit = 0; bit < 8; bit++)
        {
            value = (value & 1u) != 0 ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        }
        g_CrcTable[index] = value;
    }
}

struct Crc32
{
    u32 state = 0xFFFFFFFFu;

    void Update(const u8* bytes, size_t length)
    {
        for (size_t index = 0; index < length; index++)
        {
            state = g_CrcTable[(state ^ bytes[index]) & 0xFFu] ^ (state >> 8);
        }
    }

    u32 Value() const { return ~state; }
};

struct Sha1
{
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u8 buffer[64] = {};
    size_t buffered = 0;
    u64 total = 0;

    static u32 Rotate(u32 value, u32 bits) { return (value << bits) | (value >> (32 - bits)); }

    void Block(const u8* block)
    {
        u32 words[80];
        for (u32 index = 0; index < 16; index++)
        {
            words[index] = static_cast<u32>(block[index * 4]) << 24 | static_cast<u32>(block[index * 4 + 1]) << 16 |
                           static_cast<u32>(block[index * 4 + 2]) << 8 | static_cast<u32>(block[index * 4 + 3]);
        }
        for (u32 index = 16; index < 80; index++)
        {
            words[index] = Rotate(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        }

        u32 a = h[0];
        u32 b = h[1];
        u32 c = h[2];
        u32 d = h[3];
        u32 e = h[4];
        for (u32 index = 0; index < 80; index++)
        {
            u32 f = 0;
            u32 k = 0;
            if (index < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            }
            else if (index < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (index < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const u32 temp = Rotate(a, 5) + f + e + k + words[index];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = temp;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void Update(const u8* bytes, size_t length)
    {
        total += length;
        while (length > 0)
        {
            const size_t room = sizeof(buffer) - buffered;
            const size_t take = length < room ? length : room;
            std::memcpy(buffer + buffered, bytes, take);
            buffered += take;
            bytes += take;
            length -= take;
            if (buffered == sizeof(buffer))
            {
                Block(buffer);
                buffered = 0;
            }
        }
    }

    void Finish(u8 digest[20])
    {
        const u64 bits = total * 8;
        const u8 padding = 0x80;
        Update(&padding, 1);
        const u8 zero = 0;
        while (buffered != 56)
        {
            Update(&zero, 1);
        }
        u8 length[8];
        for (u32 index = 0; index < 8; index++)
        {
            length[index] = static_cast<u8>(bits >> (56 - index * 8));
        }
        Update(length, 8);
        for (u32 index = 0; index < 5; index++)
        {
            digest[index * 4] = static_cast<u8>(h[index] >> 24);
            digest[index * 4 + 1] = static_cast<u8>(h[index] >> 16);
            digest[index * 4 + 2] = static_cast<u8>(h[index] >> 8);
            digest[index * 4 + 3] = static_cast<u8>(h[index]);
        }
    }
};
}

bool Platform::DiscVerify::Image(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
    {
        std::fprintf(stderr, "verify-disc: %s has the files as a folder, not as an image: there is nothing to hash\n", path);
        return false;
    }

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "verify-disc: could not open %s\n", path);
        return false;
    }

    BuildCrcTable();
    Crc32 crc;
    Sha1 sha;
    u64 bytes = 0;

    // A megabyte at a time: the image is never held whole
    static u8 buffer[1 << 20];
    while (true)
    {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read == 0)
        {
            break;
        }
        crc.Update(buffer, read);
        sha.Update(buffer, read);
        bytes += read;
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
    {
        std::fprintf(stderr, "verify-disc: could not read %s whole\n", path);
        return false;
    }

    u8 digest[20];
    sha.Finish(digest);
    char sha1[41] = {};
    for (u32 index = 0; index < 20; index++)
    {
        std::snprintf(sha1 + index * 2, 3, "%02x", digest[index]);
    }

    std::fprintf(stderr, "verify-disc: %llu bytes, CRC-32 %08x, SHA-1 %s\n", static_cast<unsigned long long>(bytes),
                 crc.Value(), sha1);

    // The dump it is, or the dump it is a modified copy of: the size is the half of the identity a patch leaves alone
    for (const KnownDump& known : KnownDumps)
    {
        if (known.bytes != bytes)
        {
            continue;
        }
        if (std::strcmp(known.sha1, sha1) == 0)
        {
            if (known.release)
            {
                std::fprintf(stderr, "verify-disc: %s (%s v%s), an original retail dump\n", known.region, known.serial,
                             known.version);
            }
            else
            {
                std::fprintf(stderr, "verify-disc: %s (%s v%s), an original demo dump: a part of the game, not one of the\n"
                                     "verify-disc: four releases the port plays\n",
                             known.region, known.serial, known.version);
            }
            return true;
        }
        std::fprintf(stderr, "verify-disc: the size of %s (%s v%s), but its bytes differ: a modified copy, which plays as the\n"
                             "verify-disc: original does\n",
                     known.region, known.serial, known.version);
        return true;
    }

    std::fprintf(stderr, "verify-disc: no known dump of this disc: %llu bytes is none of the four releases or the two demo\n"
                         "verify-disc: discs the port knows. It is played as it is\n",
                 static_cast<unsigned long long>(bytes));
    return true;
}
