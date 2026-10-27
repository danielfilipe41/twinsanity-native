// Memory: the pools come from the host's heap, zeroed.
#include "platform/memory.h"

#include <cstdlib>

namespace
{
constexpr u32 PoolBytes = 32u * 1024u * 1024u;
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
    return std::calloc(size != 0 ? size : 1, 1);
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
