// The retail program's data for a build that carries none of it (include/platform/retaildata.h).
//
// The build defines TWINSANITY_RUNTIME_DATA and links none of the split's data: the game's tables, strings and vtables
// are an image of zeroes (build/host/runtime/retail_data_symbols.s, one symbol per data symbol at its retail offset)
// until this file fills it from the player's own SLES_525.68, at the first start.
//
// The executable is the European release's, byte for byte (its SHA-1 is checked), because the symbols' addresses are
// that program's. Every 4 byte word of the copies is then read as a pointer: a word holding a code symbol's address is
// pointed at the port's own function for it (tools/retail_map.py's table), and a word holding an address inside the
// program's own data is pointed at the same place in the image. What is left is data.
#include "platform/retaildata.h"

#include <cstdio>

#if defined(TWINSANITY_RUNTIME_DATA)

#include "platform/files.h"
#include "retail_image.h"
#include "retail_symbols.h"

#include <cstring>
#include <vector>

extern "C" u8 __retail_image[];
extern "C" u8 __retail_image_end[];

namespace RetailImage
{
const Extent Image = {__retail_image, static_cast<u32>(__retail_image_end - __retail_image)};
}

namespace
{
// The disc's own executable, by the name at the disc's root
constexpr char Executable[] = "SLES_525.68";
// The European release's executable, the program the symbols' addresses and the split belong to. Another release's
// program has other addresses: its data would load at the wrong ones, so it is refused rather than half read
constexpr char ReleaseSha1[] = "d41b8d53f733f930cf03d8ea944bec24429fe57c";

constexpr u32 SectionAllocated = 0x2;
constexpr u32 SectionNoBytes = 8;

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

void Hex(const u8 digest[20], char out[41])
{
    for (u32 index = 0; index < 20; index++)
    {
        std::snprintf(out + index * 2, 3, "%02x", digest[index]);
    }
}

u16 Half(const u8* at, u32 offset)
{
    u16 value = 0;
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}

u32 Word(const u8* at, u32 offset)
{
    u32 value = 0;
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}

bool ReadWhole(const char* path, std::vector<u8>& bytes)
{
    const s32 file = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (file < 0)
    {
        return false;
    }

    const s32 size = Platform::Files::Seek(file, 0, Platform::Files::SeekEnd);
    if (size <= 0)
    {
        Platform::Files::Close(file);
        return false;
    }

    bytes.resize(static_cast<size_t>(size));
    s32 done = 0;
    while (done < size)
    {
        const s32 read = Platform::Files::ReadAt(file, static_cast<u32>(done), bytes.data() + done, size - done);
        if (read <= 0)
        {
            break;
        }
        done += read;
    }
    Platform::Files::Close(file);
    return done == size;
}

// The symbol a word holds, by its address: the list is sorted, and a word that is not one of them is data
const RetailSymbols::Symbol* FindSymbol(u32 address)
{
    u32 low = 0;
    u32 high = RetailSymbols::SymbolCount;
    while (low < high)
    {
        const u32 middle = (low + high) / 2;
        const RetailSymbols::Symbol& symbol = RetailSymbols::Symbols[middle];
        if (address < symbol.address)
        {
            high = middle;
        }
        else if (address > symbol.address)
        {
            low = middle + 1;
        }
        else
        {
            return &symbol;
        }
    }
    return nullptr;
}

// The function a word points into, when it is an offset from one rather than its start
const RetailSymbols::Relocation* FindRelocation(u32 word)
{
    u32 low = 0;
    u32 high = RetailSymbols::RelocationCount;
    while (low < high)
    {
        const u32 middle = (low + high) / 2;
        if (word < RetailSymbols::Relocations[middle].word)
        {
            high = middle;
        }
        else if (word > RetailSymbols::Relocations[middle].word)
        {
            low = middle + 1;
        }
        else
        {
            return &RetailSymbols::Relocations[middle];
        }
    }
    return nullptr;
}

// The port's function for a code symbol, or null when the port does not define it (a table entry nothing calls)
void* FindCode(u32 address)
{
    u32 low = 0;
    u32 high = RetailSymbols::HostCodeCount;
    while (low < high)
    {
        const u32 middle = (low + high) / 2;
        if (address < RetailSymbols::HostCodes[middle].address)
        {
            high = middle;
        }
        else if (address > RetailSymbols::HostCodes[middle].address)
        {
            low = middle + 1;
        }
        else
        {
            return RetailSymbols::HostCodes[middle].host;
        }
    }
    return nullptr;
}

bool g_Loaded = false;
}

bool Platform::RetailData::Baked()
{
    return false;
}

bool Platform::RetailData::Load(bool verbose)
{
    if (g_Loaded)
    {
        return true;
    }

    std::vector<u8> executable;
    if (!ReadWhole(Executable, executable))
    {
        std::fprintf(stderr, "retail-data: this build carries none of the game's data, and the disc in front of it has no "
                             "%s at its root to read it from: use the disc (or its image) this game's copy came from\n",
                     Executable);
        return false;
    }

    u8 digest[20];
    Sha1 hash;
    hash.Update(executable.data(), executable.size());
    hash.Finish(digest);
    char sha1[41] = {};
    Hex(digest, sha1);
    if (std::strcmp(sha1, ReleaseSha1) != 0)
    {
        std::fprintf(stderr, "retail-data: %s is not the European release's executable (SHA-1 %s, want %s): only that "
                             "release's program has the addresses this build's data is read through\n",
                     Executable, sha1, ReleaseSha1);
        return false;
    }

    u8* image = RetailImage::Image.base;
    if (RetailImage::Image.size != RetailSymbols::ImageSize)
    {
        std::fprintf(stderr, "retail-data: the image is %u bytes, the map's is %u: regenerate tools/retail_symbols.h\n",
                     RetailImage::Image.size, RetailSymbols::ImageSize);
        return false;
    }
    std::memset(image, 0, RetailImage::Image.size);

    const u8* elf = executable.data();
    const u32 shoff = Word(elf, 0x20);
    const u16 shentsize = Half(elf, 0x2E);
    const u16 shnum = Half(elf, 0x30);

    struct Copy
    {
        u32 address;
        u32 size;
    };
    static Copy copies[32];
    u32 copyCount = 0;
    u32 copiedBytes = 0;
    for (u32 index = 0; index < shnum; index++)
    {
        const u8* section = elf + shoff + index * static_cast<u32>(shentsize);
        if (shoff + (index + 1) * shentsize > executable.size())
        {
            break;
        }
        const u32 type = Word(section, 4);
        const u32 flags = Word(section, 8);
        const u32 address = Word(section, 12);
        const u32 offset = Word(section, 16);
        const u32 size = Word(section, 20);
        if ((flags & SectionAllocated) == 0 || type == SectionNoBytes || size == 0)
        {
            continue;
        }
        if (address < RetailSymbols::ImageBase || static_cast<u64>(address) + size > RetailSymbols::ImageEnd ||
            static_cast<u64>(offset) + size > executable.size() || copyCount >= 32)
        {
            continue;
        }

        std::memcpy(image + (address - RetailSymbols::ImageBase), elf + offset, size);
        copies[copyCount].address = address;
        copies[copyCount].size = size;
        copyCount++;
        copiedBytes += size;
    }

    if (copyCount == 0)
    {
        std::fprintf(stderr, "retail-data: %s has no data sections in the image's range: it is not this game's program\n",
                     Executable);
        return false;
    }

    // Every word: a code symbol's address becomes the port's function for it, and an address inside the data becomes the
    // same place in the image. The pointer's own addend (a pointer into a table, not at its start) is kept
    u32 codePointers = 0;
    u32 offsetPointers = 0;
    u32 dataPointers = 0;
    u32 undefined = 0;
    u32 dangling = 0;
    for (u32 copyIndex = 0; copyIndex < copyCount; copyIndex++)
    {
        u32* at = reinterpret_cast<u32*>(image + (copies[copyIndex].address - RetailSymbols::ImageBase));
        const u32 words = copies[copyIndex].size / 4;
        for (u32 index = 0; index < words; index++)
        {
            const u32 value = at[index];
            const RetailSymbols::Relocation* relocation = FindRelocation(copies[copyIndex].address + index * 4);
            if (relocation != nullptr && value >= relocation->symbol)
            {
                // A word the program made as a symbol's address (at an offset or not): the port's function when one is at
                // that address (a data label in the text names the same bytes as a function), the image's same place for
                // a data one
                const u32 addend = value - relocation->symbol;
                void* host = FindCode(relocation->symbol);
                if (host != nullptr)
                {
                    at[index] = static_cast<u32>(reinterpret_cast<uintptr_t>(host)) + addend;
                    if (addend == 0)
                    {
                        codePointers++;
                    }
                    else
                    {
                        offsetPointers++;
                    }
                    continue;
                }
                if (relocation->symbol >= RetailSymbols::ImageBase)
                {
                    at[index] = static_cast<u32>(reinterpret_cast<uintptr_t>(image + (relocation->symbol -
                                                                                       RetailSymbols::ImageBase))) +
                                addend;
                    dataPointers++;
                    continue;
                }
            }

            const RetailSymbols::Symbol* symbol = FindSymbol(value);
            if (symbol == nullptr || !symbol->code)
            {
                if (value >= 0x00100000u && value < RetailSymbols::ImageBase)
                {
                    dangling++;
                }
                continue;
            }

            void* host = FindCode(symbol->address);
            if (host == nullptr)
            {
                undefined++;
                continue;
            }

            at[index] = static_cast<u32>(reinterpret_cast<uintptr_t>(host)) + (value - symbol->address);
            codePointers++;
        }
    }

    if (verbose)
    {
        std::fprintf(stderr, "retail-data: %s %zu bytes (the European release's), %u stretches, %u bytes; %u code "
                             "pointers, %u into a function, %u data pointers, %u words left at code addresses the port "
                             "does not define, %u words at addresses inside a function\n",
                     Executable, executable.size(), copyCount, copiedBytes, codePointers, offsetPointers, dataPointers,
                     undefined, dangling);
    }
    if (undefined != 0)
    {
        std::fprintf(stderr, "retail-data: %u pointer word(s) name code the port has no function for; the tables holding "
                             "them are left as they were\n",
                     undefined);
    }

    g_Loaded = true;
    return true;
}

bool Platform::RetailData::Dump(const char* path)
{
    if (!g_Loaded)
    {
        std::fprintf(stderr, "retail-data: nothing has been loaded to dump\n");
        return false;
    }

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "retail-data: cannot write %s\n", path);
        return false;
    }
    const u32* words = reinterpret_cast<const u32*>(RetailImage::Image.base);
    for (u32 index = 0; index < RetailImage::Image.size / 4; index++)
    {
        std::fprintf(file, "%08X %08X\n", RetailSymbols::ImageBase + index * 4, words[index]);
    }
    std::fclose(file);
    return true;
}

#else

bool Platform::RetailData::Baked()
{
    return true;
}

bool Platform::RetailData::Load(bool)
{
    return true;
}

bool Platform::RetailData::Dump(const char*)
{
    std::fprintf(stderr, "retail-data: this build carries the data baked in, there is no image to dump\n");
    return false;
}

#endif
