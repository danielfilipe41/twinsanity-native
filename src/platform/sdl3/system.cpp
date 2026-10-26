// Start-up: like the null platform's (everything already runs), plus the sound processor's driver.
// The retail start-up (ps2/system.cpp:65) opens the sound driver with Sound::InitialiseRemote in its service startup; the
// host's must too, or Platform::Audio's SDL stream never opens and every sound plays to nothing.
#include "platform/sound.h"
#include "platform/system.h"

#include <cstdlib>
#include <ctime>

void Platform::System::Initialise()
{
}

void Platform::System::StartServices()
{
    Sound::InitialiseRemote();
}

[[noreturn]] void Platform::System::Exit(s32 status)
{
    std::exit(status);
}

Platform::System::ConsoleLanguage Platform::System::Language()
{
    return LanguageEnglish;
}

Platform::System::DateTime Platform::System::LocalTime()
{
    std::time_t now = std::time(nullptr);
    std::tm local = *std::localtime(&now);
    DateTime dateTime;
    dateTime.year = static_cast<u16>(local.tm_year + 1900);
    dateTime.month = static_cast<u8>(local.tm_mon + 1);
    dateTime.day = static_cast<u8>(local.tm_mday);
    dateTime.hour = static_cast<u8>(local.tm_hour);
    dateTime.minute = static_cast<u8>(local.tm_min);
    dateTime.second = static_cast<u8>(local.tm_sec);
    return dateTime;
}
