#include "platform/keybinds.h"

#include "platform/saves.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <cstring>

namespace
{
using Action = Platform::Keybinds::Action;
using Kind = Platform::Keybinds::Kind;

// An action's name in the file, and the binding it has when the file has no line for it: the layout the port has always had (the
// arrows for the d-pad, the space bar and the letters beside it for the face buttons, the triggers' digits under the shoulders'
// letters, the two sticks on the keyboard's two halves; a pad's cross, circle, square and triangle where SDL puts them)
struct Binding
{
    const char* name;
    SDL_Scancode key;
    s32 pad;
};

Binding g_Bindings[] = {
    {"cross", SDL_SCANCODE_SPACE, Platform::Keybinds::PadSouth},
    {"circle", SDL_SCANCODE_K, Platform::Keybinds::PadEast},
    {"square", SDL_SCANCODE_J, Platform::Keybinds::PadWest},
    {"triangle", SDL_SCANCODE_L, Platform::Keybinds::PadNorth},
    {"up", SDL_SCANCODE_UP, Platform::Keybinds::PadDpadUp},
    {"down", SDL_SCANCODE_DOWN, Platform::Keybinds::PadDpadDown},
    {"left", SDL_SCANCODE_LEFT, Platform::Keybinds::PadDpadLeft},
    {"right", SDL_SCANCODE_RIGHT, Platform::Keybinds::PadDpadRight},
    {"l1", SDL_SCANCODE_Q, Platform::Keybinds::PadLeftShoulder},
    {"r1", SDL_SCANCODE_E, Platform::Keybinds::PadRightShoulder},
    {"l2", SDL_SCANCODE_1, Platform::Keybinds::PadLeftTrigger},
    {"r2", SDL_SCANCODE_3, Platform::Keybinds::PadRightTrigger},
    {"select", SDL_SCANCODE_BACKSPACE, Platform::Keybinds::PadBack},
    {"start", SDL_SCANCODE_RETURN, Platform::Keybinds::PadStart},
    {"l3", SDL_SCANCODE_UNKNOWN, Platform::Keybinds::PadLeftStick},
    {"r3", SDL_SCANCODE_UNKNOWN, Platform::Keybinds::PadRightStick},
    {"left stick up", SDL_SCANCODE_W, Platform::Keybinds::PadNone},
    {"left stick down", SDL_SCANCODE_S, Platform::Keybinds::PadNone},
    {"left stick left", SDL_SCANCODE_A, Platform::Keybinds::PadNone},
    {"left stick right", SDL_SCANCODE_D, Platform::Keybinds::PadNone},
    {"right stick up", SDL_SCANCODE_I, Platform::Keybinds::PadNone},
    {"right stick down", SDL_SCANCODE_K, Platform::Keybinds::PadNone},
    {"right stick right", SDL_SCANCODE_L, Platform::Keybinds::PadNone},
    {"right stick left", SDL_SCANCODE_J, Platform::Keybinds::PadNone},
};

constexpr u32 ActionCount = static_cast<u32>(Action::Count);
static_assert(sizeof(g_Bindings) / sizeof(g_Bindings[0]) == ActionCount, "one binding per action");

// The pad inputs' names in the file, SDL's layout names for them
struct PadName
{
    const char* name;
    s32 input;
};

const PadName g_PadNames[] = {
    {"none", Platform::Keybinds::PadNone},
    {"south", Platform::Keybinds::PadSouth},
    {"east", Platform::Keybinds::PadEast},
    {"west", Platform::Keybinds::PadWest},
    {"north", Platform::Keybinds::PadNorth},
    {"left shoulder", Platform::Keybinds::PadLeftShoulder},
    {"right shoulder", Platform::Keybinds::PadRightShoulder},
    {"left trigger", Platform::Keybinds::PadLeftTrigger},
    {"right trigger", Platform::Keybinds::PadRightTrigger},
    {"back", Platform::Keybinds::PadBack},
    {"start", Platform::Keybinds::PadStart},
    {"left stick", Platform::Keybinds::PadLeftStick},
    {"right stick", Platform::Keybinds::PadRightStick},
    {"dpad up", Platform::Keybinds::PadDpadUp},
    {"dpad down", Platform::Keybinds::PadDpadDown},
    {"dpad left", Platform::Keybinds::PadDpadLeft},
    {"dpad right", Platform::Keybinds::PadDpadRight},
};

bool g_Loaded = false;

// The first input pressed since the pad code last cleared it: the controller page's next binding (whether there was one)
bool g_Taken = false;
Kind g_TakenKind = Kind::Key;
s32 g_TakenInput = 0;

constexpr u32 PathLength = 512;
constexpr u32 MaxLines = 128;
constexpr u32 LineLength = 128;

// The file's lines, as read and as written back
struct FileLines
{
    char text[MaxLines][LineLength];
    u32 count;
};

// Where the file is: beside the settings file and the saves, so where a player's own data lives has one definition
void KeybindsPath(char* path)
{
    // The base is bounded so the file name always fits
    char base[PathLength - sizeof("/keybinds.txt")];
    Platform::Saves::DataDirectory(base, sizeof(base));
    std::snprintf(path, PathLength, "%s/keybinds.txt", base);
}

const char* KindName(Kind kind)
{
    return kind == Kind::Pad ? "pad" : "key";
}

// The pad input a name stands for (PadNone for a name no input has)
s32 PadFromName(const char* name)
{
    for (const PadName& pad : g_PadNames)
    {
        if (std::strcmp(pad.name, name) == 0)
        {
            return pad.input;
        }
    }

    return -1;
}

// The name of a binding's input: SDL's own for a key, the table's for a pad input, "none" for no binding
const char* InputName(Kind kind, s32 input)
{
    if (input == 0)
    {
        return "none";
    }

    if (kind == Kind::Pad)
    {
        for (const PadName& pad : g_PadNames)
        {
            if (pad.input == input)
            {
                return pad.name;
            }
        }

        return "none";
    }

    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(input));
    return (name == nullptr || name[0] == '\0') ? "none" : name;
}

// An input from the name the file gives it (-1 for a name this build has no input for, 0 for none)
s32 InputFromName(Kind kind, const char* name)
{
    if (std::strcmp(name, "none") == 0 || name[0] == '\0')
    {
        return 0;
    }

    if (kind == Kind::Pad)
    {
        return PadFromName(name);
    }

    const SDL_Scancode key = SDL_GetScancodeFromName(name);
    return key == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<s32>(key);
}

// A line's text for an action's binding: "<action> <key|pad> <input>"
void WriteBindingLine(char* out, u32 size, const Binding& binding, Kind kind)
{
    const s32 input = kind == Kind::Pad ? binding.pad : static_cast<s32>(binding.key);
    std::snprintf(out, size, "%s %s %s", binding.name, KindName(kind), ::InputName(kind, input));
}

// The binding whose action name a line starts with, the longest one that does (an action's name has spaces in it), and where the
// rest of the line begins after it. Null when the line is no action's
Binding* BindingAtLineStart(const char*& rest, const char* line)
{
    while (*line == ' ' || *line == '\t')
    {
        line++;
    }

    Binding* best = nullptr;
    size_t bestLength = 0;
    for (Binding& candidate : g_Bindings)
    {
        const size_t length = std::strlen(candidate.name);
        if (length > bestLength && std::strncmp(line, candidate.name, length) == 0 &&
            (line[length] == ' ' || line[length] == '\t'))
        {
            best = &candidate;
            bestLength = length;
        }
    }

    if (best == nullptr)
    {
        return nullptr;
    }

    rest = line + bestLength;
    while (*rest == ' ' || *rest == '\t')
    {
        rest++;
    }

    return best;
}

// The binding a line is about: its action's name, then the kind's word (a line with no kind word, from the port's first format, is
// a key's). Returns false for a line that is no binding's
bool LineIsBinding(const char* line, const Binding& binding, Kind kind)
{
    const char* rest = line;
    if (BindingAtLineStart(rest, line) != &binding)
    {
        return false;
    }

    // The kind's word, when the line has one
    const char* second = rest;
    while (*rest != '\0' && *rest != ' ' && *rest != '\t' && *rest != '\n' && *rest != '\r')
    {
        rest++;
    }

    const size_t length = static_cast<size_t>(rest - second);
    if (length == 3 && std::strncmp(second, "key", 3) == 0)
    {
        return kind == Kind::Key;
    }

    if (length == 3 && std::strncmp(second, "pad", 3) == 0)
    {
        return kind == Kind::Pad;
    }

    // No kind word: a key's line
    return kind == Kind::Key;
}

// The file's line placed, replacing the first line of that action's and kind's (later ones dropped) and appended when the file has
// none
void PlaceBinding(FileLines& lines, const Binding& binding, Kind kind)
{
    char text[LineLength];
    WriteBindingLine(text, sizeof(text), binding, kind);

    bool placed = false;
    for (u32 index = 0; index < lines.count;)
    {
        if (!LineIsBinding(lines.text[index], binding, kind))
        {
            index++;
            continue;
        }

        if (!placed)
        {
            std::snprintf(lines.text[index], LineLength, "%s", text);
            placed = true;
            index++;
            continue;
        }

        for (u32 other = index; other + 1 < lines.count; other++)
        {
            std::memcpy(lines.text[other], lines.text[other + 1], LineLength);
        }
        lines.count--;
    }

    if (!placed && lines.count < MaxLines)
    {
        std::snprintf(lines.text[lines.count++], LineLength, "%s", text);
    }
}

// The file as the port starts it: a comment, then every action's two bindings as they default
void AddHeader(FileLines& lines)
{
    static const char* const Header[] = {
        "# Crash Twinsanity: the controller's bindings, one to a line as \"<action> key|pad <input>\".",
        "# A key is SDL's name for it (space, return, left shift, f1, a, 1, keypad 5); a pad input is",
        "# south, east, west, north, left shoulder, right shoulder, left trigger, right trigger, back,",
        "# start, left stick, right stick, dpad up, dpad down, dpad left, dpad right. \"none\" leaves an",
        "# action to the other kind of input. The options' controller page writes the file back, and the",
        "# lines this build has no action for are kept. The page's own escape and back end a capture",
        "# instead of being bound.",
    };

    for (const char* line : Header)
    {
        if (lines.count < MaxLines)
        {
            std::snprintf(lines.text[lines.count++], LineLength, "%s", line);
        }
    }
}

// The bindings written to the file: its own lines merged with them, so a player's comments and the lines of actions this build
// doesn't know survive. A file that isn't there is made with the header and the defaults
void Save()
{
    char path[PathLength];
    KeybindsPath(path);

    static FileLines lines;
    lines.count = 0;
    bool made = true;
    if (std::FILE* file = std::fopen(path, "rb"))
    {
        made = false;
        while (lines.count < MaxLines && std::fgets(lines.text[lines.count], LineLength, file) != nullptr)
        {
            // The newline is the writer's, so the lines are kept without it
            char* end = lines.text[lines.count] + std::strlen(lines.text[lines.count]);
            while (end > lines.text[lines.count] && (end[-1] == '\n' || end[-1] == '\r'))
            {
                *--end = '\0';
            }
            lines.count++;
        }
        std::fclose(file);
    }

    if (made)
    {
        AddHeader(lines);
    }

    for (const Binding& binding : g_Bindings)
    {
        PlaceBinding(lines, binding, Kind::Key);
        PlaceBinding(lines, binding, Kind::Pad);
    }

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "keybinds: can't write %s\n", path);
        return;
    }

    // The lines are kept without their newlines, and written with one each
    for (u32 index = 0; index < lines.count; index++)
    {
        std::fprintf(file, "%s\n", lines.text[index]);
    }

    std::fclose(file);
    std::fprintf(stderr, "keybinds: %s %s\n", path, made ? "written with the defaults" : "written");
}

// The file read over the defaults: a line's input replaces its action's of that kind; a line of an action this build doesn't have,
// or of an input it has no name for, is reported and left alone (the rest of the file still loads)
void ReadFile(const char* path)
{
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        Save();
        return;
    }

    char line[LineLength];
    u32 lineNumber = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        lineNumber++;
        // A comment, a blank line and a line of the file's own are passed over
        char* text = line;
        while (*text == ' ' || *text == '\t')
        {
            text++;
        }
        if (*text == '\0' || *text == '\n' || *text == '\r' || *text == '#')
        {
            continue;
        }

        // The action's name, which has spaces in it, and the rest of the line after it
        const char* rest = nullptr;
        Binding* binding = BindingAtLineStart(rest, line);
        if (binding == nullptr)
        {
            std::fprintf(stderr, "keybinds: %s line %u: no action this build has starts the line\n", path, lineNumber);
            continue;
        }

        // The kind's word, when the line has one, then the input's name to the line's end, its newline gone and its runs of spaces
        // and tabs collapsed to one, so a line's columns can be lined up and still read
        const char* cursor = rest;
        Kind kind = Kind::Key;
        if ((cursor[0] == 'k' && std::strncmp(cursor, "key", 3) == 0) || (cursor[0] == 'p' && std::strncmp(cursor, "pad", 3) == 0))
        {
            kind = cursor[0] == 'p' ? Kind::Pad : Kind::Key;
            cursor += 3;
            while (*cursor == ' ' || *cursor == '\t')
            {
                cursor++;
            }
        }

        char input[LineLength];
        u32 length = 0;
        for (; *cursor != '\0'; cursor++)
        {
            const bool space = *cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r';
            if (!space)
            {
                input[length++] = *cursor;
            }
            else if (length > 0 && input[length - 1] != ' ')
            {
                input[length++] = ' ';
            }
        }
        while (length > 0 && input[length - 1] == ' ')
        {
            length--;
        }
        input[length] = '\0';

        if (length == 0)
        {
            std::fprintf(stderr, "keybinds: %s line %u: no input given, %s stays on %s\n", path, lineNumber, binding->name,
                         ::InputName(kind, kind == Kind::Pad ? binding->pad : static_cast<s32>(binding->key)));
            continue;
        }

        const s32 value = InputFromName(kind, input);
        if (value < 0)
        {
            std::fprintf(stderr, "keybinds: %s line %u: no %s input called \"%s\", %s stays on %s\n", path, lineNumber,
                         KindName(kind), input, binding->name,
                         ::InputName(kind, kind == Kind::Pad ? binding->pad : static_cast<s32>(binding->key)));
            continue;
        }

        if (kind == Kind::Pad)
        {
            binding->pad = value;
        }
        else
        {
            binding->key = static_cast<SDL_Scancode>(value);
        }
    }

    std::fclose(file);
    std::fprintf(stderr, "keybinds: %s read\n", path);
}
}

u32 Platform::Keybinds::Actions()
{
    return ActionCount;
}

const char* Platform::Keybinds::ActionName(Action action)
{
    const u32 index = static_cast<u32>(action);
    return index < ActionCount ? g_Bindings[index].name : "";
}

s32 Platform::Keybinds::Binding(Action action, Kind kind)
{
    const u32 index = static_cast<u32>(action);
    if (index >= ActionCount)
    {
        return 0;
    }

    if (kind == Kind::Pad)
    {
        return g_Bindings[index].pad;
    }

    return kind == Kind::Key ? static_cast<s32>(g_Bindings[index].key) : 0;
}

void Platform::Keybinds::SetBinding(Action action, Kind kind, s32 input)
{
    const u32 index = static_cast<u32>(action);
    if (index >= ActionCount)
    {
        return;
    }

    if (kind == Kind::Pad)
    {
        g_Bindings[index].pad = input;
    }
    else if (kind == Kind::Key)
    {
        g_Bindings[index].key = static_cast<SDL_Scancode>(input);
    }
}

const char* Platform::Keybinds::InputName(Kind kind, s32 input)
{
    return ::InputName(kind, input);
}

void Platform::Keybinds::Save()
{
    ::Save();
}

bool Platform::Keybinds::Taken(Kind* kind, s32* input)
{
    if (kind != nullptr)
    {
        *kind = g_TakenKind;
    }
    if (input != nullptr)
    {
        *input = g_TakenInput;
    }

    return g_Taken;
}

void Platform::Keybinds::ClearTaken()
{
    g_Taken = false;
    g_TakenInput = 0;
}

bool Platform::Keybinds::IsCancel(Kind kind, s32 input)
{
    // The two the page keeps for itself: escape ends a capture, and so does a pad's back button (a keyboard's escape is the only
    // key that does, so a pad's back is only the cancel when it is not bound to an action)
    return (kind == Kind::Key && input == static_cast<s32>(SDL_SCANCODE_ESCAPE)) ||
           (kind == Kind::Pad && input == Platform::Keybinds::PadBack);
}

void Platform::Keybinds::NoteKey(s32 scancode)
{
    if (!g_Taken && scancode != 0)
    {
        g_Taken = true;
        g_TakenKind = Kind::Key;
        g_TakenInput = scancode;
    }
}

void Platform::Keybinds::NotePadInput(s32 input)
{
    if (!g_Taken && input != 0)
    {
        g_Taken = true;
        g_TakenKind = Kind::Pad;
        g_TakenInput = input;
    }
}

void Platform::Keybinds::Load()
{
    if (g_Loaded)
    {
        return;
    }

    // Set before the file is read: a read that fails leaves the defaults, and the next pad read must not try the file again
    g_Loaded = true;
    char path[PathLength];
    KeybindsPath(path);
    ReadFile(path);
}
