// The host's own settings file, beside the saves: the options a player chose survive a restart, which the game's own save only
// does once a save has been written and loaded (the front end at boot has none). The values are the game's own - its four
// volume groups, the music's stereo and the screen's widescreen - applied at the sound processor's first reset (that is where
// the game puts its defaults) and written back as they change.
#pragma once

#include "common.h"

#include "platform/display.h"
#include "platform/system.h"

// The sound processor's first reset: put the file's options into the game, if there is a file
void HostSettingsApplyOnce();
// Write the file when the options have changed
void HostSettingsKeep();
// The game thread's once-a-second tick (Platform::Graphics::BeginLogicFrame): HostSettingsKeep when a second has passed.
// The audio callback must not call either: it would read the game's own state while the game thread writes it, and touch
// the filesystem on the realtime thread
void HostSettingsTick();
// The picture's antialiasing, from the file's `antialias` line: off when the file has none, and off for a word this build
// has no mode for. Read before the renderer makes its first frame's targets, like the rate, and written back as it was read
AntiAlias HostSettingsAntiAlias();
// The antialiasing kept, as the options screen changes it: written at the next tick
void HostSettingsSetAntiAlias(AntiAlias mode);

// The language the game's voices, movies, title cards and credits are in, from the file's `language` line: english, french,
// german, spanish or italian, the five the disc carries. English when the file has none, and for a word this build has no
// language for. Asked for as the game starts, which is where it puts its language in place
Platform::System::ConsoleLanguage HostSettingsLanguage();
// A language's name as the game and the disc spell it (English, French, German, Spanish, Italian: its text folders and its
// voice banks): for finding the disc's, and for the line about a disc that has none of it
const char* HostSettingsLanguageName(Platform::System::ConsoleLanguage language);
// The language its texts are read in: the file's `text_language` line when it names one, else the language above. A disc whose
// texts were patched into another slot keeps its voices where they are
Platform::System::ConsoleLanguage HostSettingsTextLanguage();

// The picture's rate: how often the host presents a frame (the game's logic stays at the disc's field rate). Read from the
// file's `render` line before the renderer starts; 50, the field rate, when the file has none. 0 is unlimited
s32 HostSettingsRenderRate();
// The picture's rate kept, as the options screen changes it: written at the next tick
void HostSettingsSetRenderRate(s32 rate);

// The languages a player can choose between, as indices into the five the disc carries (english, french, german, spanish,
// italian: the words the file's `language` line takes), the two in use and the way to change them. The game puts its languages
// in place once, as it starts, so a change is kept for the next run
u32 HostSettingsLanguageChoices();
const char* const* HostSettingsLanguageChoiceNames();
u32 HostSettingsLanguageChoice();
u32 HostSettingsTextLanguageChoice();
void HostSettingsSetLanguageChoice(u32 index);
void HostSettingsSetTextLanguageChoice(u32 index);

// The display's own settings, from the file's `window`, `resolution`, `vsync`, `aspect` and `scale` lines: the window's mode,
// the resolution in pixels (0 by 0: the design's 960x540), vsync, the aspect and the resolution scale as a multiplier of
// 960x540 (1 is the design's). The words are the ones Platform::Display names its modes with, so what the display wrote is what
// it reads back. Read once, before the window exists, and kept up to date as the options menu changes them
struct HostDisplaySettings
{
    u32 width;
    u32 height;
    const char* window;
    bool vsync;
    const char* aspect;
    float scale;
    float drawDistance;
};
HostDisplaySettings HostSettingsDisplay();
// The display's settings kept: the next tick writes them to the file, whether or not the game's own options changed
void HostSettingsSetDisplay(const HostDisplaySettings& settings);

// The camera's reach, as a multiple of the lens's far plane: the file's `draw_distance` line, 1 when it has none. Read by the
// renderer for the projection and for the frustum's far plane, so it must stay cheap (it is called every frame)
f32 HostSettingsDrawDistance();
