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

Platform::System::ConsoleLanguage Platform::System::TextLanguage()
{
    // No settings of its own: the texts are in the language everything else is
    return Language();
}


// No choice of its own: the headless platform plays in english and the options screen shows no row for a language

u32 Platform::System::LanguageChoices()
{
    return 0;
}

const char* const* Platform::System::LanguageChoiceNames()
{
    return nullptr;
}

u32 Platform::System::LanguageChoice()
{
    return 0;
}

u32 Platform::System::TextLanguageChoice()
{
    return 0;
}

void Platform::System::SetLanguageChoice(u32)
{
}

void Platform::System::SetTextLanguageChoice(u32)
{
}

// Nothing keeps the choice here (no settings file): every cutscene is skippable
bool Platform::System::CutsceneSkip()
{
    return true;
}

void Platform::System::SetCutsceneSkip(bool)
{
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
