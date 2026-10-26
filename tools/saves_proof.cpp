// The offline saves proof: drives the host's Platform::Saves (src/platform/null/saves.cpp, the memory card as a directory of
// the host's filesystem) through the sequence the game's save code runs, with the retail file set (icon.sys, the icon, the
// folder file and the four banks), then the error paths, then a fresh process re-reading what was left. Every step prints
// PASS/FAIL and the numbers it saw. Build (from the repo root):
//   python3 configure.py && ninja saves-proof
// Run:
//   build/host/saves_proof              all the steps (the fresh-process check runs itself)
//   build/host/saves_proof --fresh      only the re-read of what an earlier run left (the persistence check on its own)
//   build/host/saves_proof --space-only the card is assumed almost full: the ops that need space must report ErrorNotEnoughSpace
// The card's root is $XDG_DATA_HOME/twinsanity, ~/.local/share/twinsanity without it.
#include "platform/saves.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using Platform::Saves::FileEntry;
using Platform::Saves::Operation;
using Platform::Saves::Result;
using Platform::Saves::StorageInfo;

// The game's own codes (savedevice.cpp's Region/Product and the empty g_SaveName, savemanager.cpp's D_00309958)
constexpr const char* Region = "BE";
constexpr const char* Product = "SLES-52568";
constexpr const char* GameSave = "";
constexpr const char* NamedSave = "PROOF";
// The port and slot the retail game's device is on (SaveDevice::Construct)
constexpr s32 Port = 0;
constexpr s32 Slot = 1;

// The retail save's files: Sony's 0x3C4 icon.sys, a 4 KiB icon, the 0x800 folder named after the product and four 0xF400 banks
struct RetailFile
{
    const char* name;
    u32 size;
    u32 seed;
};

constexpr RetailFile GameFiles[] = {
    {"icon.sys", 0x3C4, 0x1000}, {"Crash.ico", 0x1000, 0x2000}, {"BESLES-52568", 0x800, 0x3000},
    {"Bank0.bin", 0xF400, 0x4000}, {"Bank1.bin", 0xF400, 0x5000}, {"Bank2.bin", 0xF400, 0x6000},
    {"Bank3.bin", 0xF400, 0x7000},
};
constexpr u32 GameFileCount = sizeof(GameFiles) / sizeof(GameFiles[0]);

int g_Failures = 0;

// The card's root directory
std::string BasePath()
{
    const char* dataHome = ::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0')
    {
        return std::string(dataHome) + "/twinsanity";
    }

    const char* home = ::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
    {
        return std::string(home) + "/.local/share/twinsanity";
    }

    return "/tmp/twinsanity";
}

std::string CardPath()
{
    return BasePath() + "/card0_1";
}

std::string SavePath(const char* save)
{
    return CardPath() + "/" + Region + Product + save;
}

bool Exists(const std::string& path)
{
    struct stat status;
    return ::stat(path.c_str(), &status) == 0;
}

bool IsDirectory(const std::string& path)
{
    struct stat status;
    return ::stat(path.c_str(), &status) == 0 && (status.st_mode & S_IFDIR) != 0;
}

s32 FreeKilobytes()
{
    struct statvfs status;
    if (::statvfs(BasePath().c_str(), &status) != 0 && ::statvfs("/", &status) != 0)
    {
        return -1;
    }

    return static_cast<s32>((static_cast<u64>(status.f_bavail) * static_cast<u64>(status.f_frsize)) >> 10);
}

// A file's bytes: a deterministic xorshift fill, so a fresh process can check the same ones
std::vector<u8> Data(u32 size, u32 seed)
{
    std::vector<u8> bytes(size);
    u32 state = seed * 2654435761u + 1u;
    for (u8& byte : bytes)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<u8>(state >> 24);
    }

    return bytes;
}

// The kilobytes the save should measure with the host's own arithmetic (half a kilobyte an entry, the directory's "." and ".."
// among them, and two for the directory itself)
u32 ExpectedKilobytes(const RetailFile* files, u32 count)
{
    u32 kilobytes = 0;
    for (u32 i = 0; i < count; i++)
    {
        kilobytes += Platform::Saves::FileKilobytes(files[i].size);
    }

    return kilobytes + (count + 1) / 2 + 2;
}

void Assert(bool ok, const char* what)
{
    std::printf("    %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
    {
        g_Failures++;
    }
}

// One queued operation walked to its end, its result and the card's info printed
Result Step(const char* what, bool started, Operation operation)
{
    StorageInfo info;
    Operation running = Platform::Saves::Update(Port, Slot, &info);
    Result result = Platform::Saves::GetResult(operation);
    std::printf("  %-36s started=%d update=%-2d result=(%d,%d,%d) info=(type %d, free %d KB, formatted %d)\n", what,
                started ? 1 : 0, static_cast<s32>(running), result.succeeded, result.step, result.error, info.type,
                info.freeKilobytes, info.formatted);
    if (!started)
    {
        g_Failures++;
    }

    return result;
}

// Every file of the save, its size and its bytes checked against the fill that was written
bool ReadBackFiles(const char* save, bool full)
{
    const RetailFile* files = full ? GameFiles : GameFiles;
    u32 count = full ? GameFileCount : 1;
    bool good = true;
    for (u32 i = 0; i < count; i++)
    {
        s32 size = 0;
        FileEntry entry;
        bool started = Platform::Saves::FindFile(Port, Slot, files[i].name, save, &size, &entry);
        StorageInfo info;
        Platform::Saves::Update(Port, Slot, &info);
        Result found = Platform::Saves::GetResult(Operation::FindFile);
        std::printf("  FindFile(\"%s\", \"%s\")          started=%d result=(%d,%d,%d) size=%d (wrote %u)\n", files[i].name,
                    save, started ? 1 : 0, found.succeeded, found.step, found.error, size, files[i].size);
        bool sizeOk = found.succeeded == 1 && size == static_cast<s32>(files[i].size);
        bool nameOk = std::strcmp(entry.name, files[i].name) == 0 && entry.size == files[i].size;
        std::vector<u8> back(files[i].size, 0);
        bool readStarted = Platform::Saves::Read(Port, Slot, save, files[i].name, back.data(), static_cast<s32>(back.size()));
        Platform::Saves::Update(Port, Slot, &info);
        Result read = Platform::Saves::GetResult(Operation::Read);
        bool bytesOk = read.succeeded == 1 && back == Data(files[i].size, files[i].seed);
        std::printf("  Read(\"%s\", \"%s\")             started=%d result=(%d,%d,%d) bytes %s\n", files[i].name, save,
                    readStarted ? 1 : 0, read.succeeded, read.step, read.error, bytesOk ? "identical" : "DIFFERENT");
        if (!sizeOk || !nameOk || !bytesOk)
        {
            std::printf("    entry.name=\"%s\" entry.size=%u\n", entry.name, entry.size);
        }

        good = good && sizeOk && nameOk && bytesOk;
    }

    return good;
}

int Fresh();

int Full()
{
    std::printf("saves-proof: card root %s (%d KB free)\n", BasePath().c_str(), FreeKilobytes());

    // 1. the card: Initialise and a check of its info
    Platform::Saves::Initialise(Region, Product);
    StorageInfo info;
    Operation running = Platform::Saves::Update(Port, Slot, &info);
    std::printf("  after Initialise(\"%s\", \"%s\"): update=%d info=(type %d, free %d KB, formatted %d)\n", Region, Product,
                static_cast<s32>(running), info.type, info.freeKilobytes, info.formatted);
    Assert(info.type == Platform::Saves::StorageMemoryCard && info.formatted == 1,
           "the storage is a formatted PS2 memory card");

    // 2. Format makes the card's directory
    bool formatStarted = Platform::Saves::Format(Port, Slot);
    Result format = Step("Format(0, 1)", formatStarted, Operation::Format);
    Assert(format.succeeded == 1, "Format succeeded");
    Assert(IsDirectory(CardPath()), "the card directory exists under the save root");

    // 3. CreateSave: a named save and the game's own (empty) name
    bool createStarted = Platform::Saves::CreateSave(Port, Slot, NamedSave);
    Result create = Step("CreateSave(0, 1, \"PROOF\")", createStarted, Operation::CreateSave);
    Assert(create.succeeded == 1, "CreateSave succeeded");
    std::printf("    save directory: %s (%s)\n", SavePath(NamedSave).c_str(), IsDirectory(SavePath(NamedSave)) ? "exists" : "MISSING");
    Assert(IsDirectory(SavePath(NamedSave)), "the save directory was made with the expected name (BE+SLES-52568+PROOF)");

    bool gameCreateStarted = Platform::Saves::CreateSave(Port, Slot, GameSave);
    Result gameCreate = Step("CreateSave(0, 1, \"\")  (the game's name)", gameCreateStarted, Operation::CreateSave);
    Assert(gameCreate.succeeded == 1, "CreateSave of the game's empty name succeeded");
    std::printf("    game save directory: %s (%s)\n", SavePath(GameSave).c_str(), IsDirectory(SavePath(GameSave)) ? "exists" : "MISSING");
    Assert(IsDirectory(SavePath(GameSave)), "the game's save directory (the region and product alone) was made");

    // 4. the retail files written into the game's save and read back byte for byte
    bool wroteAll = true;
    bool readAll = true;
    for (u32 i = 0; i < GameFileCount; i++)
    {
        std::vector<u8> data = Data(GameFiles[i].size, GameFiles[i].seed);
        bool writeStarted = Platform::Saves::Write(Port, Slot, GameSave, GameFiles[i].name, data.data(), static_cast<s32>(data.size()));
        Result write = Step(GameFiles[i].name, writeStarted, Operation::Write);
        wroteAll = wroteAll && write.succeeded == 1;
        std::vector<u8> back(data.size(), 0);
        bool readStarted = Platform::Saves::Read(Port, Slot, GameSave, GameFiles[i].name, back.data(), static_cast<s32>(back.size()));
        Result read = Step(GameFiles[i].name, readStarted, Operation::Read);
        bool same = read.succeeded == 1 && back == data;
        if (!same)
        {
            u32 at = 0;
            while (at < data.size() && back[at] == data[at])
            {
                at++;
            }

            std::printf("    wrote %u bytes, read %u bytes, first difference at %u\n", static_cast<u32>(data.size()),
                        static_cast<u32>(back.size()), at);
        }

        readAll = readAll && same;
    }

    Assert(wroteAll, "every retail file was written");
    Assert(readAll, "every file read back byte for byte identical");

    // a file into the named save too (the directory-per-save split)
    bool namedWriteStarted = Platform::Saves::Write(Port, Slot, NamedSave, "note.txt", Data(16, 0x11).data(), 16);
    Result namedWrite = Step("Write PROOF/note.txt", namedWriteStarted, Operation::Write);
    Assert(namedWrite.succeeded == 1 && Exists(SavePath(NamedSave) + "/note.txt"), "the named save took a file of its own");

    // 5. FindFile / MeasureSave / FileKilobytes numbers
    std::printf("  FileKilobytes: 0=%u 1=%u 0x400=%u 0x401=%u 0xF400=%u\n",
                Platform::Saves::FileKilobytes(0), Platform::Saves::FileKilobytes(1), Platform::Saves::FileKilobytes(0x400),
                Platform::Saves::FileKilobytes(0x401), Platform::Saves::FileKilobytes(0xF400));
    Assert(Platform::Saves::FileKilobytes(0) == 0 && Platform::Saves::FileKilobytes(1) == 1 &&
               Platform::Saves::FileKilobytes(0x400) == 1 && Platform::Saves::FileKilobytes(0x401) == 2 &&
               Platform::Saves::FileKilobytes(0xF400) == 61,
           "FileKilobytes rounds up to whole kilobytes");
    std::printf("  SaveBytes: (61,1)=%u (61,4)=%u (257,7)=%u bytes\n", Platform::Saves::SaveBytes(61, 1),
                Platform::Saves::SaveBytes(61, 4), Platform::Saves::SaveBytes(257, 7));
    Assert(Platform::Saves::SaveBytes(61, 1) == 64u << 10 && Platform::Saves::SaveBytes(61, 4) == 65u << 10 &&
               Platform::Saves::SaveBytes(257, 7) == 263u << 10,
           "SaveBytes adds the directory's half-kilobyte entries and clusters");
    Assert(ReadBackFiles(GameSave, true), "FindFile and Read see the bytes that were written");

    s32 gameKilobytes = 0;
    bool measureStarted = Platform::Saves::MeasureSave(Port, Slot, GameSave, &gameKilobytes);
    Result measure = Step("MeasureSave(0, 1, \"\")", measureStarted, Operation::MeasureSave);
    std::printf("    measured %d KB, expected %u KB\n", gameKilobytes, ExpectedKilobytes(GameFiles, GameFileCount));
    Assert(measure.succeeded == 1 && gameKilobytes == static_cast<s32>(ExpectedKilobytes(GameFiles, GameFileCount)),
           "MeasureSave returned the kilobytes the retail files take");
    // The game's SaveDevice::SaveSpace sums the same kilobytes and calls SaveBytes; it must agree with MeasureSave
    u32 neededKilobytes = Platform::Saves::FileKilobytes(0x3C4) + Platform::Saves::FileKilobytes(0x1000) +
                          Platform::Saves::FileKilobytes(0x800) + 4 * Platform::Saves::FileKilobytes(0xF400);
    u32 neededBytes = Platform::Saves::SaveBytes(neededKilobytes, 7);
    std::printf("    SaveDevice::SaveSpace equivalent: SaveBytes(%u, 7)=%u bytes vs MeasureSave<<10=%u bytes\n",
                neededKilobytes, neededBytes, static_cast<u32>(gameKilobytes) << 10);
    Assert(neededBytes == (static_cast<u32>(gameKilobytes) << 10),
           "the game's SaveSpace arithmetic agrees with MeasureSave");

    s32 namedKilobytes = 0;
    bool namedMeasureStarted = Platform::Saves::MeasureSave(Port, Slot, NamedSave, &namedKilobytes);
    Result namedMeasure = Step("MeasureSave(0, 1, \"PROOF\")", namedMeasureStarted, Operation::MeasureSave);
    std::printf("    measured %d KB (one 16 byte file: 1+? KB)\n", namedKilobytes);
    Assert(namedMeasure.succeeded == 1 && namedKilobytes > 0, "MeasureSave of the named save returned its size");

    s32 missingKilobytes = 0;
    bool missingMeasureStarted = Platform::Saves::MeasureSave(Port, Slot, "NOPE", &missingKilobytes);
    Result missingMeasure = Step("MeasureSave(0, 1, \"NOPE\")", missingMeasureStarted, Operation::MeasureSave);
    Assert(missingMeasure.succeeded == 1 && missingKilobytes == -1, "MeasureSave of a save that isn't there returned -1");

    // 6. the error paths
    u8 scratch[0x40];
    bool readMissingStarted = Platform::Saves::Read(Port, Slot, GameSave, "NotHere.bin", scratch, sizeof(scratch));
    Result readMissing = Step("Read a file that isn't there", readMissingStarted, Operation::Read);
    Assert(readMissing.succeeded == 0 && readMissing.error == Platform::Saves::ErrorNoSave,
           "reading a nonexistent file reports ErrorNoSave");

    bool readNoSaveStarted = Platform::Saves::Read(Port, Slot, "NoSave", "icon.sys", scratch, sizeof(scratch));
    Result readNoSave = Step("Read from a save that isn't there", readNoSaveStarted, Operation::Read);
    Assert(readNoSave.succeeded == 0 && readNoSave.error == Platform::Saves::ErrorNoSave,
           "reading from a nonexistent save reports ErrorNoSave");

    bool writeNoSaveStarted = Platform::Saves::Write(Port, Slot, "NoSave", "icon.sys", scratch, sizeof(scratch));
    Result writeNoSave = Step("Write into a save that isn't there", writeNoSaveStarted, Operation::Write);
    Assert(writeNoSave.succeeded == 0 && writeNoSave.error == Platform::Saves::ErrorNoSave,
           "writing into a nonexistent save reports ErrorNoSave");
    Assert(!Exists(CardPath() + "/BESLES-52568NoSave"), "the refused write left no save directory behind");

    if (FreeKilobytes() < 4096)
    {
        std::printf("  Write beyond space: SKIPPED (the filesystem isn't nearly full; run --space-only on one that is)\n");
    }
    else
    {
        std::printf("  Write beyond space: SKIPPED (card free %d KB; run --space-only on an almost-full filesystem)\n",
                    FreeKilobytes());
    }

    // 7. a fresh process reads the card back
    std::printf("  fresh process: re-reading the card\n");
    ::fflush(stdout);
    char self[4096];
    ssize_t length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0)
    {
        self[length] = '\0';
        pid_t child = ::fork();
        if (child == 0)
        {
            ::execl(self, self, "--fresh", static_cast<char*>(nullptr));
            ::_exit(127);
        }

        int status = 0;
        ::waitpid(child, &status, 0);
        Assert(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a fresh process found and read the save back");
    }
    else
    {
        Assert(false, "could not locate its own executable for the fresh-process check");
    }

    std::printf("saves-proof: %s (%d failures)\n", g_Failures == 0 ? "PASS" : "FAIL", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}

// The persistence check: what the run before left, read by a process that wrote nothing
int Fresh()
{
    Platform::Saves::Initialise(Region, Product);
    StorageInfo info;
    Platform::Saves::Update(Port, Slot, &info);
    std::printf("  fresh: card root %s info=(type %d, free %d KB, formatted %d)\n", BasePath().c_str(), info.type,
                info.freeKilobytes, info.formatted);
    Assert(info.type == Platform::Saves::StorageMemoryCard, "the fresh process still sees a memory card");

    s32 kilobytes = 0;
    Platform::Saves::MeasureSave(Port, Slot, GameSave, &kilobytes);
    Platform::Saves::Update(Port, Slot, &info);
    std::printf("  fresh: MeasureSave(\"\")=%d KB, expected %u KB\n", kilobytes, ExpectedKilobytes(GameFiles, GameFileCount));
    Assert(kilobytes == static_cast<s32>(ExpectedKilobytes(GameFiles, GameFileCount)), "the save is still the right size");

    bool good = ReadBackFiles(GameSave, true);
    Assert(good, "every file is still there, byte for byte");
    std::printf("saves-proof --fresh: %s (%d failures)\n", g_Failures == 0 ? "PASS" : "FAIL", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}

// The almost-full card: the operations that need room must report ErrorNotEnoughSpace and leave nothing behind
int SpaceOnly()
{
    // The harness lays the card and the save's directory down first (an empty directory takes no space): the free space is
    // then what the operations must check against
    ::mkdir(BasePath().c_str(), 0777);
    ::mkdir(CardPath().c_str(), 0777);
    ::mkdir(SavePath(GameSave).c_str(), 0777);
    Platform::Saves::Initialise(Region, Product);
    StorageInfo info;
    Platform::Saves::Update(Port, Slot, &info);
    std::printf("saves-proof --space-only: card root %s (free %d KB)\n", BasePath().c_str(), info.freeKilobytes);
    Assert(info.freeKilobytes < 8, "the card is nearly full (under 8 KB free)");

    bool createStarted = Platform::Saves::CreateSave(Port, Slot, "TIGHT");
    Result create = Step("CreateSave(0, 1, \"TIGHT\")", createStarted, Operation::CreateSave);
    Assert(create.succeeded == 0 && create.error == Platform::Saves::ErrorNotEnoughSpace,
           "CreateSave reports ErrorNotEnoughSpace");
    Assert(!Exists(CardPath() + "/BESLES-52568TIGHT"), "no save directory was left behind");

    std::vector<u8> data = Data(0x2000, 0xBEEF);
    bool writeStarted = Platform::Saves::Write(Port, Slot, GameSave, "full.bin", data.data(), static_cast<s32>(data.size()));
    Result write = Step("Write 8 KB into a full card", writeStarted, Operation::Write);
    Assert(write.succeeded == 0 && write.error == Platform::Saves::ErrorNotEnoughSpace,
           "Write beyond space reports ErrorNotEnoughSpace");
    Assert(!Exists(SavePath(GameSave) + "/full.bin"), "no partially written file was left behind");

    std::printf("saves-proof --space-only: %s (%d failures)\n", g_Failures == 0 ? "PASS" : "FAIL", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--fresh") == 0)
        {
            return Fresh();
        }

        if (std::strcmp(argv[i], "--space-only") == 0)
        {
            return SpaceOnly();
        }
    }

    return Full();
}
