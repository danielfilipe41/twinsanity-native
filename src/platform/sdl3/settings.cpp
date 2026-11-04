// The settings file itself (settings.h): a line per option, beside the saves.
#include "settings.h"

#include "game/renderer.h"
#include "game/sound.h"

#include "platform/saves.h"

#include <chrono>
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
    char path[PathLength];
    SettingsPath(path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return;
    }

    char line[LineLength];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        int rate = 0;
        if (std::sscanf(line, "render %d", &rate) == 1 && rate > 0)
        {
            g_RenderRate = rate;
        }
    }

    std::fclose(file);
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
    char path[PathLength];
    SettingsPath(path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return;
    }

    char line[LineLength];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        char word[16] = {};
        if (std::sscanf(line, "antialias %15s", word) != 1)
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

    std::fclose(file);
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

AntiAlias HostSettingsAntiAlias()
{
    LoadAntiAlias();
    return g_AntiAlias;
}

void HostSettingsApplyOnce()
{
    if (g_Applied)
    {
        return;
    }

    g_Applied = true;
    char path[PathLength];
    SettingsPath(path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return;
    }

    s16 left[Groups] = {};
    s16 right[Groups] = {};
    u32 stereo = g_MusicStereo;
    u8 widescreen = g_WidescreenTv;
    char line[LineLength];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        // "group <number> <left> <right>", "stereo <number>", "widescreen <number>"
        u32 group = 0;
        int a = 0;
        int b = 0;
        if (std::sscanf(line, "group %u %d %d", &group, &a, &b) == 3 && group < Groups)
        {
            left[group] = static_cast<s16>(a);
            right[group] = static_cast<s16>(b);
        }
        else if (std::sscanf(line, "stereo %d", &a) == 1)
        {
            stereo = static_cast<u32>(a);
        }
        else if (std::sscanf(line, "widescreen %d", &a) == 1)
        {
            widescreen = static_cast<u8>(a);
        }
    }

    std::fclose(file);
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

    if (g_Kept && std::memcmp(left, g_KeptLeft, sizeof(left)) == 0 && std::memcmp(right, g_KeptRight, sizeof(right)) == 0 &&
        stereo == g_KeptStereo && widescreen == g_KeptWidescreen)
    {
        return;
    }

    std::memcpy(g_KeptLeft, left, sizeof(left));
    std::memcpy(g_KeptRight, right, sizeof(right));
    g_KeptStereo = stereo;
    g_KeptWidescreen = widescreen;
    g_Kept = true;
    LoadRenderRate();
    char path[PathLength];
    SettingsPath(path);
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return;
    }

    for (u32 group = 0; group < Groups; group++)
    {
        std::fprintf(file, "group %u %d %d\n", group, static_cast<int>(left[group]), static_cast<int>(right[group]));
    }

    // The antialiasing is the file's own line, written back as it was read: the port has no screen to set it on, so editing
    // the file is the way, and a run that rewrote the file must not lose the choice
    std::fprintf(file, "stereo %d\nwidescreen %d\nrender %d\nantialias %s\n", static_cast<int>(stereo),
                 static_cast<int>(widescreen), static_cast<int>(g_RenderRate), AntiAliasWord(g_AntiAlias));
    std::fclose(file);
}
