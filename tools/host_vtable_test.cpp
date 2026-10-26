// Host unit test: the data converter's contract. A converted retail vtable is an array of GccVTableEntry whose
// function fields are relocations the host linker resolves against the C++ functions' RETAIL() labels; CallVirtual
// dispatches through them (plain path) and through Abi::CallEabi (mixed int+float path, direct call off-MIPS).
// Run: g++ -m32 -malign-double -Iinclude -Iinclude/host tools/host_vtable_test.cpp -o /tmp/host_vtable_test && /tmp/host_vtable_test
#include "common.h"
#include "gcc2.h"

#include <cstdio>

struct Counter
{
    s32 value;
};

extern "C"
{
void Counter_Add(Counter* self, s32 amount) RETAIL(FUN_TEST_ADD);
void Counter_Get(const Counter* self, s32* out) RETAIL(FUN_TEST_GET);
f32 Counter_Mix(void* self, s32 a, f32 b, s32 c) RETAIL(FUN_TEST_MIX);
}

void Counter_Add(Counter* self, s32 amount)
{
    self->value += amount;
}

void Counter_Get(const Counter* self, s32* out)
{
    *out = self->value;
}

f32 Counter_Mix(void*, s32 a, f32 b, s32 c)
{
    return static_cast<f32>(a) + b + static_cast<f32>(c);
}

// The converted .data: entry 0 empty (no RTTI), then the slots
const GccVTableEntry g_TestVTable[] = {
    {0, 0, nullptr},
    {0, 0, reinterpret_cast<void*>(Counter_Add)},
    {0, 0, reinterpret_cast<void*>(Counter_Get)},
    {0, 0, reinterpret_cast<void*>(Counter_Mix)},
};

int main()
{
    Counter counter{40};
    CallVirtual<void>(&counter, g_TestVTable, 1, 2);
    s32 out = 0;
    CallVirtual<void>(&counter, g_TestVTable, 2, &out);
    f32 mixed = CallVirtual<f32>(&counter, g_TestVTable, 3, 1, 2.5f, 3);
    if (counter.value != 42 || out != 42 || mixed < 6.49f || mixed > 6.51f)
    {
        std::printf("FAIL: value=%d out=%d mixed=%f\n", counter.value, out, static_cast<double>(mixed));
        return 1;
    }

    std::printf("vtable test passed\n");
    return 0;
}
