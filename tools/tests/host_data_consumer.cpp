// Consumer for the host_data.py fixture test: links converted .rodata + a retail-layout vtable and dispatches.
#include "common.h"
#include "gcc2.h"

#include <cstdio>
#include <cstring>

struct Counter
{
    s32 value;
};

extern "C"
{
void Counter_Add(Counter* self, s32 amount) RETAIL(FUN_TEST_ADD);
void Counter_Get(const Counter* self, s32* out) RETAIL(FUN_TEST_GET);
extern const char D_TEST_STRING[];
extern const u32 D_TEST_WORDS[];
extern const GccVTableEntry g_FixtureVTable[];
}

void Counter_Add(Counter* self, s32 amount)
{
    self->value += amount;
}

void Counter_Get(const Counter* self, s32* out)
{
    *out = self->value;
}

int main()
{
    if (std::strcmp(D_TEST_STRING, "hello") != 0)
    {
        std::printf("FAIL string\n");
        return 1;
    }

    if (D_TEST_WORDS[0] != 0x12345678u)
    {
        std::printf("FAIL word\n");
        return 1;
    }

    if (g_FixtureVTable[0].function != nullptr || g_FixtureVTable[1].delta != 0)
    {
        std::printf("FAIL entry0\n");
        return 1;
    }

    Counter counter{40};
    CallVirtual<void>(&counter, g_FixtureVTable, 1, 2);
    s32 out = 0;
    CallVirtual<void>(&counter, g_FixtureVTable, 2, &out);
    if (counter.value != 42 || out != 42)
    {
        std::printf("FAIL dispatch %d %d\n", counter.value, out);
        return 1;
    }

    std::printf("converter end-to-end passed\n");
    return 0;
}
