// The PS2 has no rebinding: its pads are the console's own, mapped by the game's controller code, so this answers no actions and
// the options' controller page is not offered. The file and the page are the port's
#include "platform/keybinds.h"

u32 Platform::Keybinds::Actions()
{
    return 0;
}

void Platform::Keybinds::Load()
{
}

const char* Platform::Keybinds::ActionName(Action)
{
    return "";
}

s32 Platform::Keybinds::Binding(Action, Kind)
{
    return 0;
}

void Platform::Keybinds::SetBinding(Action, Kind, s32)
{
}

const char* Platform::Keybinds::InputName(Kind, s32)
{
    return "none";
}

void Platform::Keybinds::Save()
{
}

bool Platform::Keybinds::Taken(Kind*, s32*)
{
    return false;
}

void Platform::Keybinds::ClearTaken()
{
}

bool Platform::Keybinds::IsCancel(Kind, s32)
{
    return false;
}

void Platform::Keybinds::NoteKey(s32)
{
}

void Platform::Keybinds::NotePadInput(s32)
{
}
