#pragma once

#include "common.h"

// The platform layer: what the game needs of the machine it runs on. The game's C++ calls these interfaces and a platform
// implements them (src/platform/<platform>/, picked by configure.py). The PS2 one is on PS2SDK; the retail asm that's left calls
// Sony's SDK by name, which src/sce/ puts on the same interfaces. The platform's entry sets things up and calls Main.
namespace Platform::System
{
// Makes the machine ready for the game, first thing: on the PS2 the graphics hardware reset and the I/O processor restarted with
// the disc's modules
void Initialise();
// Starts what the pads, the memory cards and the sound go through
void StartServices();
// Ends the run. The game's own exit row (and a window's close) asks for it from inside a frame, so a host only records the ask
// here: the process is ended at the frame's end (EndRun), never where the call was made. Ending it there tears the renderer down
// while the draws the frame already queued are still waiting for their state and the renderer's thread is still running the frame
// before it; the C library's exit then unloads the driver's libraries under that thread, and the process dies inside the driver
void Exit(s32 status);
// Whether Exit was asked for and the run has not ended yet, and the host's frame end ending it there: the game's loop has no exit
// of its own, so what Exit asked for happens between two of its frames
bool ExitRequested();
[[noreturn]] void EndRun();

// The console's languages (the PS2's system settings')
enum ConsoleLanguage : s32
{
    LanguageJapanese = 0,
    LanguageEnglish = 1,
    LanguageFrench = 2,
    LanguageSpanish = 3,
    LanguageGerman = 4,
    LanguageItalian = 5,
    LanguageDutch = 6,
    LanguagePortuguese = 7,
};

// The console's language, which is the one the game's voices, movies, title cards and credits are in (and its texts, unless
// TextLanguage names another)
ConsoleLanguage Language();
// The language the game's texts are read in, when it isn't the one the speech is: a disc whose text banks were moved between
// slots (a translation patch). The same as Language() wherever nothing says otherwise
ConsoleLanguage TextLanguage();

// The languages a player can choose between, named as the disc and the settings file spell them ("english", "french"...), the
// two in use as indices into that list and the way to change them. The game puts its languages in place once, early (its
// voices are streamed banks and its texts are read from the disc), so a change lands at the next start
u32 LanguageChoices();
const char* const* LanguageChoiceNames();
u32 LanguageChoice();
void SetLanguageChoice(u32 index);
u32 TextLanguageChoice();
void SetTextLanguageChoice(u32 index);

// Whether cutscenes can be skipped: a cutscene file on its first viewing too (retail only lets one played before go), and a
// scripted cutscene through the level's own skip condition (the game options' row, on by default). A platform without a settings
// file answers its own choice, and the console answers retail
bool CutsceneSkip();
void SetCutsceneSkip(bool skip);
// How many seconds of a scripted cutscene a skip runs at a time while it runs the scene out unseen (the settings file's
// `cutscene_speed`, 10 by default): the scene's own scripts still reach its own end, only sooner
u32 CutsceneSkipSpeed();


struct DateTime
{
    u16 year;
    // From 1
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
};

// The local date and time
DateTime LocalTime();
}
