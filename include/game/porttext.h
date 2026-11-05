// The port's own menu lines. A text number from PortTextFirst up is one of these, and GameText answers it from here (language.h),
// so a row's label, a label widget or a page's title names one exactly as it names a line of the disc's text files. The disc's
// own table ends well below 0x800 (its last texts are the galleries', in the 0x2xx), so nothing retail is displaced.
//
// The lines keep the disc's languages: the slots the texts are read in (english 0, french 1, german 2, spanish 3, italian 4), with
// english for a line a language does not carry. Values a row shows rather than names (a
// resolution's pixels, a multiplier, a rate) are the host's own words and stay as they are.
#pragma once

#include "common.h"

enum PortText : u32
{
    PortTextFirst = 0x800,
    PortTextResolution = PortTextFirst,
    PortTextWindow,
    PortTextFrameRate,
    PortTextAspect,
    PortTextRenderScale,
    PortTextAntiAliasing,
    PortTextPopIn,
    PortTextVoices,
    PortTextTexts,
    PortTextExit,
    PortTextCloseGame,
    PortTextExitQuestion,
    PortTextCheckingSaves,
    PortTextCreateFolderFailed,
    PortTextLoadingSaves,
    PortTextNoSaveCreated,
    PortTextAreYouSure,
    PortTextRemapControls,
    PortTextPressInput,
    PortTextDeadZone,
    PortTextCard,
    PortTextCount,
};

// The line in the texts' language (the slot the disc's text files were read from), english where that language has none
const char* PortTextString(u32 text);
