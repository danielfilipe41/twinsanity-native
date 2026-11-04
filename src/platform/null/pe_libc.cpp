// The retail libc calls on i686 PE. include/retail/libc.h names them as the retail binary did -- asm("memcpy"), asm("strlen")
// and so on -- so the objects reference those names unadorned, while the PE CRT exports them with a leading underscore. These
// forwarders define the unadorned names the linker asks for, each calling the CRT's own function. Linux and the other hosts
// link the unadorned names directly and need none of this.
#include "common.h"

#if defined(_WIN32)
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C"
{
void* LinkerMemoryCopy(void* destination, const void* source, size_t size) asm("memcpy");
void* LinkerMemoryMove(void* destination, const void* source, size_t size) asm("memmove");
void* LinkerMemorySet(void* destination, int value, size_t size) asm("memset");
int LinkerFormatPrint(char* buffer, const char* format, ...) asm("sprintf");
char* LinkerStringCat(char* destination, const char* source) asm("strcat");
int LinkerStringCompare(const char* left, const char* right) asm("strcmp");
char* LinkerStringCopy(char* destination, const char* source) asm("strcpy");
size_t LinkerStringLength(const char* text) asm("strlen");
char* LinkerStringCopyCount(char* destination, const char* source, size_t count) asm("strncpy");

void* LinkerMemoryCopy(void* destination, const void* source, size_t size)
{
    return ::memcpy(destination, source, size);
}

void* LinkerMemoryMove(void* destination, const void* source, size_t size)
{
    return ::memmove(destination, source, size);
}

void* LinkerMemorySet(void* destination, int value, size_t size)
{
    return ::memset(destination, value, size);
}

int LinkerFormatPrint(char* buffer, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int length = ::vsprintf(buffer, format, arguments);
    va_end(arguments);
    return length;
}

char* LinkerStringCat(char* destination, const char* source)
{
    return ::strcat(destination, source);
}

int LinkerStringCompare(const char* left, const char* right)
{
    return ::strcmp(left, right);
}

char* LinkerStringCopy(char* destination, const char* source)
{
    return ::strcpy(destination, source);
}

size_t LinkerStringLength(const char* text)
{
    return ::strlen(text);
}

char* LinkerStringCopyCount(char* destination, const char* source, size_t count)
{
    return ::strncpy(destination, source, count);
}
}
#endif
