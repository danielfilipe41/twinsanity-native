// No folder picker on this platform: a host built on the null layer has no dialog to offer, so the run's folder comes from
// the working directory or TWINSANITY_DATA (see gamedata.cpp).
#include "platform/gamedata.h"

bool Platform::GameData::PickFolder(const char*, char*, u32)
{
    return false;
}
