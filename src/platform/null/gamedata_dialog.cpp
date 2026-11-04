// No folder or file picker on this platform: a host built on the null layer has no dialog to offer, so the run's files come
// from the working directory, TWINSANITY_DATA or --disc (see gamedata.cpp).
#include "platform/gamedata.h"

bool Platform::GameData::PickFolder(const char*, char*, u32, void*)
{
    return false;
}

bool Platform::GameData::PickImage(const char*, char*, u32, void*)
{
    return false;
}
