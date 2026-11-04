// Memory: the pools come from the host's heap, zeroed.
#include "platform/memory.h"

#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
constexpr u32 PoolBytes = 32u * 1024u * 1024u;
// The pool's alignment: the game's objects hold 16-byte-aligned members (Matrix4x4 and the other quadword types the VU reads
// as one 128-bit load) that the compiler copies with SSE's aligned instructions, and the heap carves the pool in 16-byte
// units, so a 16-byte-aligned pool keeps every block it hands out aligned. The i386 CRT's calloc gives 8-byte alignment where
// the POSIX hosts' malloc gives 16, hence the explicit request on Windows.
constexpr u32 PoolAlignment = 16;
}

u32 Platform::Memory::PoolSpace()
{
    return PoolBytes;
}

void* Platform::Memory::AllocatePool(u32 size)
{
    // Cleared, as the PS2's pool is: it sits past the executable's BSS, which the loader zeroes, and the game reads fields back
    // before it writes them (an empty waypoint array's first slot is read as a path's address, a packet's unused arguments, a
    // joint's matrix). A host malloc hands back whatever the last owner of those bytes left there, and a stale pointer that
    // happens to be readable sends those reads somewhere plausible and wrong
    const u32 bytes = size != 0 ? size : 1;
#if defined(_WIN32)
    void* memory = ::_aligned_malloc(bytes, PoolAlignment);
    if (memory != nullptr)
    {
        ::memset(memory, 0, bytes);
    }
    return memory;
#else
    return std::calloc(bytes, 1);
#endif
}

void Platform::Memory::Synchronise()
{
}

void Platform::Memory::BeforeDeviceWrite(void* memory, u32 size)
{
    (void)memory;
    (void)size;
}

void Platform::Memory::WriteBackCache()
{
}
