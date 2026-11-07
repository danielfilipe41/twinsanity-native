// Start-up: like the null platform's (everything already runs), plus the sound processor's driver.
// The retail start-up (ps2/system.cpp:65) opens the sound driver with Sound::InitialiseRemote in its service startup; the
// host's must too, or Platform::Audio's SDL stream never opens and every sound plays to nothing.
#include "settings.h"

#include "graphics/bgfx_context.h"
#include "platform/sound.h"
#include "platform/system.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

void Platform::System::Initialise()
{
}

void Platform::System::StartServices()
{
    Sound::InitialiseRemote();
}

namespace
{
// The run's end as Exit asked for it, kept until the frame's end can act on it
s32 g_exitStatus = 0;
bool g_exitRequested = false;
}

void Platform::System::Exit(s32 status)
{
    // The options menu's exit row calls this from inside the game's frame, and bgfx's render thread is on the frame before this
    // one: the process is not ended here. What the process would have ended under is real -- std::exit runs the C library's exit
    // handlers, which unload the driver's libraries, and the thread still inside the driver faults on the state they freed (the
    // crash is a libgallium uniform upload reading a null pointer). The frame finishes, and its end ends the run
    g_exitStatus = status;
    g_exitRequested = true;
}

bool Platform::System::ExitRequested()
{
    return g_exitRequested;
}

[[noreturn]] void Platform::System::EndRun()
{
    // The frame is presented and nothing is queued behind it, so the renderer goes first: its thread is joined before the
    // process's libraries are unloaded, and SDL (the sound's stream) after it. The change the player made a moment ago may still
    // be waiting for the settings tick, so the file is kept before the process ends (the same write the tick would have done)
    HostSettingsKeep();
    BgfxShutdown();
    SDL_Quit();
    std::exit(g_exitStatus);
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

bool Platform::System::CutsceneSkip()
{
    return HostSettingsCutsceneSkip();
}

void Platform::System::SetCutsceneSkip(bool skip)
{
    HostSettingsSetCutsceneSkip(skip);
}

u32 Platform::System::CutsceneSkipSpeed()
{
    return HostSettingsCutsceneSpeed();
}

Platform::System::DateTime Platform::System::LocalTime()
{
    std::time_t now = std::time(nullptr);
    // localtime fills a buffer the C library shares between its callers, and the stream workers and the audio thread are
    // alive while the game stamps a save: the buffer is taken and read under a lock
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const std::tm* local = std::localtime(&now);
    if (local == nullptr)
    {
        return DateTime{};
    }

    DateTime dateTime;
    dateTime.year = static_cast<u16>(local->tm_year + 1900);
    dateTime.month = static_cast<u8>(local->tm_mon + 1);
    dateTime.day = static_cast<u8>(local->tm_mday);
    dateTime.hour = static_cast<u8>(local->tm_hour);
    dateTime.minute = static_cast<u8>(local->tm_min);
    dateTime.second = static_cast<u8>(local->tm_sec);
    return dateTime;
}
