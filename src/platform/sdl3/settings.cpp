// The settings file itself (settings.h): a line per option, beside the saves.
#include "settings.h"

#include "platform/files.h"

#include "game/renderer.h"
#include "game/sound.h"

#include "platform/saves.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr u32 PathLength = 512;
constexpr u32 Groups = VolumeGroupCount;
constexpr u32 LineLength = 128;
bool g_Applied = false;
// The values last kept (what a write put in the file), so only a real change is written
bool g_Kept = false;
s16 g_KeptLeft[Groups] = {};
s16 g_KeptRight[Groups] = {};
u32 g_KeptStereo = 0;
u8 g_KeptWidescreen = 0;

// The saves' base, which Platform::Saves owns: the settings file sits beside the cards, and deriving the directory in
// one place keeps the two from diverging when where user data lives changes
void SettingsPath(char* path)
{
    // The base is bounded so the file name always fits (the compiler sees a plain char[512] overrunning the buffer)
    char base[PathLength - sizeof("/settings.txt")];
    Platform::Saves::DataDirectory(base, sizeof(base));
    std::snprintf(path, PathLength, "%s/settings.txt", base);
}

// The file's lines, read once and kept: every option's reader scans them (each one used to open, read and close the file for
// its own line alone). A write drops them, so a reader that runs after the file was written reads what was written
bool g_LinesLoaded = false;
std::vector<std::string> g_Lines;

const std::vector<std::string>& SettingsLines()
{
    if (g_LinesLoaded)
    {
        return g_Lines;
    }

    g_LinesLoaded = true;
    g_Lines.clear();

    char path[PathLength];
    SettingsPath(path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return g_Lines;
    }

    char line[LineLength];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        g_Lines.emplace_back(line);
    }

    std::fclose(file);
    return g_Lines;
}

// The picture's rate when the file has none: the disc's field rate, so the host presents exactly when the game steps
constexpr s32 DefaultRenderRate = 50;
bool g_RenderRateLoaded = false;
s32 g_RenderRate = DefaultRenderRate;

// The file's `render` line: the picture's rate, read once before the renderer starts (the sound options wait for the sound
// processor's first reset, this one can't: the renderer paces itself with it)
void LoadRenderRate()
{
    if (g_RenderRateLoaded)
    {
        return;
    }

    g_RenderRateLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        int rate = 0;
        // 0 is unlimited (the picture presents as fast as the frame is drawn)
        if (std::sscanf(line.c_str(), "render %d", &rate) == 1 && rate >= 0)
        {
            g_RenderRate = rate;
        }
    }
}

// The file's `antialias` line: the picture's antialiasing. Read once before the renderer makes the first frame's targets
// (bgfx_context.cpp's ReadAntiAliasing), like the rate: the passes it selects have to exist before the frame that draws them
bool g_AntiAliasLoaded = false;
AntiAlias g_AntiAlias = AntiAlias::Off;

void LoadAntiAlias()
{
    if (g_AntiAliasLoaded)
    {
        return;
    }

    g_AntiAliasLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        char word[16] = {};
        if (std::sscanf(line.c_str(), "antialias %15s", word) != 1)
        {
            continue;
        }

        if (std::strcmp(word, "fxaa") == 0)
        {
            g_AntiAlias = AntiAlias::Fxaa;
        }
        else if (std::strcmp(word, "smaa") == 0)
        {
            g_AntiAlias = AntiAlias::Smaa;
        }
        else
        {
            // "off", or a word this build has no mode for: the picture is drawn as it comes, which is never wrong
            g_AntiAlias = AntiAlias::Off;
        }
    }
}

// The display's own lines (`window`, `resolution`, `vsync`, `aspect`, `scale`): the host's picture rather than the game's
// options. Read once, before the window exists, and kept as the words the file has, so what the display wrote is what it reads
// back. Anything the options menu changes marks the file dirty, and the next tick writes it whether or not the game's own
// options moved with them
bool g_DisplayLoaded = false;
u32 g_PictureWidth = 0;
u32 g_PictureHeight = 0;
char g_DisplayWindow[16] = "windowed";
bool g_DisplayVsync = false;
char g_DisplayAspect[16] = "stretch";
float g_DisplayScale = 1.0f;
float g_DrawDistance = 1.0f;
bool g_SettingsDirty = false;

// The sticks' dead zone (the pads' own, which the options screen changes and the file keeps)
bool g_DeadZoneLoaded = false;
u32 g_DeadZone = 0;

// Whether cutscenes can be skipped (the game options' row, which the file keeps): on unless the file says off
bool g_CutsceneSkipLoaded = false;
bool g_CutsceneSkip = true;

// How fast a skipped scripted cutscene runs out (the file's `cutscene_speed`): a thousand seconds of it a second by default, which a
// scene's own scripts reach their own end in a step or two
bool g_CutsceneSpeedLoaded = false;
u32 g_CutsceneSpeed = 10;
// The speed's range in the file (seconds of the scene a second of play): 1 is the game's own, the top a step holding many minutes of
// it, where a scene's own end is a step or two away
constexpr u32 MinCutsceneSpeed = 1;
constexpr u32 MaxCutsceneSpeed = 1000;

void LoadCutsceneSkip()
{
    if (g_CutsceneSkipLoaded)
    {
        return;
    }

    g_CutsceneSkipLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        char word[8] = {};
        if (std::sscanf(line.c_str(), "cutscene_skip %7s", word) == 1)
        {
            g_CutsceneSkip = std::strcmp(word, "off") != 0;
        }
    }
}

void LoadCutsceneSpeed()
{
    if (g_CutsceneSpeedLoaded)
    {
        return;
    }

    g_CutsceneSpeedLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        unsigned speed = 0;
        if (std::sscanf(line.c_str(), "cutscene_speed %u", &speed) == 1)
        {
            g_CutsceneSpeed = speed < MinCutsceneSpeed ? MinCutsceneSpeed : (speed > MaxCutsceneSpeed ? MaxCutsceneSpeed : speed);
        }
    }
}

void LoadDeadZone()
{
    if (g_DeadZoneLoaded)
    {
        return;
    }

    g_DeadZoneLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        unsigned percent = 0;
        if (std::sscanf(line.c_str(), "deadzone %u", &percent) == 1)
        {
            g_DeadZone = percent > Platform::Pads::MaxDeadZone ? Platform::Pads::MaxDeadZone : percent;
        }
    }
}

void LoadDisplay()
{
    if (g_DisplayLoaded)
    {
        return;
    }

    g_DisplayLoaded = true;
    for (const std::string& line : SettingsLines())
    {
        unsigned width = 0;
        unsigned height = 0;
        char word[16] = {};
        float scale = 0.0f;
        if (std::sscanf(line.c_str(), "resolution %ux%u", &width, &height) == 2)
        {
            g_PictureWidth = width;
            g_PictureHeight = height;
        }
        else if (std::sscanf(line.c_str(), "window %15s", word) == 1)
        {
            std::snprintf(g_DisplayWindow, sizeof(g_DisplayWindow), "%s", word);
        }
        else if (std::sscanf(line.c_str(), "vsync %15s", word) == 1)
        {
            g_DisplayVsync = std::strcmp(word, "off") != 0;
        }
        else if (std::sscanf(line.c_str(), "aspect %15s", word) == 1)
        {
            std::snprintf(g_DisplayAspect, sizeof(g_DisplayAspect), "%s", word);
        }
        else if (std::sscanf(line.c_str(), "scale %f", &scale) == 1 && scale > 0.0f)
        {
            g_DisplayScale = scale;
        }
        else if (std::sscanf(line.c_str(), "draw_distance %f", &scale) == 1 && scale > 0.0f)
        {
            g_DrawDistance = scale;
        }
    }
}

// The file's `language` line, and its `text_language` line when the texts aren't in the speech's language: read once, as the
// game starts and asks the console which language it runs in (Platform::System::Language and TextLanguage), which is the one
// place the retail code takes it from
bool g_LanguageLoaded = false;
Platform::System::ConsoleLanguage g_Language = Platform::System::LanguageEnglish;
Platform::System::ConsoleLanguage g_TextLanguage = Platform::System::LanguageEnglish;

constexpr const char* LanguageWords[] = {"english", "french", "german", "spanish", "italian"};
// The same names as the game and the disc spell them (its text folders, its voice banks: Crash6\English.mb)
constexpr const char* LanguageDisplayNames[] = {"English", "French", "German", "Spanish", "Italian"};
constexpr Platform::System::ConsoleLanguage LanguageValues[] = {
    Platform::System::LanguageEnglish, Platform::System::LanguageFrench, Platform::System::LanguageGerman,
    Platform::System::LanguageSpanish, Platform::System::LanguageItalian};
constexpr u32 LanguageCount = sizeof(LanguageWords) / sizeof(LanguageWords[0]);

const char* LanguageWord(Platform::System::ConsoleLanguage language)
{
    for (u32 index = 0; index < LanguageCount; index++)
    {
        if (LanguageValues[index] == language)
        {
            return LanguageWords[index];
        }
    }
    return LanguageWords[0];
}

// A word of the file as the console language it names: English for one this build has no language for, which every disc has
Platform::System::ConsoleLanguage LanguageOf(char* word)
{
    for (char* letter = word; *letter != '\0'; letter++)
    {
        if (*letter >= 'A' && *letter <= 'Z')
        {
            *letter = static_cast<char>(*letter - 'A' + 'a');
        }
    }

    for (u32 index = 0; index < LanguageCount; index++)
    {
        if (std::strcmp(word, LanguageWords[index]) == 0)
        {
            return LanguageValues[index];
        }
    }
    return Platform::System::LanguageEnglish;
}

// Whether the disc has a language at all: its voice bank (Crash6\\English.mb, which the file layer answers from the disc's own
// name when its build files it differently). Its texts come from the same slot, so a disc without the one has neither
bool DiscHasLanguage(Platform::System::ConsoleLanguage language)
{
    const char* name = HostSettingsLanguageName(language);
    if (name == nullptr)
    {
        return false;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "Crash6\\%s.mb", name);
    const s32 file = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (file < 0)
    {
        return false;
    }
    Platform::Files::Close(file);
    return true;
}

void LoadLanguage()
{
    if (g_LanguageLoaded)
    {
        return;
    }

    g_LanguageLoaded = true;
    Platform::System::ConsoleLanguage text = Platform::System::LanguageEnglish;
    bool textNamed = false;

    for (const std::string& line : SettingsLines())
    {
        char word[16] = {};
        if (std::sscanf(line.c_str(), "language %15s", word) == 1)
        {
            g_Language = LanguageOf(word);
        }
        else if (std::sscanf(line.c_str(), "text_language %15s", word) == 1)
        {
            text = LanguageOf(word);
            textNamed = true;
        }
    }

    // The texts are in the language played in unless the file names another
    g_TextLanguage = textNamed ? text : g_Language;

    // A language the disc does not carry would leave the game waiting on files that are not there (its voice bank, its texts):
    // the disc's own voice banks say which languages it has, and English is the one every disc has, by that name or the one
    // its build files it under
    if (!DiscHasLanguage(g_Language))
    {
        if (DiscHasLanguage(Platform::System::LanguageEnglish))
        {
            std::fprintf(stderr, "settings: this disc has no %s voices; playing in english\n",
                         HostSettingsLanguageName(g_Language));
            g_Language = Platform::System::LanguageEnglish;
            g_TextLanguage = Platform::System::LanguageEnglish;
        }
    }
    else if (!DiscHasLanguage(g_TextLanguage) && DiscHasLanguage(Platform::System::LanguageEnglish))
    {
        std::fprintf(stderr, "settings: this disc has no %s texts; reading them in english\n",
                     HostSettingsLanguageName(g_TextLanguage));
        g_TextLanguage = Platform::System::LanguageEnglish;
    }

    std::fprintf(stderr, "settings: language %s, texts %s\n", LanguageWord(g_Language), LanguageWord(g_TextLanguage));
}

// The word the file's `antialias` line is written with
const char* AntiAliasWord(AntiAlias mode)
{
    switch (mode)
    {
    case AntiAlias::Fxaa:
        return "fxaa";
    case AntiAlias::Smaa:
        return "smaa";
    case AntiAlias::Off:
    default:
        return "off";
    }
}

// The live options, as the game keeps them
void LiveOptions(s16* left, s16* right, u32* stereo, u8* widescreen)
{
    for (u32 group = 0; group < Groups; group++)
    {
        left[group] = g_GroupVolumes[group].left;
        right[group] = g_GroupVolumes[group].right;
    }

    *stereo = g_MusicStereo;
    *widescreen = g_WidescreenTv;
}

// The game's own state set from a value a group's volume was kept at, and the sound processor told directly: the game's
// SetGroupVolume scales by the volume group's fade (GroupScales), which hasn't been set yet this early in the start-up, and
// the game's own ResetSound applies g_GroupVolumes again later with the fade right
void UseGroupVolume(u32 group, s16 left, s16 right)
{
    g_GroupVolumes[group].left = left;
    g_GroupVolumes[group].right = right;
    Platform::Audio::SetGroupVolume(static_cast<s32>((group + 1) << Platform::Audio::GroupShift), static_cast<u32>(left),
                                    static_cast<u32>(right));
}
}

s32 HostSettingsRenderRate()
{
    LoadRenderRate();
    return g_RenderRate;
}

f32 HostSettingsDrawDistance()
{
    LoadDisplay();
    return g_DrawDistance;
}

u32 HostSettingsDeadZone()
{
    LoadDeadZone();
    return g_DeadZone;
}

void HostSettingsSetDeadZone(u32 percent)
{
    LoadDeadZone();
    const u32 kept = percent > Platform::Pads::MaxDeadZone ? Platform::Pads::MaxDeadZone : percent;
    if (kept == g_DeadZone)
    {
        return;
    }

    g_DeadZone = kept;
    g_SettingsDirty = true;
}

bool HostSettingsCutsceneSkip()
{
    LoadCutsceneSkip();
    return g_CutsceneSkip;
}

u32 HostSettingsCutsceneSpeed()
{
    LoadCutsceneSpeed();
    return g_CutsceneSpeed;
}

void HostSettingsSetCutsceneSkip(bool skip)
{
    LoadCutsceneSkip();
    if (skip == g_CutsceneSkip)
    {
        return;
    }

    g_CutsceneSkip = skip;
    g_SettingsDirty = true;
}

void HostSettingsSetAntiAlias(AntiAlias mode)
{
    LoadAntiAlias();
    if (mode == g_AntiAlias)
    {
        return;
    }

    g_AntiAlias = mode;
    g_SettingsDirty = true;
}

HostDisplaySettings HostSettingsDisplay()
{
    LoadDisplay();
    HostDisplaySettings settings;
    settings.width = g_PictureWidth;
    settings.height = g_PictureHeight;
    settings.window = g_DisplayWindow;
    settings.vsync = g_DisplayVsync;
    settings.aspect = g_DisplayAspect;
    settings.scale = g_DisplayScale;
    settings.drawDistance = g_DrawDistance;
    return settings;
}

void HostSettingsSetDisplay(const HostDisplaySettings& settings)
{
    LoadDisplay();
    const char* window = settings.window != nullptr ? settings.window : g_DisplayWindow;
    const char* aspect = settings.aspect != nullptr ? settings.aspect : g_DisplayAspect;
    if (settings.width == g_PictureWidth && settings.height == g_PictureHeight && settings.vsync == g_DisplayVsync &&
        settings.scale == g_DisplayScale && settings.drawDistance == g_DrawDistance &&
        std::strcmp(window, g_DisplayWindow) == 0 &&
        std::strcmp(aspect, g_DisplayAspect) == 0)
    {
        return;
    }

    g_PictureWidth = settings.width;
    g_PictureHeight = settings.height;
    g_DisplayVsync = settings.vsync;
    g_DisplayScale = settings.scale;
    g_DrawDistance = settings.drawDistance > 0.0f ? settings.drawDistance : 1.0f;
    std::snprintf(g_DisplayWindow, sizeof(g_DisplayWindow), "%s", window);
    std::snprintf(g_DisplayAspect, sizeof(g_DisplayAspect), "%s", aspect);
    g_SettingsDirty = true;
}

void HostSettingsSetRenderRate(s32 rate)
{
    LoadRenderRate();
    const s32 kept = rate < 0 ? 0 : rate;
    if (kept == g_RenderRate)
    {
        return;
    }

    g_RenderRate = kept;
    g_SettingsDirty = true;
}

AntiAlias HostSettingsAntiAlias()
{
    LoadAntiAlias();
    return g_AntiAlias;
}

const char* HostSettingsLanguageName(Platform::System::ConsoleLanguage language)
{
    for (u32 index = 0; index < LanguageCount; index++)
    {
        if (LanguageValues[index] == language)
        {
            return LanguageDisplayNames[index];
        }
    }
    return LanguageDisplayNames[0];
}

Platform::System::ConsoleLanguage HostSettingsLanguage()
{
    LoadLanguage();
    return g_Language;
}

Platform::System::ConsoleLanguage HostSettingsTextLanguage()
{
    LoadLanguage();
    return g_TextLanguage;
}

u32 HostSettingsLanguageChoices()
{
    return sizeof(LanguageWords) / sizeof(LanguageWords[0]);
}

const char* const* HostSettingsLanguageChoiceNames()
{
    return LanguageWords;
}

u32 HostSettingsLanguageChoice()
{
    LoadLanguage();
    for (u32 index = 0; index < HostSettingsLanguageChoices(); index++)
    {
        if (LanguageValues[index] == g_Language)
        {
            return index;
        }
    }

    return 0;
}

u32 HostSettingsTextLanguageChoice()
{
    LoadLanguage();
    for (u32 index = 0; index < HostSettingsLanguageChoices(); index++)
    {
        if (LanguageValues[index] == g_TextLanguage)
        {
            return index;
        }
    }

    return 0;
}

void HostSettingsSetLanguageChoice(u32 index)
{
    LoadLanguage();
    if (index >= HostSettingsLanguageChoices() || LanguageValues[index] == g_Language)
    {
        return;
    }

    g_Language = LanguageValues[index];
    g_SettingsDirty = true;
}

void HostSettingsSetTextLanguageChoice(u32 index)
{
    LoadLanguage();
    if (index >= HostSettingsLanguageChoices() || LanguageValues[index] == g_TextLanguage)
    {
        return;
    }

    g_TextLanguage = LanguageValues[index];
    g_SettingsDirty = true;
}

void HostSettingsApplyOnce()
{
    if (g_Applied)
    {
        return;
    }

    g_Applied = true;
    s16 left[Groups] = {};
    s16 right[Groups] = {};
    u32 stereo = g_MusicStereo;
    u8 widescreen = g_WidescreenTv;
    for (const std::string& line : SettingsLines())
    {
        // "group <number> <left> <right>", "stereo <number>", "widescreen <number>"
        u32 group = 0;
        int a = 0;
        int b = 0;
        if (std::sscanf(line.c_str(), "group %u %d %d", &group, &a, &b) == 3 && group < Groups)
        {
            left[group] = static_cast<s16>(a);
            right[group] = static_cast<s16>(b);
        }
        else if (std::sscanf(line.c_str(), "stereo %d", &a) == 1)
        {
            stereo = static_cast<u32>(a);
        }
        else if (std::sscanf(line.c_str(), "widescreen %d", &a) == 1)
        {
            widescreen = static_cast<u8>(a);
        }
    }
    // A file with everything zero holds the data's initials (the group volumes are bss, and the stereo's own is StereoOff): it was
    // written before the game set its own options, and applying it silences the game. The game's options (its save's) win over it
    bool silent = true;
    for (u32 group = 0; group < Groups && silent; group++)
    {
        silent = left[group] == 0 && right[group] == 0;
    }

    if (silent && stereo == 0 && widescreen == 0)
    {
        return;
    }

    for (u32 group = 0; group < Groups; group++)
    {
        UseGroupVolume(group, left[group], right[group]);
    }

    // The global alone: the music players read it as they play, and SetMusicStereo walks them (there are none this early)
    g_MusicStereo = stereo;
    g_WidescreenTv = widescreen;
}

void HostSettingsTick()
{
    // Once a second, on the game thread: the values it keeps are the game's own, which only that thread writes
    static std::chrono::steady_clock::time_point next;
    static bool started = false;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!started)
    {
        started = true;
        next = now + std::chrono::seconds(1);
        return;
    }

    if (now < next)
    {
        return;
    }

    next = now + std::chrono::seconds(1);
    HostSettingsKeep();
}

// The file's lines, as written: a player's own comment, or an option a later build knows, is not the port's to drop, so the
// file is merged rather than replaced -- the lines this port owns are replaced where they are (or appended when missing) and
// everything else is written back as it came
constexpr u32 MaxFileLines = 64;
using FileLines = char[MaxFileLines][LineLength];

// The settings the file's lines hold
enum class Setting
{
    Group,
    Stereo,
    Widescreen,
    Render,
    AntiAlias,
    Language,
    TextLanguage,
    Window,
    Resolution,
    Vsync,
    Aspect,
    Scale,
    DrawDistance,
    DeadZone,
    SaveCard,
    CutsceneSkip,
    CutsceneSpeed,
};

// Whether the line begins with the word (the leading whitespace aside, and the word complete)
bool LineStarts(const char* line, const char* word)
{
    while (*line == ' ' || *line == '\t')
    {
        line++;
    }

    const size_t length = std::strlen(word);
    return std::strncmp(line, word, length) == 0 &&
           (line[length] == ' ' || line[length] == '\t' || line[length] == '\0');
}

// Whether the line is one a setting is written as: `group <number> ...` for a group's (its own number), the word alone for
// the others
bool LineIsSetting(Setting setting, const char* line, u32 group)
{
    switch (setting)
    {
    case Setting::Group:
    {
        unsigned number = 0;
        return LineStarts(line, "group") && std::sscanf(line, "group %u", &number) == 1 && number == group;
    }
    case Setting::Stereo:
        return LineStarts(line, "stereo");
    case Setting::Widescreen:
        return LineStarts(line, "widescreen");
    case Setting::Render:
        return LineStarts(line, "render");
    case Setting::AntiAlias:
        return LineStarts(line, "antialias");
    case Setting::Language:
        return LineStarts(line, "language");
    case Setting::TextLanguage:
        return LineStarts(line, "text_language");
    case Setting::Window:
        return LineStarts(line, "window");
    case Setting::Resolution:
        return LineStarts(line, "resolution");
    case Setting::Vsync:
        return LineStarts(line, "vsync");
    case Setting::Aspect:
        return LineStarts(line, "aspect");
    case Setting::Scale:
        return LineStarts(line, "scale");
    case Setting::DrawDistance:
        return LineStarts(line, "draw_distance");
    case Setting::DeadZone:
        return LineStarts(line, "deadzone");
    case Setting::SaveCard:
        return LineStarts(line, "save_card");
    case Setting::CutsceneSkip:
        return LineStarts(line, "cutscene_skip");
    case Setting::CutsceneSpeed:
        return LineStarts(line, "cutscene_speed");
    }

    return false;
}

// The setting's line put in the file's: the first line of its kind replaced in place, any later one dropped (the port wrote
// duplicates once), and a setting the file doesn't have appended. An empty text removes the line
void PlaceSetting(FileLines& lines, u32* count, Setting setting, u32 group, const char* text)
{
    bool placed = false;
    for (u32 index = 0; index < *count;)
    {
        if (!LineIsSetting(setting, lines[index], group))
        {
            index++;
            continue;
        }

        if (!placed && text != nullptr)
        {
            std::snprintf(lines[index], LineLength, "%s", text);
            placed = true;
            index++;
            continue;
        }

        // A duplicate of the line just placed, or a line being removed. The copy is a line's own size, so it cannot be told
        // the wrong one
        for (u32 other = index; other + 1 < *count; other++)
        {
            std::memcpy(lines[other], lines[other + 1], sizeof(lines[other]));
        }
        (*count)--;
    }

    if (!placed && text != nullptr && *count < MaxFileLines)
    {
        std::snprintf(lines[(*count)++], LineLength, "%s", text);
    }
}

// The file written: its own lines merged with this port's
void WriteSettings(const s16* left, const s16* right, u32 stereo, u8 widescreen)
{
    static FileLines lines;
    u32 count = 0;
    char path[PathLength];
    SettingsPath(path);
    // The file as it was read (SettingsLines' lines, the readers' own): the merge starts from what is there, without a
    // second read of it
    for (const std::string& line : SettingsLines())
    {
        if (count >= MaxFileLines)
        {
            break;
        }

        std::snprintf(lines[count], LineLength, "%s", line.c_str());
        size_t length = std::strlen(lines[count]);
        while (length != 0 && (lines[count][length - 1] == '\n' || lines[count][length - 1] == '\r'))
        {
            lines[count][--length] = '\0';
        }
        count++;
    }

    char text[LineLength];
    for (u32 group = 0; group < Groups; group++)
    {
        std::snprintf(text, sizeof(text), "group %u %d %d", group, static_cast<int>(left[group]),
                      static_cast<int>(right[group]));
        PlaceSetting(lines, &count, Setting::Group, group, text);
    }

    std::snprintf(text, sizeof(text), "stereo %d", static_cast<int>(stereo));
    PlaceSetting(lines, &count, Setting::Stereo, 0, text);
    std::snprintf(text, sizeof(text), "widescreen %d", static_cast<int>(widescreen));
    PlaceSetting(lines, &count, Setting::Widescreen, 0, text);
    std::snprintf(text, sizeof(text), "render %d", static_cast<int>(g_RenderRate));
    PlaceSetting(lines, &count, Setting::Render, 0, text);
    std::snprintf(text, sizeof(text), "antialias %s", AntiAliasWord(g_AntiAlias));
    PlaceSetting(lines, &count, Setting::AntiAlias, 0, text);
    std::snprintf(text, sizeof(text), "language %s", LanguageWord(g_Language));
    PlaceSetting(lines, &count, Setting::Language, 0, text);
    // Only when the texts aren't in the speech's language: a line that says what the file already said is noise
    if (g_TextLanguage != g_Language)
    {
        std::snprintf(text, sizeof(text), "text_language %s", LanguageWord(g_TextLanguage));
        PlaceSetting(lines, &count, Setting::TextLanguage, 0, text);
    }
    else
    {
        PlaceSetting(lines, &count, Setting::TextLanguage, 0, nullptr);
    }

    std::snprintf(text, sizeof(text), "draw_distance %g", static_cast<double>(g_DrawDistance));
    PlaceSetting(lines, &count, Setting::DrawDistance, 0, text);
    std::snprintf(text, sizeof(text), "deadzone %u", HostSettingsDeadZone());
    PlaceSetting(lines, &count, Setting::DeadZone, 0, text);
    std::snprintf(text, sizeof(text), "save_card %u", Platform::Saves::Card());
    PlaceSetting(lines, &count, Setting::SaveCard, 0, text);
    std::snprintf(text, sizeof(text), "cutscene_skip %s", HostSettingsCutsceneSkip() ? "on" : "off");
    PlaceSetting(lines, &count, Setting::CutsceneSkip, 0, text);
    std::snprintf(text, sizeof(text), "cutscene_speed %u", HostSettingsCutsceneSpeed());
    PlaceSetting(lines, &count, Setting::CutsceneSpeed, 0, text);

    std::snprintf(text, sizeof(text), "window %s", g_DisplayWindow);
    PlaceSetting(lines, &count, Setting::Window, 0, text);
    if (g_PictureWidth != 0 && g_PictureHeight != 0)
    {
        std::snprintf(text, sizeof(text), "resolution %ux%u", g_PictureWidth, g_PictureHeight);
        PlaceSetting(lines, &count, Setting::Resolution, 0, text);
    }
    std::snprintf(text, sizeof(text), "vsync %s", g_DisplayVsync ? "on" : "off");
    PlaceSetting(lines, &count, Setting::Vsync, 0, text);
    std::snprintf(text, sizeof(text), "aspect %s", g_DisplayAspect);
    PlaceSetting(lines, &count, Setting::Aspect, 0, text);
    std::snprintf(text, sizeof(text), "scale %g", static_cast<double>(g_DisplayScale));
    PlaceSetting(lines, &count, Setting::Scale, 0, text);

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < count; index++)
    {
        std::fprintf(file, "%s\n", lines[index]);
    }
    std::fclose(file);
    // What was written is the file now: the next reader's scan starts from it rather than from what this read held
    g_LinesLoaded = false;
}

void HostSettingsKeep()
{
    // Only once the game has started its audio (its first reset): before that its option values are still the data's initial
    // zeros, and keeping those would write a file that silences the next run
    if (!g_Applied)
    {
        return;
    }

    s16 left[Groups] = {};
    s16 right[Groups] = {};
    u32 stereo = 0;
    u8 widescreen = 0;
    LiveOptions(left, right, &stereo, &widescreen);
    // The game reaches an all-zero set on its own (a fresh save controller's options applied): writing those would poison every
    // later run, so an all-zero set is never kept
    bool silent = true;
    for (u32 group = 0; group < Groups && silent; group++)
    {
        silent = left[group] == 0 && right[group] == 0;
    }

    if (silent && stereo == 0 && widescreen == 0)
    {
        return;
    }

    const bool unchanged = g_Kept && std::memcmp(left, g_KeptLeft, sizeof(left)) == 0 &&
                           std::memcmp(right, g_KeptRight, sizeof(right)) == 0 && stereo == g_KeptStereo &&
                           widescreen == g_KeptWidescreen;
    if (unchanged && !g_SettingsDirty)
    {
        return;
    }

    std::memcpy(g_KeptLeft, left, sizeof(left));
    std::memcpy(g_KeptRight, right, sizeof(right));
    g_KeptStereo = stereo;
    g_KeptWidescreen = widescreen;
    g_Kept = true;
    g_SettingsDirty = false;
    // The lines the write re-emits as they were read: the picture's rate, its antialiasing, the languages and the display's own
    // settings. They are cached from the same file, so a value this run never loaded would be written back as its default
    LoadRenderRate();
    LoadAntiAlias();
    LoadLanguage();
    LoadDisplay();
    WriteSettings(left, right, stereo, widescreen);
}
