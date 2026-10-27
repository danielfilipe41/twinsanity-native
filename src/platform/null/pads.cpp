// Controllers: none connected, every pad stays missing.
#include "platform/pads.h"

// Pad 1's test-held buttons (src/platform/ps2/pads.cpp:27, applied to pad 1's report there; debug.cpp writes them): none
// held on the null platform, whose pads stay missing.
extern "C"
{
    volatile u32 g_TestPadButtons = 0;
}

s32 Platform::Pads::Initialise()
{
    return 0;
}

s32 Platform::Pads::Open(s32 port, s32 slot, void* buffer)
{
    (void)port;
    (void)slot;
    (void)buffer;
    return 0;
}

s32 Platform::Pads::Close(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return 0;
}

s32 Platform::Pads::Read(s32 port, s32 slot, u8* data)
{
    (void)port;
    (void)slot;
    (void)data;
    return 0;
}

Platform::Pads::State Platform::Pads::GetState(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return StateDisconnected;
}

Platform::Pads::RequestState Platform::Pads::GetRequestState(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return RequestComplete;
}

s32 Platform::Pads::InfoMode(s32 port, s32 slot, ModeInfo info, s32 index)
{
    (void)port;
    (void)slot;
    (void)info;
    (void)index;
    return 0;
}

s32 Platform::Pads::SetMainMode(s32 port, s32 slot, MainMode mode, ModeLock lock)
{
    (void)port;
    (void)slot;
    (void)mode;
    (void)lock;
    return 0;
}

s32 Platform::Pads::InfoActuator(s32 port, s32 slot, s32 actuator, s32 term)
{
    (void)port;
    (void)slot;
    (void)actuator;
    (void)term;
    return 0;
}

s32 Platform::Pads::SetActuatorAlign(s32 port, s32 slot, const u8 align[6])
{
    (void)port;
    (void)slot;
    (void)align;
    return 0;
}

s32 Platform::Pads::SetActuatorDirect(s32 port, s32 slot, const u8 values[6])
{
    (void)port;
    (void)slot;
    (void)values;
    return 0;
}

s32 Platform::Pads::InfoPressureMode(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return 0;
}

s32 Platform::Pads::EnterPressureMode(s32 port, s32 slot)
{
    (void)port;
    (void)slot;
    return 0;
}
