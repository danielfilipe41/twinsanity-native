// The pickers: SDL's own dialogs, which every SDL3 host has (Windows' file dialog, Linux's portal/GTK dialog, macOS's panel,
// Android's storage access framework, the browser's where it exists). They answer from the event loop, not from the call, so
// the loop is pumped here until they do: nothing else of the game is running yet. The folder picker asks where the disc is;
// the file picker asks which disc image, when the folder the player chose holds more than one.
#include "platform/gamedata.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>

namespace
{
// What the player is asked for, in the dialog's own title (the platforms that show one; each may ignore any property)
constexpr const char* FolderTitle = "Point to the extracted disc folder, or the folder holding its .iso";
constexpr const char* ImageTitle = "Choose the disc image to play";

struct Picked
{
    // The callback may come from another thread, so the answer is published through this flag
    std::atomic<bool> answered{false};
    // Empty on a cancel, and on a failure with SDL's message saying which
    char folder[1024] = {};
};

void SDLCALL PickedFolder(void* userdata, const char* const* files, int)
{
    auto* picked = static_cast<Picked*>(userdata);
    picked->folder[0] = '\0';
    if (files != nullptr && files[0] != nullptr)
    {
        std::snprintf(picked->folder, sizeof(picked->folder), "%s", files[0]);
    }
    picked->answered.store(true, std::memory_order_release);
}
}

bool Platform::GameData::PickFolder(const char* defaultLocation, char* path, u32 size, void* window)
{
    if (path == nullptr || size == 0)
    {
        return false;
    }

    Picked picked;
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
    {
        std::fprintf(stderr, "gamedata: no folder picker: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, FolderTitle);
    if (defaultLocation != nullptr && defaultLocation[0] != '\0')
    {
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, defaultLocation);
    }
    SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    if (window != nullptr)
    {
        // A child of the run's window: the backends that put a dialog over it need one (and this port asks before the game can
        // draw anything, so the window is the only one there is)
        SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    }

    SDL_ClearError();
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, PickedFolder, &picked, properties);

    while (!picked.answered.load(std::memory_order_acquire))
    {
        // The dialog's answer arrives through the event queue; there is no window yet, so the events are only discarded
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 100);
    }
    SDL_DestroyProperties(properties);

    if (picked.folder[0] == '\0')
    {
        // A host without a picker answers immediately with SDL's reason; a cancel answers with nothing to say
        const char* reason = SDL_GetError();
        if (reason != nullptr && reason[0] != '\0')
        {
            std::fprintf(stderr, "gamedata: the folder picker gave no folder: %s\n", reason);
        }
        return false;
    }

    std::snprintf(path, size, "%s", picked.folder);
    return true;
}

bool Platform::GameData::PickImage(const char* defaultLocation, char* path, u32 size, void* window)
{
    if (path == nullptr || size == 0)
    {
        return false;
    }

    // One filter, the two layouts a disc image comes in: a plain .iso and a raw .bin
    static const SDL_DialogFileFilter filters[] = {{"Disc images", "iso;bin"}};

    Picked picked;
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
    {
        std::fprintf(stderr, "gamedata: no file picker: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, ImageTitle);
    if (defaultLocation != nullptr && defaultLocation[0] != '\0')
    {
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, defaultLocation);
    }
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
    SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    if (window != nullptr)
    {
        SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    }

    SDL_ClearError();
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, PickedFolder, &picked, properties);

    while (!picked.answered.load(std::memory_order_acquire))
    {
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 100);
    }
    SDL_DestroyProperties(properties);

    if (picked.folder[0] == '\0')
    {
        const char* reason = SDL_GetError();
        if (reason != nullptr && reason[0] != '\0')
        {
            std::fprintf(stderr, "gamedata: the file picker gave no image: %s\n", reason);
        }
        return false;
    }

    std::snprintf(path, size, "%s", picked.folder);
    return true;
}
