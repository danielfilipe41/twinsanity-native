// Memory: the pools come from the host's heap.
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
    return std::malloc(size);
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
