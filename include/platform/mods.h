#pragma once

#include "common.h"

#include <string>
#include <vector>

// Mods: the community's mods, read from one folder so the game has them without a launcher.
//
// The folder holds one sub-folder per mod, and mods.txt lists the ones that are on, one per line, in the order they apply (a
// later mod's file replaces an earlier one's; '#' and ';' start a comment; a line of `*` or `all` turns on every mod the
// folder has). Three shapes are read, told apart by what is in them, never by their names:
//
//   * a Mod Crate (the community's, made for Crate Mod Loader): ModCrateInfo.txt, layer0/ (files of the disc) and layer1/
//     (entries of the game's archive, CRASH6/CRASH.BD, by their index path);
//   * a TwinsanityEvolved mod folder: mod.json with a disc/ tree laid out like the disc. A file of that tree may come as a
//     `.ranges` recipe (TwinsanityEvolved keeps only what a mod changes of a large file): the slices of the disc's own file
//     and of the mod's own payload file the file is made of, which Platform::Files composes as the game reads it;
//   * a zip of a mod (the community ships crates so): unpacked once into mods/<zip name>/, then read like any folder;
//   * a patch of the whole disc image (xdelta3's VCDIFF, PPF or BPS, as the community ships a change of the disc), dropped in
//     the mods folder as itself or as a folder of its own: the image is the patched one while the game reads it, with no copy
//     of it made (Platform::DiscPatch). One patch at a time, and only of a plain ISO image;
//   * a folder of the game's own files as its tools build them (TT Lab's build output, or loose files): a path under CRASH6/,
//     FMV/, SYSTEM.CNF or a disc serial is a file of the disc, a path under one of the archive's own folders is an entry of
//     the archive.
//
// A mod's disc files replace the disc's whole (Platform::Files::Open answers them first, read-only). Its archive entries are
// put into a rebuilt CRASH6/CRASH.BH and appended to a composed CRASH6/CRASH.BD, so an entry grows or shrinks without the
// disc being touched. While any mod is on, the game's saves go to the cards of the mods that are on (Platform::Saves): one
// folder per lot of mods, never the vanilla cards and never another lot's.
//
// Not applied, and said in the log when a mod carries them: a crate's ModCrateSettings.txt and scripts (code changes of Crate
// Mod Loader's own), a modified executable's code (the port runs its own), and a language pack (a disc's languages are its
// own files here).
//
// The game's own mods page (game/olegpages.h, under the options) lists the folder's mods from here and writes mods.txt back.
namespace Platform::Mods
{
// One entry of the archive a mod replaces or adds: its index path (the game's names, '\\' separators, upper case) and the
// host file holding its bytes
struct ArchiveFile
{
    std::string path;
    std::string host;
};

// One piece of a TwinsanityEvolved recipe (`<disc path>.ranges`): bytes of the disc's own file, of the mod's own payload, or
// zeroes
struct RangePiece
{
    enum class Kind
    {
        Base,    // the disc's own file, at `offset`
        Payload, // the mod's own file (`host`), at `offset`
        Zero,    // zeroes
    };

    Kind kind = Kind::Base;
    u64 offset = 0;
    u64 length = 0;
    std::string host; // Payload: the mod's own file
};

// A file of the disc a mod gives as the pieces it changes rather than whole, with the size its pieces add up to
struct Ranges
{
    std::string path; // the disc's path it is for (upper case, '/' separators)
    u64 size = 0;
    std::vector<RangePiece> pieces;
};

struct Mod
{
    std::string id;      // the folder's name
    std::string name, author, version, description;
    std::string kind;    // "crate" | "mod" (a mod.json) | "files" (the game's own files) | "patch" (a patch of the image)
    std::string dir;     // the folder read
    std::string patch;   // a patch mod's file
    bool on = false;     // applied by this run
    bool listed = false; // named by mods.txt (whether or not it could be applied)
    u32 discFiles = 0;      // files of the disc it replaces or adds
    u32 archiveFiles = 0;   // entries of the archive it replaces or adds
};

// Reads the mods folder and makes the mods its mods.txt lists apply. The folder is `folder` (--mods-dir), else
// TWINSANITY_MODS, else `preferred` (the settings file's `mods_dir`, a launcher's or the player's choice), else mods/ beside
// the disc's files, else ./mods, else `writable`: the host's own folder for the game's files (a phone's or a page's, where the
// disc itself cannot be written), which is made when it is not there. A chosen folder that cannot be written still has its
// mods read; the mods page cannot change its mods.txt and zips cannot be unpacked in it, which the log says.
//
// `extra` are the --mod ids of this run (they apply after the list's), `none` turns the folder's mods off (--no-mods). Call
// once, after Platform::Files::SetRoot and before the game opens anything; a second call forgets the first.
void Initialise(const char* folder = nullptr, const std::vector<std::string>& extra = {}, bool none = false,
                const char* preferred = nullptr, const char* writable = nullptr);

// The folder read ("" when none was found)
const char* Folder();
const std::vector<Mod>& Mods();
// Whether a mod is on and changes files
bool Enabled();
// The mods.txt as it was read: the mods that are on, in the order they apply
const std::vector<std::string>& Listed();
// The mods page's choice: mods.txt written with these mods, in this order (a later one's file replaces an earlier one's). The
// mods this run has open do not change: a restart applies the new list. False when there is no folder or the file cannot be
// written (what the folder had is kept).
bool WriteList(const std::vector<std::string>& ids);
// How many disc files and archive entries the mods apply (after later mods' files replaced earlier ones')
u32 DiscFileCount();
u32 ArchiveFileCount();

// The host file replacing this disc path, or nullptr. The path is one of the game's ("Crash6\\Crash.bh", any case, either
// slash); a file the mods add answers here too
const std::string* File(const char* discPath);
// The recipe a TwinsanityEvolved mod gives for this disc path (its `.ranges` file), or nullptr. Same paths as File
const Ranges* RangesFile(const char* discPath);
// The whole CRASH6/CRASH.BH and CRASH6/CRASH.BD a mod ships (a rebuilt archive pair), or nullptr
const std::string* ArchiveIndex();
const std::string* ArchiveData();
// The archive's entries the mods replace or add, in the order they are put into the index (deduplicated: a later mod's
// entry is the one here)
const std::vector<ArchiveFile>& ArchiveFiles();

// One line for the log: the folder, how many mods it has and which are on, what they apply. "" when there is no folder.
std::string Describe();

// What the game will read, through Platform::Files: the archive's index as the mods rebuilt it, every entry they replace or
// add (its bytes compared with the mod's own file), and whether each of their disc files opens. Prints one line per problem
// and a summary; false when anything does not read (--verify-mods).
bool Verify();
}
