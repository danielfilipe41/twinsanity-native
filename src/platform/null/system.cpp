// Start-up: everything already runs, so set-up succeeds at once.
#include "platform/system.h"

#include <cstdlib>
#include <ctime>

void Platform::System::Initialise()
{
}

void Platform::System::StartServices()
{
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
