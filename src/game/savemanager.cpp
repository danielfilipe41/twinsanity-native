#include "game/porttext.h"
#include "game/savemanager.h"

#include "game/clock.h"
#include "game/gamecontroller.h"
#include "game/memory.h"
#include "game/olegpages.h"
#include "game/pads.h"
#include "game/progress.h"
#include "game/renderer.h"
#include "game/savedevice.h"
#include "game/sound.h"
#include "game/stream.h"
#include "game/string.h"
#include "game/widgets.h"
#include "retail/libc.h"

#include <cstdio>

// The save manager, the save code on OLEG's screens it derives from and its bank files
extern "C"
{
    extern const GccVTableEntry g_SaveManagerVTable[] RETAIL(D_002F5120);
    extern const GccVTableEntry g_SaveCodeVTable[] RETAIL(D_00306570);
    extern const GccVTableEntry g_SaveCodeScreensVTable[] RETAIL(D_00306470);
    extern const GccVTableEntry g_BankFileVTable[] RETAIL(D_002F5200);
    extern const GccVTableEntry g_BankSummaryVTable[] RETAIL(BanksHeader_methods);
    // "crash twinsanity", "Crash~Twinsanity", "Crash.ico", "BESLES-52568", "StartUp\Crash.ico", the save's name "", "Bank" and
    // ".bin"; the buffer the save's files are read into
    extern const char g_SaveCodeName[] RETAIL(D_002F4800);
    extern const char g_SaveTitle[] RETAIL(D_002F4818);
    extern const char g_IconName[] RETAIL(D_002F4830);
    extern const char g_ProductCode[] RETAIL(D_002F4840);
    extern const char g_IconPath[] RETAIL(D_002F4850);
    extern const char g_SaveName[] RETAIL(D_00309958);
    extern const char g_BankPrefix[] RETAIL(D_00309988);
    extern const char g_BankExtension[] RETAIL(D_00309990);
    extern u8 g_SaveBuffer[] RETAIL(D_0030BF28);
}

namespace
{
constexpr u32 Banks = 4;
constexpr u32 BankSize = 0xF400;
// The icon files' room, and the folder's file's size
constexpr u32 IconFilesRoom = 3;
constexpr u32 FolderFileSize = 0x800;
// The tag the folder's and the banks' files end with (with their checksums)
constexpr u32 SaveTag = 1;

// The manager's OLEG screens (its screens' indexes): every widget hidden (before another is shown), the message's, the choices'
// and the save slots'
enum ManagerScreen : u32
{
    EveryWidgetScreen = 0,
    MessageScreen = 1,
    ChoicesScreen = 2,
    SlotsScreen = 3,
};

// Its labels and their texts (messages' and strings' indexes): the message, the choices' title and the save slots' title
enum ManagerLabel : u32
{
    MessageLabel = 0,
    ChoicesLabel = 1,
    SlotsLabel = 2,
};

// The screens appear and disappear over half a second
s32 HalfSecond()
{
    return static_cast<s32>(g_ClockUnitsPerSecond * 0.5f);
}

void SwitchScreen(SaveManager* manager, u32 screen)
{
    manager->oleg->Hide(manager->oleg->screens[manager->screens[EveryWidgetScreen]], HalfSecond(), 0);
    manager->oleg->Show(manager->oleg->screens[manager->screens[screen]], HalfSecond(), 0);
}

// The choices page's mode of a screen of choices
s32 ChoicesMode(u32 screen)
{
    if (screen >= SaveScreenNoCard && screen <= SaveScreenCancelSave)
    {
        return static_cast<s32>(screen - SaveScreenNoCard) + ChoicesNoCard;
    }

    if (screen >= SaveScreenFormatFailed && screen <= SaveScreenLoadFailed)
    {
        return static_cast<s32>(screen - SaveScreenFormatFailed) + ChoicesFormatFailed;
    }

    return ChoicesHidden;
}
}

extern "C"
{
    void ConstructSaveCode(SaveCode* code, const char* name, OLEG* oleg, void* device)
    {
        SaveCode::Construct(code, name, static_cast<SaveDevice*>(device));
        auto* manager = static_cast<SaveManager*>(code);
        manager->oleg = oleg;
        manager->vtable = g_SaveCodeScreensVTable;
        for (String& string : manager->strings)
        {
            string = {nullptr, 0, 0};
        }

        for (StringLabel*& message : manager->messages)
        {
            message = nullptr;
        }

        manager->choicesPage = nullptr;
        manager->slotsPage = nullptr;
        for (s32& screen : manager->screens)
        {
            screen = 0;
        }

        manager->screens[EveryWidgetScreen] = -1;
    }
}

namespace
{
// The port's own wording for a save screen, where a computer's saves say something the console's messages do not: the folder
// creation that fails, the slots being read, the question the pause menu's new game asks, and the heading over the new game's
// "create a save?" message (the label draws a line break as two lines)
void PortScreenMessage(u32 operation, u32 screen, String* text)
{
    switch (screen)
    {
    case SaveScreenFormatFailed:
        StringAssign(text, GameText(PortTextCreateFolderFailed));
        return;
    case SaveScreenSaveSlots:
    case SaveScreenLoadSlots:
        StringAssign(text, GameText(PortTextLoadingSaves));
        return;
    case SaveScreenCancelSave:
        // The pause menu's new game asks its own question; retail's own cancel-save wording stays for retail's flows
        if (operation == SaveOperationPauseSave)
        {
            StringAssign(text, GameText(PortTextAreYouSure));
        }

        return;
    case SaveScreenCreate:
    {
        String heading;
        StringConstruct(&heading, GameText(PortTextNoSaveCreated));
        StringAppend(&heading, "\n");
        StringAppend(&heading, GameText(g_ScreenMessages[screen]));
        StringAssign(text, heading.string);
        StringDestroy(&heading);
        return;
    }
    default:
        return;
    }
}
}

u32 SaveManager::Ask(u32 operation, u32 file)
{
    bool message = true;
    bits.screen = SaveScreenMessage;
    switch (bits.operation)
    {
    case SaveOperationNone:
    case SaveOperationCheckInserted:
        message = false;
        break;
    // The others but the autosave bring the message's screen up
    case SaveOperationCheckRoom:
    case SaveOperationNewGameSave:
    case SaveOperationPauseSave:
    case SaveOperationLoad:
        SwitchScreen(this, MessageScreen);
        break;
    default:
        break;
    }

    if (message)
    {
        MessageText(g_OperationMessages[operation], &strings[MessageLabel]);
        // The checks are the port's own: a computer's saves are files in a folder, and there is no card to look for
        if (bits.operation == SaveOperationCheckRoom || bits.operation == SaveOperationCheckInserted)
        {
            StringAssign(&strings[MessageLabel], GameText(PortTextCheckingSaves));
        }

        StringAssign(&messages[MessageLabel]->text, strings[MessageLabel].string);
    }

    return device->Ask(operation, file);
}

// Whether the screens of the operation running offer "Continue without saving". Retail offers it for a new game's save alone, the
// pause menu's save making the player pick a slot or leave the whole flow; the port offers it for any save the player asked for,
// since a player whose card is full, missing or broken keeps playing either way, and the pause menu had no other way out of a save
// that cannot happen
bool SaveManager::OffersToSkipSaving() const
{
    return bits.operation == SaveOperationNewGameSave || bits.operation == SaveOperationPauseSave;
}

u32 SaveManager::ShowChoices(u32 screen)
{
    bits.screen = screen;
    if (choicesPage != nullptr)
    {
        s32 mode = ChoicesMode(screen);
        MessageText(g_ScreenMessages[screen], &strings[ChoicesLabel]);
        PortScreenMessage(bits.operation, screen, &strings[ChoicesLabel]);
        StringAssign(&messages[ChoicesLabel]->text, strings[ChoicesLabel].string);
        static_cast<SaveChoicesPage*>(choicesPage)->ShowItems(mode, OffersToSkipSaving());
    }

    SwitchScreen(this, ChoicesScreen);
    return screen;
}

u32 SaveManager::ShowSlots(u32 screen)
{
    bits.screen = screen;
    if (slotsPage != nullptr)
    {
        s32 mode = SlotsPageHidden;
        if (screen == SaveScreenSaveSlots)
        {
            mode = SlotsPageSave;
        }
        else if (screen == SaveScreenLoadSlots)
        {
            mode = SlotsPageLoad;
        }

        MessageText(g_ScreenMessages[screen], &strings[SlotsLabel]);
        PortScreenMessage(bits.operation, screen, &strings[SlotsLabel]);

        // Which card the slots are on, when there's more than one: the screens' pages are the cards
        if (Platform::Saves::CardCount() > 1)
        {
            char card[32];
            std::snprintf(card, sizeof(card), "  (%s %u/%u)", GameText(PortTextCard), Platform::Saves::Card() + 1,
                          Platform::Saves::CardCount());
            StringAppend(&strings[SlotsLabel], card);
        }

        StringAssign(&messages[SlotsLabel]->text, strings[SlotsLabel].string);
        static_cast<SaveCodePage*>(slotsPage)->ShowSlotItems(mode, OffersToSkipSaving());
    }

    SwitchScreen(this, SlotsScreen);
    return screen;
}

void SaveManager::DestroyScreens(u32 destroyFlags)
{
    StringDestroy(&strings[SlotsLabel]);
    StringDestroy(&strings[ChoicesLabel]);
    StringDestroy(&strings[MessageLabel]);
    vtable = g_SaveCodeVTable;
    StringDestroy(&name);
    if ((destroyFlags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(this);
    }
}

void SaveManager::Draw(Renderer*)
{
    // The save screens' pages are the cards: L1 and L2 the card before, R1 and R2 the one after, while a card's screens are up and
    // the card is idle (the "no save on this card" question and the "insert a save" one too, so a player is never kept from another
    // card's saves). The menus' own input is the pads', and a press is the frame a button goes down (the same edge their pages step on)
    if (!ShowsCards())
    {
        return;
    }

    if (device->flags.running != SaveDevice::OperationNone || Platform::Saves::CardCount() < 2)
    {
        return;
    }

    GamePad* pad = g_MainGamePad;
    if (pad == nullptr)
    {
        return;
    }

    const u32 pressed = pad->now & ~pad->previous;
    if ((pressed & ((1u << PadL1) | (1u << PadL2))) != 0)
    {
        StepCard(-1);
    }
    else if ((pressed & ((1u << PadR1) | (1u << PadR2))) != 0)
    {
        StepCard(1);
    }
}

bool SaveManager::ShowsCards() const
{
    switch (bits.screen)
    {
    case SaveScreenSaveSlots:
    case SaveScreenLoadSlots:
    case SaveScreenCreate:
    case SaveScreenInsertSave:
        return true;
    default:
        return false;
    }
}

bool SaveManager::StepCard(s32 pages)
{
    const s32 count = static_cast<s32>(Platform::Saves::CardCount());
    if (count < 2 || !ShowsCards())
    {
        return false;
    }

    if (device->flags.running != SaveDevice::OperationNone)
    {
        return false;
    }

    // The card the pages land on, round the ring of them, kept for the next run
    s32 card = static_cast<s32>(Platform::Saves::Card()) + pages;
    card = (card % count + count) % count;
    Platform::Saves::SetCard(static_cast<u32>(card));

    // The slots the screens showed were the last card's, so they are cleared before the new card's are read: a card that can't be
    // read shows empty slots rather than another card's saves (and a save is never written over a slot that looked like another
    // card's)
    folder.ClearSummaries();

    // The device's next operations go to it, and the card is checked again: a card with a save is read and its slots shown, one
    // without gets its save's folder made (saving) or is refused (loading), as retail's own flow does with a card
    s32 port = 0;
    s32 slot = 0;
    Platform::Saves::CardPortSlot(static_cast<u32>(card), &port, &slot);
    device->SetCard(port, slot);
    Ask(SaveDevice::OperationMeasure, 0);
    return true;
}

SaveManager* SaveManager::Construct(SaveManager* manager, GameController* controller)
{
    ConstructSaveCode(manager, g_SaveCodeName, &controller->oleg, &manager->memoryCard);
    manager->vtable = g_SaveManagerVTable;
    ConstructIconSys(&manager->iconSys, g_SaveTitle, g_IconName);
    ConstructIconFiles(&manager->iconFiles, IconFilesRoom, &manager->iconSys);
    ConstructFolderFile(&manager->folder, g_ProductCode, Banks, SaveTag, FolderFileSize, g_SaveBuffer);
    SaveDevice::Construct(&manager->memoryCard, Banks, &manager->iconFiles, &manager->folder, g_SaveName);
    AddIconFile(&manager->iconFiles, ConstructCopiedFile(MemoryAllocate(sizeof(CopiedFile)), g_IconPath, g_IconName));
    manager->banks = static_cast<BankFile**>(MemoryAllocate2(Banks * sizeof(BankFile*)));
    for (u32 index = 0; index < Banks; index++)
    {
        String name;
        StringConstruct(&name, g_BankPrefix);
        String number;
        StringConstructNumber(&number, index);
        StringAppend(&name, number.string);
        StringDestroy(&number);
        StringAppend(&name, g_BankExtension);
        auto* bank = static_cast<BankFile*>(MemoryAllocate(sizeof(BankFile)));
        ConstructSaveFile(bank, name.string, SaveTag, BankSize, g_SaveBuffer);
        bank->vtable = g_BankFileVTable;
        ConstructBankSummary(&bank->summary);
        bank->controller = controller;
        manager->banks[index] = bank;
        manager->folder.summaries[index] = &bank->summary.folder;
        manager->memoryCard.files[index] = bank;
        StringDestroy(&name);
    }

    return manager;
}

void SaveManager::Destroy(u32 destroyFlags)
{
    vtable = g_SaveManagerVTable;
    for (u32 index = 0; index < Banks; index++)
    {
        BankFile* bank = banks[index];
        if (bank != nullptr)
        {
            CallVirtual<void>(bank, bank->vtable, SaveFile::DestroySlot, u32{DestroyAndFree});
        }
    }

    MemoryDeallocate2_(banks);
    memoryCard.Destroy(DestroyOnly);
    DestroyFolderFile(&folder, DestroyOnly);
    DestroyIconFiles(&iconFiles, DestroyOnly);
    // icon.sys's destructor inline
    StringDestroy(&iconSys.title);
    DestroySaveFile(&iconSys, DestroyOnly);
    StringDestroy(&strings[SlotsLabel]);
    StringDestroy(&strings[ChoicesLabel]);
    StringDestroy(&strings[MessageLabel]);
    DestroySaveCode(this, destroyFlags);
}

void BankFile::GatherSettings()
{
    SaveController* settings = &controller->saveController;
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    GameRendererController* renderer = G_GameRendererController;
    settings->options.vibration = pads->flags.vibration;
    settings->effectsVolume = GroupVolumeLevel(EffectsGroup);
    settings->musicVolume = GroupVolumeLevel(MusicGroup);
    settings->options.musicStereo = g_MusicStereo;
    settings->options.widescreen = g_WidescreenTv;
    settings->screenOffset.x = renderer->screenOffset.x;
    settings->screenOffset.y = renderer->screenOffset.y;
    MakeBankSummary(&summary, settings);
}

void BankFile::Destroy(u32 destroyFlags)
{
    summary.folder.Destroy(DestroyOnly);
    DestroySaveFile(this, destroyFlags);
}

void BankFile::ReadData(Stream* stream)
{
    controller->saveController.Read(stream);
}

void BankFile::WriteData(Stream* stream)
{
    controller->saveController.Write(stream);
}

void SaveManager::Nothing10()
{
}

void ConstructBankSummary(void* memory)
{
    auto* summary = static_cast<SaveSummary*>(memory);
    FolderSummary* folder = &summary->folder;
    FolderSummary::Construct(folder);
    folder->vtable = g_BankSummaryVTable;
    RetailLibc::MemorySet(&summary->progress, 0, sizeof(summary->progress));
    summary->time = 0;
}

void MakeBankSummary(void* memory, SaveController* settings)
{
    auto* summary = static_cast<SaveSummary*>(memory);
    SaveControllerSummary bits = settings->summary;
    SavedProgress progress = summary->progress;
    progress.area = settings->options.area;
    progress.character = bits.character;
    progress.lives = bits.lives;
    progress.crystals = bits.crystals;
    progress.done = bits.done;
    summary->progress = progress;
    summary->time = settings->timePlayed;
    summary->folder.Refresh();
}

void SaveSummary::Destroy(u32 destroyFlags)
{
    folder.Destroy(destroyFlags);
}

void SaveSummary::Read(Stream* stream)
{
    stream->Read(&progress, sizeof(progress), 1);
    stream->ReadU32(reinterpret_cast<u32*>(&time));
    folder.Read(stream);
}

void SaveSummary::Write(Stream* stream)
{
    stream->Write(&progress, sizeof(progress));
    stream->WriteU32(time);
    folder.Write(stream);
}
