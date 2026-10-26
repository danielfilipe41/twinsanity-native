// Disc: always ready, backed by the host's files.
#include "platform/disc.h"

s32 Platform::Disc::Initialise()
{
    return 1;
}

s32 Platform::Disc::SetMedia(Media media)
{
    (void)media;
    return 0;
}

Platform::Disc::Readiness Platform::Disc::WaitReady()
{
    return Ready;
}
