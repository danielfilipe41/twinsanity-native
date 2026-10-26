// The settings file itself (settings.h): a line per option, beside the saves.
#include "settings.h"

#include "game/renderer.h"
#include "game/sound.h"

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

// The saves' base (Platform::Saves computes its own, which this can't reach): $XDG_DATA_HOME/twinsanity, else
// ~/.local/share/twinsanity
void SettingsPath(char* path)
{
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0')
    {
        std::snprintf(path, PathLength, "%s/twinsanity/settings.txt", dataHome);
        return;
    }

    const char* home = std::getenv("HOME");
    std::snprintf(path, PathLength, "%s/.local/share/twinsanity/settings.txt", home != nullptr ? home : "/tmp");
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

    std::fprintf(file, "stereo %d\nwidescreen %d\n", static_cast<int>(stereo), static_cast<int>(widescreen));
    std::fclose(file);
}
