// The C++ runtime's allocations, 16-byte aligned on this target.
//
// GCC takes operator new's result as aligned to std::max_align_t's 16 bytes and copies a struct into it with SSE's aligned
// moves (movaps); the i386 CRT's malloc hands back 8-byte aligned memory, so about half of those copies fault. The
// allocator below is the CRT's own aligned one, and every new/delete pair in the link goes through it: these definitions
// come before libstdc++'s, whose allocator members are then never pulled out of the archive.
#include "common.h"

#if defined(_WIN32)
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace
{
constexpr std::size_t AllocationAlignment = 16;

void* Allocate(std::size_t size)
{
    return ::_aligned_malloc(size != 0 ? size : 1, AllocationAlignment);
}
}

void* operator new(std::size_t size)
{
    void* memory = Allocate(size);
    if (memory == nullptr)
    {
        // The build has no exceptions; libstdc++'s operator new ends in abort() on a failed allocation here too
        std::abort();
    }
    return memory;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return Allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return Allocate(size);
}

void operator delete(void* memory) noexcept
{
    ::_aligned_free(memory);
}

void operator delete[](void* memory) noexcept
{
    ::_aligned_free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    ::_aligned_free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    ::_aligned_free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    ::_aligned_free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    ::_aligned_free(memory);
}
#endif
