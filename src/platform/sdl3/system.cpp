// Start-up: like the null platform's (everything already runs), plus the sound processor's driver.
// The retail start-up (ps2/system.cpp:65) opens the sound driver with Sound::InitialiseRemote in its service startup; the
// host's must too, or Platform::Audio's SDL stream never opens and every sound plays to nothing.
#include "settings.h"

#include "platform/sound.h"
#include "platform/system.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    // The options menu's exit row ends the process from inside the game: the change the player made a moment ago may still be
    // waiting for the settings tick, so the file is kept before the process ends (the same write the tick would have done)
    HostSettingsKeep();
    std::exit(status);
}

Platform::System::ConsoleLanguage Platform::System::Language()
{
    // The player's choice from the settings file (its `language` line), checked against the disc it is playing: the game puts
    // it in place once, as it starts
    return HostSettingsLanguage();
}

Platform::System::ConsoleLanguage Platform::System::TextLanguage()
{
    // The settings file's `text_language` line, when it names one the speech isn't: a disc whose texts were patched into
    // another slot keeps its voices where they were
    return HostSettingsTextLanguage();
}

// The five the disc carries, in the order the settings file's words and the options screen's row take them
u32 Platform::System::LanguageChoices()
{
    return HostSettingsLanguageChoices();
}

const char* const* Platform::System::LanguageChoiceNames()
{
    return HostSettingsLanguageChoiceNames();
}

u32 Platform::System::LanguageChoice()
{
    return HostSettingsLanguageChoice();
}

u32 Platform::System::TextLanguageChoice()
{
    return HostSettingsTextLanguageChoice();
}

// Kept in the settings file, for the next start: the voices are streamed banks the game opened at boot and its texts were read
// into memory then
void Platform::System::SetLanguageChoice(u32 index)
{
    HostSettingsSetLanguageChoice(index);
}

void Platform::System::SetTextLanguageChoice(u32 index)
{
    HostSettingsSetTextLanguageChoice(index);
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
