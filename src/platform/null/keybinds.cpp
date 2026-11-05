// A platform with no rebinding answers no actions, which keeps the options' controller page off its menus
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
