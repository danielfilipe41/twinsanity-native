#include "game/olegpages.h"

#include "game/clock.h"
#include "game/colour.h"
#include "game/controllers.h"
#include "game/gamecontroller.h"
#include "game/math.h"
#include "game/memory.h"
#include "game/oleg.h"
#include "game/porttext.h"

#include "platform/display.h"
#include "platform/keybinds.h"
#include "platform/pads.h"
#include "platform/system.h"
#include "game/pads.h"
#include "game/renderer.h"
#include "game/savemanager.h"
#include "game/sound.h"
#include "game/widgets.h"

#include <cstdio>
// The options the front end sets are also the ones a save carries: SaveController::TakeOptions copies the live ones when a save
// is written, and the game applies a save's options when it loads one. Without this, entering a game would apply the last
// save's options over what the player has just chosen in the menus
static void OptionsChanged()
{
    if (G_GameController != nullptr)
    {
        G_GameController->saveController.TakeOptions();
    }
}


extern "C"
{
    // The header's constants the start-up sets for this file, which nothing reads: up (0, 1, 0, 1), the UI's shadow (its offset
    // and colour) and 45 degrees (65536ths); and its own: (0, 1, -1, 1), (0, 1, 0, 1), 0, white
    // and black
    extern Vector4 g_OlegPagesUnusedUp RETAIL(D_0030BDE0);
    extern f32 g_OlegPagesUnusedShadowX RETAIL(D_0030A5D8);
    extern f32 g_OlegPagesUnusedShadowY RETAIL(D_0030A5DC);
    extern u32 g_OlegPagesUnusedShadowColour RETAIL(D_0030A5E0);
    extern s32 g_OlegPagesUnusedAngle RETAIL(D_0030A5E8);
    extern Vector4 g_OlegPagesUnusedUpBack RETAIL(D_0030BDF0);
    extern Vector4 g_OlegPagesUnusedUp2 RETAIL(D_0030BE00);
    extern u32 g_OlegPagesUnusedZero RETAIL(D_0030A5F0);
    extern u32 g_OlegPagesUnusedWhite RETAIL(D_0030A5F8);
    extern u32 g_OlegPagesUnusedBlack RETAIL(D_0030A5FC);
}

namespace
{
// Memory for an object of a type
template <typename T> T* Allocate()
{
    return static_cast<T*>(MemoryAllocate(sizeof(T)));
}

// The main menu's title and its items' texts
constexpr const char* MainMenuTitle = "crash twinsanity";
constexpr u32 NewGameText = 9;
constexpr u32 LoadGameText = 0xA;
constexpr u32 OptionsText = 0xB;
// Its items' IDs
constexpr u32 NewGameItem = 0;
constexpr u32 LoadGameItem = 2;
constexpr u32 OptionsItem = 0x1E;
// The exit row: the main menu's own, and the pause menu's quitting turned into one
constexpr u32 ExitItem = 0x1F;


// What the pause menu's exit question says, and the quit page it is asked on (retail's, found where the pause menu makes it: it is
// the page's yes item that is turned from returning to the title into ending the process, and the quit row turns it back)

QuitPage* g_QuitPage = nullptr;
// A screen of OLEG's is shown over half a second, the others hidden; an item's gallery or movie starts half a second after it's
// picked
constexpr f32 ScreenChangeSeconds = 0.5f;
constexpr f32 ItemDelaySeconds = 0.5f;
// "continue", the ID of the items going back to a page's parent, and how far a frame moves the screen
constexpr u32 ContinueText = 5;
constexpr u32 ParentBackItem = 0x1D;
constexpr f32 ScreenStepX = Rounded(0.015);
constexpr f32 ScreenStepY = Rounded(0.03);

// The game over page: its title, "continue" (restarting from the save) and "quit game"
constexpr u32 GameOverTitle = 0x44;
constexpr u32 RestartItemId = 9;
constexpr u32 QuitGameText = 0x1A;
constexpr u32 QuitGameItem = 0xB;
// The confirmations: "yes" and "no" (its ID 0x1C), the quit's yes (its ID 0xB) and the disable autosave's (0xA)
constexpr u32 YesText = 3;
constexpr u32 NoText = 2;
constexpr u32 NoItem = 0x1C;
constexpr u32 QuitYesItem = 0xB;
constexpr u32 DisableAutosaveYesItem = 0xA;
// The notices' "continue" (its ID 0xC)
constexpr u32 NoticeContinueItem = 0xC;
// The options: the pages' titles, their links (IDs 6, 7, 8) and back (0x1C on the sub-pages)
constexpr u32 GraphicOptionsText = 0xD;
constexpr u32 SoundOptionsText = 0xE;
constexpr u32 GameOptionsText = 0xF;
constexpr u32 GraphicOptionsItem = 6;
constexpr u32 SoundOptionsItem = 7;
constexpr u32 GameOptionsItem = 8;
// The port's own controller page (its link, and its rows are the actions' indexes past the retail items')
constexpr u32 ControllerOptionsItem = 0x2A;
constexpr u32 BackText = 4;
constexpr u32 SubPageBackItem = 0x1C;
// Their items: vibration (no and yes), centring the screen, widescreen (off and on), the volumes (0 to 10), the output type and
// its choices (mono, stereo, Dolby Pro Logic II)
constexpr u32 VibrationText = 0x15;
constexpr u32 VibrationItem = 0x1A;
constexpr u32 CentreScreenText = 0x11;
constexpr u32 CentreScreenItem = 0x15;
constexpr u32 WidescreenText = 0x10;
constexpr u32 WidescreenItem = 0x16;
constexpr u32 OffText = 0;
constexpr u32 OnText = 1;
constexpr u32 EffectsVolumeText = 0x12;
constexpr u32 EffectsVolumeItem = 0x17;
constexpr u32 MusicVolumeText = 0x13;
constexpr u32 MusicVolumeItem = 0x18;
constexpr s32 MaxVolume = 10;
constexpr f32 VolumeStep = Rounded(0.1);
constexpr u32 OutputTypeText = 0x14;
constexpr u32 OutputTypeItem = 0x19;
constexpr u32 OutputTypes = 3;
constexpr u32 MonoText = 6;
// The display's own rows on the graphic options page: the host's picture, not the game's options. Each is a choice of the
// names the host itself offers (Platform::Display holds them, and the words the settings file records), and the page hands a
// changed choice back to the host, which applies it and keeps it. Their IDs sit past the retail page's (the retail items are
// 0x14 to 0x1D)
constexpr u32 ResolutionItemId = 0x20;
constexpr u32 WindowItemId = 0x21;
constexpr u32 FrameRateItemId = 0x22;
constexpr u32 VsyncItemId = 0x23;
constexpr u32 AspectItemId = 0x24;
constexpr u32 RenderScaleItemId = 0x25;
constexpr u32 AntiAliasItemId = 0x26;
constexpr u32 DrawDistanceItemId = 0x29;

// The host's value as a choice index, and the choice index back: the antialiasing's is its enum
u32 AntiAliasChoice()
{
    return static_cast<u32>(Platform::Display::AntiAliasing());
}

void SetAntiAliasChoice(u32 index)
{
    Platform::Display::SetAntiAliasing(static_cast<AntiAlias>(index));
}

// A choice row, one per host setting: its label, its item ID, the names the host offers (a static array of the host's, which
// outlives the menu), how many and how they are read and written
struct DisplayRow
{
    u32 text;
    u32 item;
    const char* const* (*names)();
    u32 (*count)();
    u32 (*value)();
    void (*set)(u32 index);
};

const DisplayRow DisplayRows[] = {
    {PortTextResolution, ResolutionItemId, Platform::Display::ResolutionNames, Platform::Display::ResolutionCount,
     Platform::Display::Resolution, Platform::Display::SetResolution},
    {PortTextWindow, WindowItemId, Platform::Display::WindowModeNames, Platform::Display::WindowModeCount, Platform::Display::WindowMode,
     Platform::Display::SetWindowMode},
    {PortTextFrameRate, FrameRateItemId, Platform::Display::FrameRateNames, Platform::Display::FrameRateCount,
     Platform::Display::FrameRate, Platform::Display::SetFrameRateIndex},
    {PortTextAspect, AspectItemId, Platform::Display::AspectNames, Platform::Display::AspectCount, Platform::Display::Aspect,
     Platform::Display::SetAspect},
    {PortTextRenderScale, RenderScaleItemId, Platform::Display::ScaleNames, Platform::Display::ScaleCount, Platform::Display::Scale,
     Platform::Display::SetScale},
    {PortTextAntiAliasing, AntiAliasItemId, Platform::Display::AntiAliasNames, Platform::Display::AntiAliasCount, AntiAliasChoice,
     SetAntiAliasChoice},
    {PortTextPopIn, DrawDistanceItemId, Platform::Display::DrawDistanceNames, Platform::Display::DrawDistanceCount,
     Platform::Display::DrawDistance, Platform::Display::SetDrawDistance},
};

constexpr u32 DisplayRowCount = sizeof(DisplayRows) / sizeof(DisplayRows[0]);

// Where the graphic options page keeps them, once built: the page class is a retail one whose layout is CHECK_SIZE'd, so there
// is no member to add to it, and the rows are read straight from here by its Entered and Frame
MenuItem* g_DisplayItems[DisplayRowCount];
MenuItem* g_VsyncItem = nullptr;

// The language rows on the game options page: the voices' and the texts'. The game puts its languages in place once, as it
// starts (the voices are streamed banks it opened, the texts were read into memory), so a change lands at the next run
constexpr u32 VoicesItemId = 0x27;
constexpr u32 TextsItemId = 0x28;
// The stick dead zone row on the game options page, and the percentages its choices are (steps of five, off to twenty)
constexpr u32 DeadZoneItemId = 0x2B;
constexpr u32 DeadZoneSteps = 5;
constexpr u32 DeadZoneChoices = 5;
// The row's words: the percentages as the row shows them (numbers stay numbers, as the display rows' do)
const char* const DeadZoneNames[DeadZoneChoices] = {"off", "5%", "10%", "15%", "20%"};
MenuItem* g_VoicesItem = nullptr;
MenuItem* g_TextsItem = nullptr;
// The stick dead zone row, on a host whose pads keep one
MenuItem* g_DeadZoneItem = nullptr;
// The cutscene skip row: on by default, off for retail's
constexpr u32 CutsceneSkipItemId = 0x2C;
constexpr u32 CutsceneSkipChoices = 2;
const char* const CutsceneSkipNames[CutsceneSkipChoices] = {"off", "on"};
MenuItem* g_CutsceneSkipItem = nullptr;

// The two applied as they are stepped. The texts follow the voices while they were the same language, and only a player who
// picks a different one for them splits the two (which is what a disc with translated-in-another-slot texts wants)
void ApplyLanguageChoice(u32 voices, u32 texts)
{
    const u32 oldVoices = Platform::System::LanguageChoice();
    const u32 oldTexts = Platform::System::TextLanguageChoice();
    if (voices != oldVoices)
    {
        Platform::System::SetLanguageChoice(voices);
        if (texts == oldVoices)
        {
            Platform::System::SetTextLanguageChoice(voices);
        }
    }

    if (texts != oldTexts && texts != oldVoices)
    {
        Platform::System::SetTextLanguageChoice(texts);
    }
}

// A choice row named by the host's own words: the names are the host's statics and the array holding them is the item's, which
// its Destroy frees (the same allocator ChoiceItem's own choice texts use)
ChoiceItem* ConstructNamedChoice(ChoiceItem* item, u32 text, u32 id, const char* const* names, u32 choices, s32 value)
{
    ChoiceItem::Construct(item, text, id, choices, 1, 0, MenuPlayers);
    item->usesStrings = 1;
    item->strings = static_cast<const char**>(MemoryAllocate2(choices * sizeof(const char*)));
    for (u32 choice = 0; choice < choices; choice++)
    {
        item->strings[choice] = names[choice];
    }

    item->SetRange(0, static_cast<s32>(choices) - 1, 1, value);
    return item;
}

// The pause menu: its title, its pages' (save game, load game), its items' texts and IDs
constexpr u32 PauseTitle = 0x18;
constexpr u32 SaveGameText = 0x19;
constexpr u32 SaveGameItem = 1;
constexpr u32 DisableAutosaveText = 0x16;
constexpr u32 DisableAutosaveItem = 0xA;
constexpr u32 ResumeText = 0x1B;
constexpr u32 ResumeItem = 0xC;
// The extras: their title, the gem extras' titles (the gem's after the first), links (IDs 0xE on) and items (16 each, their IDs
// their indexes), the complete game's movie item and its texts before and once the game's done, and resume's ID
constexpr u32 ExtrasTitle = 0x17;
constexpr u32 GemExtrasTitle = 0x59;
constexpr u32 GemExtrasLink = 0xE;
constexpr u32 GemExtrasItems = 16;
constexpr u32 CompleteItem = 0x14;
constexpr u32 CompleteText = 0x61;
constexpr u32 NotCompleteText = 0x58;
constexpr u32 ExtrasResumeItem = 0x1C;
// The galleries' first texts and their pictures' names
constexpr u32 BossesText = 0x62;
constexpr u32 ConceptText = 0x72;
constexpr u32 EnemiesText = 0x82;
constexpr u32 MoviesText = 0x92;
constexpr u32 UnseenText = 0xA2;
constexpr u32 StoryboardsText = 0x47;
constexpr const char* BossesName = "Extras\\Bosses\\Boss";
constexpr const char* ConceptName = "Extras\\Concept\\Concept";
constexpr const char* EnemiesName = "Extras\\Enemies\\Enemy";
constexpr const char* UnseenName = "Extras\\Unseen\\Unseen";
// The movies' movies: the bonus movies and the story's
constexpr u32 ExtrasMovies[GemExtrasItems] = {
    GameMovie::FirstBonus, GameMovie::FirstBonus + 1, GameMovie::FirstBonus + 2, GameMovie::FirstBonus + 3,
    GameMovie::Intro,      GameMovie::Intro + 1,      GameMovie::Intro + 2,      GameMovie::Intro + 3,
    GameMovie::Intro + 4,  GameMovie::Intro + 5,      GameMovie::Intro + 6,      GameMovie::Intro + 7,
    GameMovie::Intro + 8,  GameMovie::Intro + 9,      GameMovie::Intro + 10,     GameMovie::Intro + 11,
};
// The movies needing the story done: from the 14th, once the story's area is 24
constexpr u32 FirstStoryEndMovie = 13;
constexpr u32 StoryDoneArea = 24;
// The levels' storyboards: their pictures' names and how many each has
struct Storyboard
{
    const char* name;
    u32 pictures;
};
constexpr Storyboard Storyboards[GemExtrasItems] = {
    {"Extras\\Storyboards\\02-Jungle\\Jungle", 8},           {"Extras\\Storyboards\\01-NSanity\\NSanity", 11},
    {"Extras\\Storyboards\\03-Cavern\\Cavern", 14},          {"Extras\\Storyboards\\04-Totem\\Totem", 15},
    {"Extras\\Storyboards\\06-IceClimb\\IceClimb", 17},      {"Extras\\Storyboards\\05-IceHub\\IceHub", 15},
    {"Extras\\Storyboards\\07-Slip\\Slip", 9},               {"Extras\\Storyboards\\08-HighSeas\\HighSeas", 14},
    {"Extras\\Storyboards\\09-Academy\\Academy", 8},         {"Extras\\Storyboards\\10-Boiler\\Boiler", 7},
    {"Extras\\Storyboards\\11-Classroom\\Classroom", 9},     {"Extras\\Storyboards\\12-Rooftop\\Rooftop", 7},
    {"Extras\\Storyboards\\13-Twinsanity\\Twinsanity", 5},   {"Extras\\Storyboards\\14-Rockslide\\Rockslide", 6},
    {"Extras\\Storyboards\\15-Pursuit\\Pursuit", 6},         {"Extras\\Storyboards\\16-AntAgony\\AntAgony", 10},
};
// The levels pages: each world's layout (the columns' places, the first row's height and the rows' spacing) and its four levels'
// areas (the items' IDs)
constexpr u32 LevelsPerWorld = 4;
struct LevelsLayout
{
    f32 evenX;
    f32 oddX;
    f32 top;
    f32 spacing;
    u32 levels[LevelsPerWorld];
};
constexpr u32 Worlds = 4;
constexpr LevelsLayout LevelsLayouts[Worlds] = {
    {Rounded(0.6), Rounded(0.4), Rounded(0.15), Rounded(0.15), {0, 1, 3, 4}},
    {Rounded(0.4), Rounded(0.6), Rounded(0.15), Rounded(0.15), {6, 7, 9, 10}},
    {Rounded(0.6), Rounded(0.4), Rounded(0.15), Rounded(0.15), {13, 15, 17, 18}},
    {Rounded(0.4), Rounded(0.6), Rounded(0.15), Rounded(0.15), {20, 21, 22, 23}},
};
// Past the last world (never asked for): the retail code's layout, with no levels (the retail code's are what its stack held)
constexpr LevelsLayout NoWorld = {Rounded(0.4), Rounded(0.6), Rounded(0.2), 0.125f, {}};
// The levels' and the save slots' rings (panels: AddPanelRings' discs) in the middle of their widgets, the level widgets' size
// and the levels' discs' radii
constexpr Vector2 LevelWidgetScale = {Rounded(0.6), Rounded(0.1)};
constexpr Vector2 LevelPanelRadii = {Rounded(0.3), Rounded(0.05)};
// The save slots page: a save code item per slot (IDs from 0x100), and a ring panel (0.2 by 0.075) behind each slot's widget, in
// two staggered columns (the even slots' sliding in from the left)
constexpr u32 SlotItemIds = 0x100;
constexpr f32 SlotEvenX = Rounded(0.3);
constexpr f32 SlotOddX = Rounded(0.7);
constexpr f32 SlotsTop = Rounded(0.26);
constexpr f32 SlotSpacing = Rounded(0.09);
constexpr Vector2 SlotPanelRadii = {Rounded(0.2), Rounded(0.075)};

// Every other screen hidden and the screen shown, over half a second
void ShowScreen(OLEG& oleg, u32 screen)
{
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    oleg.Show(oleg.screens[screen], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
}

// A gallery item of a text and ID: its pictures (first to last) and their name
GalleryItem* MakeGalleryItem(u32 text, u32 id, const char* name, u32 first, u32 last)
{
    auto* item = Allocate<GalleryItem>();
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->vtable = g_GalleryItemVTable;
    StringConstruct(&item->name, name);
    item->first = 0;
    item->last = 0;
    item->unused16[0] = 0;
    item->unused16[1] = 0;
    item->first = static_cast<u8>(first);
    item->last = static_cast<u8>(last);
    return item;
}

// A gem extras page made: titled by its gem, its vtable, back (made first, added last)
BackItem* StartGemExtras(GemExtrasPage* page, u32 gem, MenuPage* parent, const GccVTableEntry* vtable)
{
    OlegPage::ConstructTitled(page, GemExtrasTitle + gem, parent);
    page->gem = gem;
    page->vtable = vtable;
    return BackItem::Construct(Allocate<BackItem>(), BackText, ParentBackItem, parent, MenuPlayers);
}

// The galleries of a gem extras page: each of so many pictures, numbered on from the last
void AddGalleries(GemExtrasPage* page, u32 text, const char* name, u32 pictures)
{
    u32 first = 1;
    u32 last = pictures;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MakeGalleryItem(text + item, item, name, first, last));
        first += pictures;
        last += pictures;
    }
}

// A confirmation's yes and its no (back to the parent, selected first)
void AddYesNo(OlegPage* page, u32 yesId, u32 action, MenuPage* parent)
{
    ActionItem* yes = ActionItem::Construct(Allocate<ActionItem>(), YesText, yesId, action);
    BackItem* no = BackItem::Construct(Allocate<BackItem>(), NoText, NoItem, parent, MenuPlayers);
    page->Add(yes);
    page->Add(no);
    page->SetFirstItem(no->Id());
}
}

OlegPage* OlegPage::Construct(OlegPage* page, const char* name, MenuPage* parent)
{
    MenuPage::Construct(page, name, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OlegPageVTable;
    return page;
}

OlegPage* OlegPage::ConstructTitled(OlegPage* page, u32 title, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, title, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OlegPageVTable;
    return page;
}

void OlegPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

NewGamePage* NewGamePage::Construct(NewGamePage* page, MainMenuPage* owner)
{
    OlegPage::ConstructTitled(page, NewGameText, nullptr);
    page->owner = owner;
    page->vtable = g_NewGamePageVTable;
    return page;
}

void NewGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void NewGamePage::Entered(u32, u32)
{
    g_OlegGameController->LoadForNewGame();
}

LoadGamePage* LoadGamePage::Construct(LoadGamePage* page, OlegPage* owner)
{
    OlegPage::ConstructTitled(page, LoadGameText, nullptr);
    page->owner = owner;
    page->vtable = g_LoadGamePageVTable;
    return page;
}

void LoadGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void LoadGamePage::Entered(u32, u32)
{
    g_OlegGameController->LoadSavedGame();
}

ActionItem* ActionItem::Construct(ActionItem* item, u32 text, u32 id, u32 action)
{
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->action = action;
    item->vtable = g_ActionItemVTable;
    return item;
}

void ActionItem::Destroy(u32 flags)
{
    LinkItem::Destroy(flags);
}

MenuPage* ActionItem::Activate(u32 player, MenuPage* page)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    switch (action)
    {
    case ActionPauseMenu:
        controller->ReturnToPauseMenu();
        break;
    case ActionOptions:
        ShowScreen(oleg, OLEG::ScreenOptions);
        break;
    case ActionScreenPosition:
        ShowScreen(oleg, OLEG::ScreenScreenPosition);
        break;
    case ActionQuit:
        // Retail's own question, and the yes returns to the title: the exit question above turns both, so they are turned back
        if (g_QuitPage != nullptr)
        {
            oleg.quitTitle.text = QuitTitleText;
            if (auto* yes = static_cast<ActionItem*>(g_QuitPage->Find(QuitYesItem)))
            {
                yes->action = ActionTitle;
            }
        }

        ShowScreen(oleg, OLEG::ScreenQuit);
        break;
    case ActionTitle:
        controller->QuitToTitle();
        break;
    case ActionDisableAutosave:
        ShowScreen(oleg, OLEG::ScreenDisableAutosave);
        break;
    case ActionStopSaving:
        controller->StopSaving();
        break;
    case ActionLoad:
        controller->LoadSavedGame();
        break;
    case ActionSave:
        controller->SaveFromPause();
        break;
    case ActionExit:
        Platform::System::Exit(0);
        break;
    case ActionAskExit:
        if (g_QuitPage != nullptr)
        {
            // The quit screen's title is a label of its own (oleg.quitTitle, beside the menu): the question is its text
            oleg.quitTitle.text = PortTextExitQuestion;
            if (auto* yes = static_cast<ActionItem*>(g_QuitPage->Find(QuitYesItem)))
            {
                yes->action = ActionExit;
            }
        }

        ShowScreen(oleg, OLEG::ScreenQuit);
        break;
    case ActionBind:
        // The port's controller page, waiting for the row's next input
        static_cast<ControllerOptionsPage*>(page)->BeginCapture(bits.id);
        break;
    default:
        break;
    }

    return MenuItem::Activate(player, page);
}

MainMenuPage* MainMenuPage::Construct(MainMenuPage* page)
{
    OlegPage::Construct(page, MainMenuTitle, &g_ResumePage);
    page->vtable = g_MainMenuPageVTable;
    NewGamePage::Construct(&page->newGame, page);
    LoadGamePage::Construct(&page->loadGame, page);
    LinkItem* newGame = LinkItem::Construct(Allocate<LinkItem>(), NewGameText, NewGameItem,
                                            &page->newGame, MenuPlayers);
    LinkItem* loadGame = LinkItem::Construct(Allocate<LinkItem>(), LoadGameText, LoadGameItem,
                                             &page->loadGame, MenuPlayers);
    ActionItem* options = ActionItem::Construct(Allocate<ActionItem>(), OptionsText,
                                                OptionsItem, ActionItem::ActionOptions);
    ActionItem* exit = ActionItem::Construct(Allocate<ActionItem>(), PortTextExit, ExitItem, ActionItem::ActionExit);
    page->Add(newGame);
    page->Add(loadGame);
    page->Add(options);
    page->Add(exit);
    return page;
}

void MainMenuPage::Destroy(u32 flags)
{
    loadGame.OlegPage::Destroy(DestroyOnly);
    newGame.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void MainMenuPage::Frame(u32, MenuInput*, MenuSounds*)
{
}

ScreenPositionPage* ScreenPositionPage::Construct(ScreenPositionPage* page, MenuPage* parent)
{
    OlegPage::Construct(page, "", parent);
    page->vtable = g_ScreenPositionPageVTable;
    BackItem* item = BackItem::Construct(Allocate<BackItem>(), ContinueText, ParentBackItem,
                                         parent, MenuPlayers);
    page->Add(item);
    return page;
}

void ScreenPositionPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void ScreenPositionPage::Frame(u32, MenuInput* input, MenuSounds*)
{
    GameRendererController* renderer = G_GameRendererController;
    Vector2 offset;
    CopyVector2(&offset, &renderer->screenOffset);
    if (input->Has(MenuInput::ActionLeft, 0) != 0)
    {
        offset.x -= ScreenStepX;
    }
    else if (input->Has(MenuInput::ActionRight, 0) != 0)
    {
        offset.x += ScreenStepX;
    }

    if (input->Has(MenuInput::ActionUp, 0) != 0)
    {
        offset.y -= ScreenStepY;
    }
    else if (input->Has(MenuInput::ActionDown, 0) != 0)
    {
        offset.y += ScreenStepY;
    }

    renderer->SetScreenOffset(&offset);
}

u32 ScreenPositionPage::Back(u32)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    oleg.Show(oleg.screens[OLEG::ScreenOptions], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    return 1;
}

void UnusedPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage::Entered(u32, u32)
{
}

void UnusedPage::Left(u32, u32)
{
}

RestartItem* RestartItem::Construct(RestartItem* item, u32 text, u32 id, u32 entry)
{
    ChoiceItem::Construct(item, text, id, 0, 1, 0, MenuPlayers);
    item->vtable = g_RestartItemVTable;
    item->restart.value = 0;
    item->restart.restarts = 1;
    item->restart.entry = entry;
    return item;
}

void RestartItem::Destroy(u32 flags)
{
    ChoiceItem::Destroy(flags);
}

MenuPage* RestartItem::Activate(u32, MenuPage*)
{
    if (restart.restarts != 0)
    {
        g_OlegGameController->RequestRestart(restart.entry);
    }

    return nullptr;
}

GameOverPage* GameOverPage::Construct(GameOverPage* page)
{
    OlegPage::ConstructTitled(page, GameOverTitle, nullptr);
    page->vtable = g_GameOverPageVTable;
    RestartItem* restart = RestartItem::Construct(Allocate<RestartItem>(), ContinueText,
                                                  RestartItemId, EntrySaved);
    ActionItem* quit = ActionItem::Construct(Allocate<ActionItem>(), QuitGameText, QuitGameItem,
                                             ActionItem::ActionTitle);
    page->Add(restart);
    page->Add(quit);
    page->SetFirstItem(restart->Id());
    return page;
}

void GameOverPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

DisableAutosavePage* DisableAutosavePage::Construct(DisableAutosavePage* page, MenuPage* parent)
{
    OlegPage::ConstructTitled(page, DisableAutosaveTitleText, parent);
    page->vtable = g_DisableAutosavePageVTable;
    AddYesNo(page, DisableAutosaveYesItem, ActionItem::ActionStopSaving, parent);
    return page;
}

void DisableAutosavePage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

u32 DisableAutosavePage::Back(u32)
{
    ShowScreen(g_OlegGameController->oleg, OLEG::ScreenPauseMenu);
    return 1;
}

QuitPage* QuitPage::Construct(QuitPage* page, MenuPage* parent)
{
    OlegPage::ConstructTitled(page, QuitTitleText, parent);
    page->vtable = g_QuitPageVTable;
    AddYesNo(page, QuitYesItem, ActionItem::ActionTitle, parent);
    // Where the exit question asks (ActionAskExit) and where the quitting's own question is put back
    g_QuitPage = page;
    return page;
}

void QuitPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

u32 QuitPage::Back(u32)
{
    ShowScreen(g_OlegGameController->oleg, OLEG::ScreenPauseMenu);
    return 1;
}

NoticePage* NoticePage::Construct(NoticePage* page, u32 toPauseMenu, MenuPage* parent)
{
    OlegPage::Construct(page, "", parent);
    page->vtable = g_NoticePageVTable;
    MenuItem* item;
    if (toPauseMenu != 0)
    {
        item = ActionItem::Construct(Allocate<ActionItem>(), ContinueText, NoticeContinueItem,
                                     ActionItem::ActionPauseMenu);
    }
    else
    {
        item = BackItem::Construct(Allocate<BackItem>(), ContinueText, NoticeContinueItem,
                                   &g_ResumePage, MenuPlayers);
    }

    page->Add(item);
    return page;
}

void NoticePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void UnusedPage2::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage2::Entered(u32, u32)
{
}

void UnusedPage2::Left(u32, u32)
{
}

void UnusedPage3::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void UnusedPage3::Frame(u32, MenuInput*, MenuSounds*)
{
}

void GameOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void GameOptionsPage::Entered(u32 player, u32)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    Find(VibrationItem)->SetFlag(player, pads->flags.vibration != 0);
    if (g_DeadZoneItem != nullptr)
    {
        g_DeadZoneItem->SetIntValue(player, static_cast<s32>(Platform::Pads::DeadZonePercent() / DeadZoneSteps));
    }
    if (g_CutsceneSkipItem != nullptr)
    {
        g_CutsceneSkipItem->SetIntValue(player, Platform::System::CutsceneSkip() ? 1 : 0);
    }
    if (g_VoicesItem != nullptr)
    {
        g_VoicesItem->SetIntValue(player, static_cast<s32>(Platform::System::LanguageChoice()));
        g_TextsItem->SetIntValue(player, static_cast<s32>(Platform::System::TextLanguageChoice()));
    }
}

void GameOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    u8 vibration;
    Find(VibrationItem)->GetFlag(player, &vibration);
    if (vibration != 0)
    {
        pads->flags.vibration = 1;
    }
    else
    {
        pads->DisableVibration();
    }

    if (g_DeadZoneItem != nullptr)
    {
        // Steps of five percent, applied as the row is stepped (the host writes the file at its next tick)
        s32 deadZone = 0;
        if (g_DeadZoneItem->GetIntValue(player, &deadZone) != 0)
        {
            Platform::Pads::SetDeadZonePercent(static_cast<u32>(deadZone) * DeadZoneSteps);
        }
    }

    if (g_CutsceneSkipItem != nullptr)
    {
        // Applied as the row is stepped: the file keeps it, and the cutscenes after this one answer to it
        s32 cutsceneSkip = 0;
        if (g_CutsceneSkipItem->GetIntValue(player, &cutsceneSkip) != 0)
        {
            Platform::System::SetCutsceneSkip(cutsceneSkip != 0);
        }
    }

    if (g_VoicesItem != nullptr)
    {
        s32 voices = 0;
        s32 texts = 0;
        g_VoicesItem->GetIntValue(player, &voices);
        g_TextsItem->GetIntValue(player, &texts);
        ApplyLanguageChoice(static_cast<u32>(voices), static_cast<u32>(texts));
    }

    OptionsChanged();
}

void GameOptionsPage::Left(u32, u32)
{
}

void GraphicsOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}


void GraphicsOptionsPage::Entered(u32 player, u32)
{
    Find(WidescreenItem)->SetFlag(player, g_WidescreenTv);
    // The display's rows show what the host has now (a host that presents no picture has none of them)
    for (u32 index = 0; index < DisplayRowCount; index++)
    {
        if (g_DisplayItems[index] != nullptr)
        {
            g_DisplayItems[index]->SetIntValue(player, static_cast<s32>(DisplayRows[index].value()));
        }
    }

    if (g_VsyncItem != nullptr)
    {
        g_VsyncItem->SetFlag(player, Platform::Display::Vsync() ? 1 : 0);
    }
}

void GraphicsOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    u8 widescreen;
    Find(WidescreenItem)->GetFlag(player, &widescreen);
    g_WidescreenTv = widescreen;

    // The display's rows as they are stepped: the host compares its own value against the choice, so a frame that changed none
    // of them is a handful of compares and no window work
    for (u32 index = 0; index < DisplayRowCount; index++)
    {
        s32 value = 0;
        if (g_DisplayItems[index] != nullptr && g_DisplayItems[index]->GetIntValue(player, &value) != 0)
        {
            DisplayRows[index].set(static_cast<u32>(value));
        }
    }

    if (g_VsyncItem != nullptr)
    {
        u8 vsync = 0;
        g_VsyncItem->GetFlag(player, &vsync);
        Platform::Display::SetVsync(vsync != 0);
    }

    OptionsChanged();
}

SoundOptionsPage* SoundOptionsPage::Construct(SoundOptionsPage* page, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, SoundOptionsText, MenuPlayers);
    page->parent = parent;
    page->vtable = g_SoundOptionsPageVTable;
    ValueItem* effects = ValueItem::Construct(Allocate<ValueItem>(), EffectsVolumeText, EffectsVolumeItem, 0, MaxVolume, 0, MaxVolume,
                                              MenuPlayers);
    ValueItem* music = ValueItem::Construct(Allocate<ValueItem>(), MusicVolumeText, MusicVolumeItem, 0, MaxVolume, 0, MaxVolume,
                                            MenuPlayers);
    ChoiceItem* output = ChoiceItem::Construct(Allocate<ChoiceItem>(), OutputTypeText, OutputTypeItem, OutputTypes, 1, 0, MenuPlayers);
    BackItem* back = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, parent, MenuPlayers);
    for (u32 choice = 0; choice < OutputTypes; choice++)
    {
        output->SetChoiceText(choice, static_cast<s32>(MonoText + choice));
    }

    page->Add(effects);
    page->Add(music);
    page->Add(output);
    page->Add(back);
    return page;
}

void SoundOptionsPage::Destroy(u32 flags)
{
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void SoundOptionsPage::Entered(u32 player, u32)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    MenuItem* output = Find(OutputTypeItem);
    f32 effectsLevel = GroupVolumeLevel(EffectsGroup);
    f32 musicLevel = GroupVolumeLevel(MusicGroup);
    u32 stereo = g_MusicStereo;
    effects->SetIntValue(player, static_cast<s32>(effectsLevel * MaxVolume + 0.5f));
    music->SetIntValue(player, static_cast<s32>(musicLevel * MaxVolume + 0.5f));
    output->SetIntValue(player, static_cast<s32>(stereo));
}

void SoundOptionsPage::Frame(u32 player, MenuInput*, MenuSounds*)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    s32 value;
    if (Item(selections[player])->Id() == MusicVolumeItem)
    {
        music->GetIntValue(player, &value);
    }
    else
    {
        effects->GetIntValue(player, &value);
    }

    f32 level = static_cast<f32>(value) * VolumeStep;
    SetGroupVolume(level, level, EffectsGroup);
    SetGroupVolume(level, level, SecondEffectsGroup);
    SetGroupVolume(level, level, MovieGroup);
    OptionsChanged();
}

void SoundOptionsPage::Left(u32 player, u32)
{
    MenuItem* effects = Find(EffectsVolumeItem);
    MenuItem* music = Find(MusicVolumeItem);
    MenuItem* output = Find(OutputTypeItem);
    s32 values[3];
    effects->GetIntValue(player, &values[0]);
    music->GetIntValue(player, &values[1]);
    output->GetIntValue(player, &values[2]);
    f32 effectsLevel = static_cast<f32>(values[0]) * VolumeStep;
    f32 musicLevel = static_cast<f32>(values[1]) * VolumeStep;
    SetGroupVolume(effectsLevel, effectsLevel, EffectsGroup);
    SetGroupVolume(effectsLevel, effectsLevel, SecondEffectsGroup);
    SetGroupVolume(effectsLevel, effectsLevel, MovieGroup);
    SetGroupVolume(musicLevel, musicLevel, MusicGroup);
    SetMusicStereo(static_cast<u32>(values[2]));
    OptionsChanged();
}

// The controller page's own vtable: the page class is the port's, so its functions are too. Slot 1 entered, 2 a frame, 3 left,
// 4 destroyed, 5 the back action, 6 and 8 the game's frame begun and ended, 7 the slot nothing calls (the base's do nothing)
namespace
{
ControllerOptionsPage* PageOf(void* page)
{
    return static_cast<ControllerOptionsPage*>(page);
}

void ControllerPageEntered(void* page, u32 player, u32 mode)
{
    PageOf(page)->Entered(player, mode);
}

void ControllerPageFrame(void* page, u32 player, MenuInput* input, MenuSounds* sounds)
{
    PageOf(page)->Frame(player, input, sounds);
}

void ControllerPageLeft(void* page, u32 player, u32 mode)
{
    PageOf(page)->Left(player, mode);
}

void ControllerPageDestroy(void* page, u32 flags)
{
    PageOf(page)->Destroy(flags);
}

u32 ControllerPageBack(void* page, u32 player)
{
    (void)page;
    (void)player;
    return 0;
}

void ControllerPageUnused(void* page, u32 player)
{
    (void)page;
    (void)player;
}
}

// The port's page vtable: entries 0 and 7 are empty, as every retail vtable's are
const GccVTableEntry g_ControllerOptionsPageVTable[] = {
    {0, 0, nullptr},
    {0, 0, reinterpret_cast<void*>(&ControllerPageEntered)},
    {0, 0, reinterpret_cast<void*>(&ControllerPageFrame)},
    {0, 0, reinterpret_cast<void*>(&ControllerPageLeft)},
    {0, 0, reinterpret_cast<void*>(&ControllerPageDestroy)},
    {0, 0, reinterpret_cast<void*>(&ControllerPageBack)},
    {0, 0, reinterpret_cast<void*>(&ControllerPageUnused)},
    {0, 0, nullptr},
    {0, 0, reinterpret_cast<void*>(&ControllerPageUnused)},
};

// The page the options made, freed with them: the options page's own layout is retail's, so the page is beside it
ControllerOptionsPage* g_ControllerOptionsPage = nullptr;

// The controller page's own sizes for the options' list, which it puts on while it's shown: its rows are smaller and its list a
// window of fifteen (the retail pages keep the drawer's own sizes, a tenth taller and with every row drawn)
constexpr f32 CompactItemScaleX = 0.45f;
constexpr f32 CompactItemScaleY = 0.55f;
constexpr f32 CompactItemSpacing = Rounded(0.035);
constexpr u32 CompactWindow = 15;

// The drawer the options' pages are drawn with: one for them all, so the controller page's sizes are only its own
MenuDrawer* OptionsDrawer()
{
    if (g_OlegGameController == nullptr)
    {
        return nullptr;
    }

    return g_OlegGameController->oleg.optionsMenu.drawer;
}

ControllerOptionsPage* ControllerOptionsPage::Construct(ControllerOptionsPage* page, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, PortTextRemapControls, MenuPlayers);
    page->parent = parent;
    page->vtable = g_ControllerOptionsPageVTable;
    page->capturing = NoCapturing;
    page->compact = 0;
    page->keptScaleX = 0.0f;
    page->keptScaleY = 0.0f;
    page->keptSpacing = 0.0f;
    page->keptWindow = 0;
    for (u32 action = 0; action < MaxRows; action++)
    {
        page->labels[action][0] = '\0';
    }

    // A row per action, its label the action's and its bindings'; the item's ID is the action's index
    const u32 actions = Platform::Keybinds::Actions();
    for (u32 action = 0; action < actions && action < MaxRows; action++)
    {
        ActionItem* item = ActionItem::Construct(Allocate<ActionItem>(), NoText, action, ActionItem::ActionBind);
        item->text = page->labels[action];
        page->Add(item);
    }

    BackItem* back = BackItem::Construct(Allocate<BackItem>(), BackText, BackRowItem, parent, MenuPlayers);
    page->Add(back);
    page->SetFirstItem(0);
    page->RefreshLabels();
    return page;
}

void ControllerOptionsPage::Destroy(u32 flags)
{
    // The drawer's sizes back, if the page goes while it has them on
    Left(0, 0);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void ControllerOptionsPage::Entered(u32, u32)
{
    Platform::Keybinds::ClearTaken();
    SetWaiting(NoCapturing);

    // The list is the page's own size while it's shown: the options' pages share the drawer, so its sizes are put back when the
    // page is left
    MenuDrawer* drawer = OptionsDrawer();
    if (drawer != nullptr && compact == 0)
    {
        keptScaleX = drawer->manyItemsScale.x;
        keptScaleY = drawer->manyItemsScale.y;
        keptSpacing = drawer->manyItemsSpacing;
        keptWindow = drawer->windowSize;
        drawer->manyItemsScale = {CompactItemScaleX, CompactItemScaleY};
        drawer->manyItemsSpacing = CompactItemSpacing;
        drawer->windowSize = CompactWindow;
        compact = 1;
    }
}

void ControllerOptionsPage::Left(u32, u32)
{
    MenuDrawer* drawer = OptionsDrawer();
    if (drawer != nullptr && compact != 0)
    {
        drawer->manyItemsScale = {keptScaleX, keptScaleY};
        drawer->manyItemsSpacing = keptSpacing;
        drawer->windowSize = keptWindow;
        compact = 0;
    }
}

void ControllerOptionsPage::Frame(u32, MenuInput* input, MenuSounds*)
{
    if (capturing == NoCapturing)
    {
        return;
    }

    // The menus' own back ends the wait (the page holds it off while waiting, so this is the page's to take)
    if (input != nullptr && input->Has(MenuInput::ActionBack, ButtonBindings::OnPress) != 0)
    {
        Platform::Keybinds::ClearTaken();
        SetWaiting(NoCapturing);
        return;
    }

    Platform::Keybinds::Kind kind = Platform::Keybinds::Kind::Key;
    s32 code = 0;
    if (!Platform::Keybinds::Taken(&kind, &code))
    {
        return;
    }

    // The page keeps the two inputs that end a wait for itself
    if (Platform::Keybinds::IsCancel(kind, code))
    {
        Platform::Keybinds::ClearTaken();
        SetWaiting(NoCapturing);
        return;
    }

    Platform::Keybinds::SetBinding(static_cast<Platform::Keybinds::Action>(capturing), kind, code);
    Platform::Keybinds::Save();
    Platform::Keybinds::ClearTaken();
    SetWaiting(NoCapturing);
}

void ControllerOptionsPage::BeginCapture(u32 action)
{
    Platform::Keybinds::ClearTaken();
    SetWaiting(action < Platform::Keybinds::Actions() ? action : NoCapturing);
}

void ControllerOptionsPage::SetWaiting(u32 action)
{
    capturing = action;
    // The wait is the page's: the menus' select would take the row's own select press again (and clear the input being bound), and
    // their back would leave the page on the press being bound. Both come back when the wait ends
    const u32 waiting = capturing != NoCapturing ? 0 : 1;
    flags.takesSelect = waiting;
    flags.takesBack = waiting;
    flags.takesUpDown = waiting;
    flags.takesLeftRight = waiting;
    RefreshLabels();
}

void ControllerOptionsPage::RefreshLabels()
{
    const u32 actions = Platform::Keybinds::Actions();
    for (u32 action = 0; action < actions && action < MaxRows; action++)
    {
        const Platform::Keybinds::Action typed = static_cast<Platform::Keybinds::Action>(action);
        if (capturing == action)
        {
            std::snprintf(labels[action], LabelLength, "%s: %s", Platform::Keybinds::ActionName(typed),
                          GameText(PortTextPressInput));
            continue;
        }

        // The two names are the platform's own, in buffers of its own: copied out before the label is made. SDL names a key with
        // its shifted character ("E", "Left Shift", "F1"), which the menus say in lower case: the file keeps SDL's spelling, the
        // page says "e", "left shift", "f1"
        char key[32];
        char pad[32];
        std::snprintf(key, sizeof(key), "%s",
                      Platform::Keybinds::InputName(Platform::Keybinds::Kind::Key,
                                                    Platform::Keybinds::Binding(typed, Platform::Keybinds::Kind::Key)));
        std::snprintf(pad, sizeof(pad), "%s",
                      Platform::Keybinds::InputName(Platform::Keybinds::Kind::Pad,
                                                    Platform::Keybinds::Binding(typed, Platform::Keybinds::Kind::Pad)));
        for (char* letter = key; *letter != '\0'; letter++)
        {
            if (*letter >= 'A' && *letter <= 'Z')
            {
                *letter = static_cast<char>(*letter - 'A' + 'a');
            }
        }

        std::snprintf(labels[action], LabelLength, "%s: %s / %s", Platform::Keybinds::ActionName(typed), key, pad);
    }
}

OptionsPage* OptionsPage::Construct(OptionsPage* page, MenuPage* parent)
{
    MenuPage::ConstructTitled(page, OptionsText, MenuPlayers);
    page->parent = parent;
    page->vtable = g_OptionsPageVTable;

    GameOptionsPage& game = page->game;
    MenuPage::ConstructTitled(&game, GameOptionsText, MenuPlayers);
    game.parent = page;
    game.vtable = g_GameOptionsPageVTable;
    ToggleItem* vibration = ToggleItem::Construct(Allocate<ToggleItem>(), VibrationText, VibrationItem, NoText, YesText, 1, 0, MenuPlayers);
    BackItem* gameBack = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, page, MenuPlayers);
    game.Add(vibration);

    // The sticks' dead zone, on a host whose pads keep one: the row steps through off and five to twenty percent
    g_DeadZoneItem = nullptr;
    if (Platform::Pads::DeadZoneAvailable())
    {
        ChoiceItem* deadZone =
            ConstructNamedChoice(Allocate<ChoiceItem>(), PortTextDeadZone, DeadZoneItemId, DeadZoneNames, DeadZoneChoices,
                                 static_cast<s32>(Platform::Pads::DeadZonePercent() / DeadZoneSteps));
        game.Add(deadZone);
        g_DeadZoneItem = deadZone;
    }

    // The languages, on a host that offers a choice between them (the console's own language is its system settings')
    g_VoicesItem = nullptr;
    g_TextsItem = nullptr;
    if (Platform::System::LanguageChoices() != 0)
    {
        const u32 choices = Platform::System::LanguageChoices();
        const char* const* names = Platform::System::LanguageChoiceNames();
        ChoiceItem* voices = ConstructNamedChoice(Allocate<ChoiceItem>(), PortTextVoices, VoicesItemId, names, choices,
                                                  static_cast<s32>(Platform::System::LanguageChoice()));
        ChoiceItem* texts = ConstructNamedChoice(Allocate<ChoiceItem>(), PortTextTexts, TextsItemId, names, choices,
                                                 static_cast<s32>(Platform::System::TextLanguageChoice()));
        game.Add(voices);
        game.Add(texts);
        g_VoicesItem = voices;
        g_TextsItem = texts;

    }

    // Whether cutscenes may be skipped: off is retail's (a cutscene file only once played), on skips the files on their first
    // viewing and lets the levels' scripted scenes take their own skip
    ChoiceItem* cutsceneSkip =
        ConstructNamedChoice(Allocate<ChoiceItem>(), PortTextCutsceneSkip, CutsceneSkipItemId, CutsceneSkipNames,
                             CutsceneSkipChoices, Platform::System::CutsceneSkip() ? 1 : 0);
    game.Add(cutsceneSkip);
    g_CutsceneSkipItem = cutsceneSkip;

    game.Add(gameBack);

    GraphicsOptionsPage& graphics = page->graphics;
    MenuPage::ConstructTitled(&graphics, GraphicOptionsText, MenuPlayers);
    graphics.parent = page;
    graphics.vtable = g_GraphicsOptionsPageVTable;
    ActionItem* centre = ActionItem::Construct(Allocate<ActionItem>(), CentreScreenText, CentreScreenItem,
                                               ActionItem::ActionScreenPosition);
    ToggleItem* widescreen = ToggleItem::Construct(Allocate<ToggleItem>(), WidescreenText, WidescreenItem, OffText, OnText, 1, 0,
                                                   MenuPlayers);
    BackItem* graphicsBack = BackItem::Construct(Allocate<BackItem>(), BackText, SubPageBackItem, page, MenuPlayers);
    graphics.Add(centre);
    graphics.Add(widescreen);

    // The display's own options, when the host presents a picture of its own: the resolution, the window's mode, the rate, the
    // aspect, the render scale, the antialiasing and vsync
    for (u32 index = 0; index < DisplayRowCount; index++)
    {
        g_DisplayItems[index] = nullptr;
    }
    g_VsyncItem = nullptr;
    if (Platform::Display::Presentable())
    {
        for (u32 index = 0; index < DisplayRowCount; index++)
        {
            const DisplayRow& row = DisplayRows[index];
            const u32 choices = row.count();
            if (choices == 0)
            {
                continue;
            }

            ChoiceItem* item = ConstructNamedChoice(Allocate<ChoiceItem>(), row.text, row.item, row.names(), choices,
                                                      static_cast<s32>(row.value()));
            graphics.Add(item);
            g_DisplayItems[index] = item;
        }

        ToggleItem* vsync = ToggleItem::Construct(Allocate<ToggleItem>(), NoText, VsyncItemId, OffText, OnText, 1,
                                                  Platform::Display::Vsync() ? 1 : 0, MenuPlayers);
        vsync->text = "vsync";
        graphics.Add(vsync);
        g_VsyncItem = vsync;
    }

    graphics.Add(graphicsBack);

    SoundOptionsPage::Construct(&page->sound, page);
    LinkItem* graphicsLink = LinkItem::Construct(Allocate<LinkItem>(), GraphicOptionsText, GraphicOptionsItem, &page->graphics,
                                                 MenuPlayers);
    LinkItem* soundLink = LinkItem::Construct(Allocate<LinkItem>(), SoundOptionsText, SoundOptionsItem, &page->sound, MenuPlayers);
    LinkItem* gameLink = LinkItem::Construct(Allocate<LinkItem>(), GameOptionsText, GameOptionsItem, &page->game, MenuPlayers);
    BackItem* back = BackItem::Construct(Allocate<BackItem>(), BackText, ParentBackItem, parent, MenuPlayers);
    page->Add(graphicsLink);
    page->Add(soundLink);
    page->Add(gameLink);

    // The port's controller page, on a host whose inputs can be rebound: a row per input, written back to keybinds.txt
    g_ControllerOptionsPage = nullptr;
    if (Platform::Keybinds::Actions() != 0)
    {
        ControllerOptionsPage* controller = ControllerOptionsPage::Construct(Allocate<ControllerOptionsPage>(), page);
        g_ControllerOptionsPage = controller;
        LinkItem* controllerLink =
            LinkItem::Construct(Allocate<LinkItem>(), PortTextRemapControls, ControllerOptionsItem, controller, MenuPlayers);
        page->Add(controllerLink);
    }

    page->Add(back);
    return page;
}

void OptionsPage::Destroy(u32 flags)
{
    if (g_ControllerOptionsPage != nullptr)
    {
        // Its items and rows are the page's, freed with it
        g_ControllerOptionsPage->Destroy(DestroyAndFree);
        g_ControllerOptionsPage = nullptr;
    }

    sound.vtable = g_OlegPageVTable;
    sound.MenuPage::Destroy(DestroyOnly);
    graphics.vtable = g_OlegPageVTable;
    graphics.MenuPage::Destroy(DestroyOnly);
    game.vtable = g_OlegPageVTable;
    game.MenuPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

u32 OptionsPage::Back(u32)
{
    GameController* controller = g_OlegGameController;
    OLEG& oleg = controller->oleg;
    oleg.Hide(oleg.screens[OLEG::ScreenAllButOverlays], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    u32 screen =
        g_OlegGameController->State() == GameController::StateMainMenu ? OLEG::ScreenMainMenu : OLEG::ScreenPauseMenu;
    oleg.Show(oleg.screens[screen], static_cast<s32>(g_ClockUnitsPerSecond * ScreenChangeSeconds), 0);
    return 1;
}

void SaveGamePage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void SaveGamePage::Entered(u32, u32)
{
    g_OlegGameController->SaveFromPause();
}

PausePage* PausePage::Construct(PausePage* page, PadButtons*, Font*)
{
    OlegPage::ConstructTitled(page, PauseTitle, &g_ResumePage);
    page->vtable = g_PausePageVTable;
    SaveGamePage& saveGame = page->saveGame;
    OlegPage::ConstructTitled(&saveGame, SaveGameText, nullptr);
    saveGame.owner = page;
    saveGame.vtable = g_SaveGamePageVTable;
    LoadGamePage& loadGame = page->loadGame;
    OlegPage::ConstructTitled(&loadGame, LoadGameText, nullptr);
    loadGame.owner = page;
    loadGame.vtable = g_LoadGamePageVTable;
    QuitPage::Construct(&page->quit, page);
    ActionItem* options = ActionItem::Construct(Allocate<ActionItem>(), OptionsText, OptionsItem, ActionItem::ActionOptions);
    LinkItem* save = LinkItem::Construct(Allocate<LinkItem>(), SaveGameText, SaveGameItem, &page->saveGame, MenuPlayers);
    LinkItem* load = LinkItem::Construct(Allocate<LinkItem>(), LoadGameText, LoadGameItem, &page->loadGame, MenuPlayers);
    ActionItem* disableAutosave = ActionItem::Construct(Allocate<ActionItem>(), DisableAutosaveText, DisableAutosaveItem,
                                                        ActionItem::ActionDisableAutosave);
    // The pause menu's quitting closes the game (retail's row went back to the title): it asks first, and the question's yes ends
    // the process
    ActionItem* quit = ActionItem::Construct(Allocate<ActionItem>(), PortTextCloseGame, QuitGameItem, ActionItem::ActionAskExit);
    BackItem* resume = BackItem::Construct(Allocate<BackItem>(), ResumeText, ResumeItem, &g_ResumePage, MenuPlayers);
    page->Add(options);
    page->Add(save);
    page->Add(load);
    page->Add(disableAutosave);
    page->Add(quit);
    page->Add(resume);
    page->SetFirstItem(ResumeItem);
    return page;
}

void PausePage::Destroy(u32 flags)
{
    quit.OlegPage::Destroy(DestroyOnly);
    loadGame.OlegPage::Destroy(DestroyOnly);
    saveGame.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void PausePage::Frame(u32, MenuInput*, MenuSounds*)
{
    // The disable autosave item is there while an autosave waits or saves, not while its screens show
    MenuItem* disableAutosave = Find(DisableAutosaveItem);
    u32 saving = static_cast<u32>(g_OlegGameController->states.savingStep);
    bool autosaving = saving == GameController::SavingWait || saving == GameController::SavingSaved ||
                      saving == GameController::SavingSavedShown;
    bool shown = false;
    if (autosaving)
    {
        shown = g_OlegGameController->states.savingStep != GameController::SavingSavedShown;
    }

    disableAutosave->shown = shown ? EveryPlayer : 0;
}

void GalleryItem::Destroy(u32 flags)
{
    StringDestroy(&name);
    LinkItem::Destroy(flags);
}

MenuPage* GalleryItem::Activate(u32, MenuPage*)
{
    g_OlegGameController->RequestGallery(static_cast<s32>(g_ClockUnitsPerSecond * ItemDelaySeconds), name.string, first,
                                              last);
    return nullptr;
}

MovieItem* MovieItem::Construct(MovieItem* item, u32 text, u32 id, u32 movie)
{
    LinkItem::Construct(item, text, id, nullptr, MenuPlayers);
    item->movie = movie;
    item->vtable = g_MovieItemVTable;
    return item;
}

void MovieItem::Destroy(u32 flags)
{
    LinkItem::Destroy(flags);
}

MenuPage* MovieItem::Activate(u32, MenuPage*)
{
    g_OlegGameController->RequestMovie(static_cast<s32>(g_ClockUnitsPerSecond * ItemDelaySeconds), movie);
    return nullptr;
}

void GemExtrasPage::ShowFound()
{
    GameProgress& progress = g_OlegGameController->progress;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        MenuItem* found = Find(item);
        found->enabled = (progress.levels[item].gems & 1u << gem) != 0 ? EveryPlayer : 0;
    }
}

BossesPage* BossesPage::Construct(BossesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_BossesPageVTable);
    AddGalleries(page, BossesText, BossesName, 1);
    page->Add(back);
    return page;
}

void BossesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void BossesPage::Entered(u32, u32)
{
    ShowFound();
}

ConceptPage* ConceptPage::Construct(ConceptPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_ConceptPageVTable);
    AddGalleries(page, ConceptText, ConceptName, 2);
    page->Add(back);
    return page;
}

void ConceptPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void ConceptPage::Entered(u32, u32)
{
    ShowFound();
}

EnemiesPage* EnemiesPage::Construct(EnemiesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_EnemiesPageVTable);
    AddGalleries(page, EnemiesText, EnemiesName, 2);
    page->Add(back);
    return page;
}

void EnemiesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void EnemiesPage::Entered(u32, u32)
{
    ShowFound();
}

MoviesPage* MoviesPage::Construct(MoviesPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_MoviesPageVTable);
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MovieItem::Construct(Allocate<MovieItem>(), MoviesText + item, item, ExtrasMovies[item]));
    }

    page->Add(back);
    return page;
}

void MoviesPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void MoviesPage::Entered(u32, u32)
{
    GameProgress& progress = g_OlegGameController->progress;
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        MenuItem* movie = Find(item);
        bool found = (progress.levels[item].gems & 1u << gem) != 0;
        if (item >= FirstStoryEndMovie)
        {
            found = found && progress.play.story >= StoryDoneArea;
        }

        movie->enabled = found ? EveryPlayer : 0;
    }
}

StoryboardsPage* StoryboardsPage::Construct(StoryboardsPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_StoryboardsPageVTable);
    for (u32 item = 0; item < GemExtrasItems; item++)
    {
        page->Add(MakeGalleryItem(StoryboardsText + item, item, Storyboards[item].name, 1, Storyboards[item].pictures));
    }

    page->Add(back);
    return page;
}

void StoryboardsPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void StoryboardsPage::Entered(u32, u32)
{
    ShowFound();
}

UnseenPage* UnseenPage::Construct(UnseenPage* page, u32 gem, MenuPage* parent)
{
    BackItem* back = StartGemExtras(page, gem, parent, g_UnseenPageVTable);
    AddGalleries(page, UnseenText, UnseenName, 3);
    page->Add(back);
    return page;
}

void UnseenPage::Destroy(u32 flags)
{
    OlegPage::Destroy(flags);
}

void UnseenPage::Entered(u32, u32)
{
    ShowFound();
}

ExtrasPage* ExtrasPage::Construct(ExtrasPage* page)
{
    OlegPage::ConstructTitled(page, ExtrasTitle, &g_ResumePage);
    page->vtable = g_ExtrasPageVTable;
    BossesPage::Construct(&page->bosses, GemBlue, page);
    ConceptPage::Construct(&page->conceptArt, GemPurple, page);
    EnemiesPage::Construct(&page->enemies, GemGreen, page);
    MoviesPage::Construct(&page->movies, GemClear, page);
    StoryboardsPage::Construct(&page->storyboards, GemRed, page);
    UnseenPage::Construct(&page->unseen, GemYellow, page);
    MovieItem* complete = MovieItem::Construct(Allocate<MovieItem>(), CompleteText, CompleteItem, GameMovie::Complete);
    BackItem* resume = BackItem::Construct(Allocate<BackItem>(), ResumeText, ExtrasResumeItem, &g_ResumePage, MenuPlayers);
    // Every link made before any is added (adding can grow the page's list)
    OlegPage* const gemPages[GameProgress::Gems] = {&page->bosses,     &page->movies,      &page->enemies,
                                                    &page->conceptArt, &page->storyboards, &page->unseen};
    LinkItem* links[GameProgress::Gems];
    for (u32 gem = 0; gem < GameProgress::Gems; gem++)
    {
        links[gem] = LinkItem::Construct(Allocate<LinkItem>(), GemExtrasTitle + gem, GemExtrasLink + gem, gemPages[gem], MenuPlayers);
    }

    for (LinkItem* link : links)
    {
        page->Add(link);
    }

    page->Add(complete);
    page->Add(resume);
    return page;
}

void ExtrasPage::Destroy(u32 flags)
{
    unseen.OlegPage::Destroy(DestroyOnly);
    storyboards.OlegPage::Destroy(DestroyOnly);
    movies.OlegPage::Destroy(DestroyOnly);
    enemies.OlegPage::Destroy(DestroyOnly);
    conceptArt.OlegPage::Destroy(DestroyOnly);
    bosses.OlegPage::Destroy(DestroyOnly);
    vtable = g_OlegPageVTable;
    MenuPage::Destroy(flags);
}

void ExtrasPage::Entered(u32, u32)
{
    GameProgress& progress = g_OlegGameController->progress;
    u32 done = progress.Done();
    MenuItem* complete = Find(CompleteItem);
    for (u32 gem = 0; gem < GameProgress::Gems; gem++)
    {
        MenuItem* link = Find(GemExtrasLink + gem);
        link->shown = progress.GemsFound(gem) != 0 ? EveryPlayer : 0;
    }

    if (done == 100)
    {
        complete->bits.text = CompleteText;
        complete->shown = EveryPlayer;
    }
    else
    {
        complete->bits.text = NotCompleteText;
        complete->shown = 0;
    }

    complete->text = nullptr;
}

LevelsPage* LevelsPage::Construct(LevelsPage* page, u32 world, MenuWidget* menu)
{
    OlegPage::Construct(page, "", &g_ResumePage);
    page->vtable = g_LevelsPageVTable;
    Widget* after = menu->next;
    const LevelsLayout& layout = world < Worlds ? LevelsLayouts[world] : NoWorld;
    MenuItem* items[Slots] = {};
    if (world < Worlds)
    {
        for (u32 level = 0; level < LevelsPerWorld; level++)
        {
            items[level] = LinkItem::ConstructNamed(Allocate<LinkItem>(), "", layout.levels[level], nullptr, MenuPlayers);
        }
    }

    Widget* tail = menu;
    for (u32 slot = 0; slot < Slots; slot++)
    {
        MenuItem* item = items[slot];
        if (item == nullptr)
        {
            page->rings[slot] = nullptr;
            page->levels[slot] = nullptr;
            continue;
        }

        Vector2 place = {(slot & 1) == 0 ? layout.evenX : layout.oddX, layout.top + layout.spacing * static_cast<f32>(slot)};
        Vector2 scale = {1.0f, 1.0f};
        Vector2 levelScale = LevelWidgetScale;
        Vector2 offset = {place.x < 0.5f ? -1.0f : 1.0f, 0.0f};
        RingWidget* ring =
            RingWidget::Construct(Allocate<RingWidget>(), AnchorMiddle, PanelRings, RingSegments, RingSteps, nullptr);
        page->rings[slot] = ring;
        LevelWidget* level = LevelWidget::Construct(Allocate<LevelWidget>(), AnchorMiddle, page, item, g_OlegGameController);
        page->levels[slot] = level;
        u32 colour;
        GetColor(&colour, ColourWhite);
        ring->SlideIn(colour, &place, &scale, &offset);
        GetColor(&colour, ColourWhite);
        level->SlideIn(colour, &place, &levelScale, &offset);
        Vector2 radii = LevelPanelRadii;
        AddPanelRings(&radii, ring);
        tail->next = ring;
        ring->next = level;
        tail = level;
        page->Add(item);
    }

    tail->next = after;
    return page;
}

void LevelsPage::Destroy(u32 flags)
{
    vtable = g_LevelsPageVTable;
    for (u32 slot = 0; slot < Slots; slot++)
    {
        if (levels[slot] != nullptr)
        {
            CallVirtual<void>(levels[slot], levels[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }

        if (rings[slot] != nullptr)
        {
            CallVirtual<void>(rings[slot], rings[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }
    }

    OlegPage::Destroy(flags);
}

MenuPage* SaveSlotsPageConstruct(void* memory, SaveManager* manager, MenuWidget* menu)
{
    auto* page = static_cast<SaveSlotsPage*>(memory);
    SaveCodePage::Construct(page, manager);
    GameController* controller = g_OlegGameController;
    page->vtable = g_SaveSlotsPageVTable;
    Widget* after = menu->next;
    // A slot for each of the save's files besides the icons
    page->count = manager->device->flags.fileCount;
    page->widgets = static_cast<SaveSlotWidget**>(MemoryAllocate2(page->count * sizeof(SaveSlotWidget*)));
    page->rings = static_cast<RingWidget**>(MemoryAllocate2(page->count * sizeof(RingWidget*)));
    Widget* tail = menu;
    for (u32 slot = 0; slot < page->count; slot++)
    {
        SaveSummary* summary = &manager->banks[slot]->summary;
        MenuItem* item = ConstructSaveCodeItem(MemoryAllocate(sizeof(SaveCodeItem)), 0, SlotItemIds + slot,
                                               SaveCodeItem::AnswerChoose, manager);
        bool even = (slot & 1) == 0;
        Vector2 place = {even ? SlotEvenX : SlotOddX, static_cast<f32>(slot) * SlotSpacing + SlotsTop};
        Vector2 scale = {1.0f, 1.0f};
        Vector2 offset = {even ? -1.0f : 1.0f, 0.0f};
        RingWidget* ring =
            RingWidget::Construct(Allocate<RingWidget>(), AnchorMiddle, PanelRings, RingSegments, RingSteps, nullptr);
        page->rings[slot] = ring;
        SaveSlotWidget* widget =
            SaveSlotWidget::Construct(Allocate<SaveSlotWidget>(), AnchorMiddle, page, item, controller, summary);
        page->widgets[slot] = widget;
        u32 colour;
        GetColor(&colour, ColourWhite);
        ring->SlideIn(colour, &place, &scale, &offset);
        GetColor(&colour, ColourWhite);
        widget->SlideIn(colour, &place, &scale, &offset);
        Vector2 radii = SlotPanelRadii;
        AddPanelRings(&radii, ring);
        tail->next = ring;
        ring->next = widget;
        tail = widget;
        page->Add(item);
    }

    tail->next = after;
    page->AddItems();
    return page;
}

void SaveSlotsPage::Destroy(u32 destroyFlags)
{
    vtable = g_SaveSlotsPageVTable;
    for (u32 slot = 0; slot < count; slot++)
    {
        if (widgets[slot] != nullptr)
        {
            CallVirtual<void>(widgets[slot], widgets[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }

        if (rings[slot] != nullptr)
        {
            CallVirtual<void>(rings[slot], rings[slot]->vtable, Widget::DestroySlot, u32{DestroyAndFree});
        }
    }

    if (widgets != nullptr)
    {
        MemoryDeallocate_(widgets);
    }

    if (rings != nullptr)
    {
        MemoryDeallocate_(rings);
    }

    // Past the save code page's destructor, straight to the menu page's
    MenuPage::Destroy(destroyFlags);
}

void InitOlegPagesModule(u32 initialize, u32 priority)
{
    if (priority != DefaultInitPriority || initialize == 0)
    {
        return;
    }

    g_OlegPagesUnusedUp.x = 0.0f;
    g_OlegPagesUnusedUp.w = 1.0f;
    g_OlegPagesUnusedShadowY = UiShadowOffset;
    g_OlegPagesUnusedUp.y = 1.0f;
    g_OlegPagesUnusedUp.z = 0.0f;
    g_OlegPagesUnusedShadowX = UiShadowOffset;
    ColourSet(&g_OlegPagesUnusedShadowColour, 0.0f, 0.0f, 0.0f, UiShadowAlpha);
    AngleFrom(&g_OlegPagesUnusedAngle, QuarterPi, AngleRadians);
    g_OlegPagesUnusedUpBack.x = 0.0f;
    g_OlegPagesUnusedUp2.w = 1.0f;
    g_OlegPagesUnusedUpBack.z = -1.0f;
    g_OlegPagesUnusedUpBack.w = 1.0f;
    g_OlegPagesUnusedUp2.x = 0.0f;
    g_OlegPagesUnusedUp2.z = 0.0f;
    g_OlegPagesUnusedUpBack.y = 1.0f;
    g_OlegPagesUnusedUp2.y = 1.0f;
    g_OlegPagesUnusedZero = 0;
    GetColor(&g_OlegPagesUnusedWhite, ColourWhite);
    GetColor(&g_OlegPagesUnusedBlack, ColourBlack);
}

void ConstructOlegPagesModule()
{
    InitOlegPagesModule(1, DefaultInitPriority);
}
