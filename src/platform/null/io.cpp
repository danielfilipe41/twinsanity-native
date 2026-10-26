// I/O processor: there is none, so nothing runs behind it.
#include "platform/io.h"

void Platform::Io::Initialise()
{
}

void Platform::Io::Restart(const char* image)
{
    (void)image;
}

s32 Platform::Io::LoadDriver(const char* path)
{
    (void)path;
    return -1;
}

void Platform::Io::InitialiseHeap()
{
}

void* Platform::Io::AllocateHeap(s32 size)
{
    (void)size;
    return nullptr;
}

s32 Platform::Io::FreeHeap(void* address)
{
    (void)address;
    return -1;
}
