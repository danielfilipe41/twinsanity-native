// Movies: no player, nothing opens.
#include "platform/movie.h"

void Platform::Movie::Construct(Player* player)
{
    (void)player;
}

void Platform::Movie::BeginPresenting(Player* player, void (*present)())
{
    (void)player;
    (void)present;
}

bool Platform::Movie::Open(Player* player, const char* file, u32 audioChannel, s32 width, void* lentMemory)
{
    (void)player;
    (void)file;
    (void)audioChannel;
    (void)width;
    (void)lentMemory;
    return false;
}

void Platform::Movie::StartSound(Player* player, f32 volume)
{
    (void)player;
    (void)volume;
}

bool Platform::Movie::Step(Player* player)
{
    (void)player;
    return false;
}

void Platform::Movie::WaitFrame(Player* player)
{
    (void)player;
}

void Platform::Movie::QueuePicture(Player* player)
{
    (void)player;
}

void Platform::Movie::Draw(Player* player, const Area& area, s32 width, s32 height, u32 skippedRows)
{
    (void)player;
    (void)area;
    (void)width;
    (void)height;
    (void)skippedRows;
}

void Platform::Movie::Close(Player* player)
{
    (void)player;
}
