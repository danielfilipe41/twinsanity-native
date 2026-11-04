// The sound processor's memory, handed out in blocks: the sound banks' samples take a block each as the game reads them, the
// music players' buffers and the effect unit's work area are reserved, and the game asks for the blocks to be moved together
// every so many frames (its CompactSoundMemory), which is where the gaps released sounds left are closed.
//
// Blocks are addressed by the word, as the processor addresses them, and a block's size is a multiple of the processor's 8
// word block so that every sound starts on a block of its own.
#pragma once

#include "common.h"

#include <vector>

namespace SdlAudio::Spu2
{
// A block no sound is registered under (a reservation's own)
constexpr u32 NoSound = 0xFFFFFFFF;

// Where the memory the sound banks may use starts: past the processor's own low area (retail's SpuMemoryStart, 0x5010 bytes)
constexpr u32 MemoryBase = 0x5010 / 2;

// Part of the memory moved by a compaction: the caller follows the moves to keep anything it holds a block's address in step
struct MemoryMove
{
    u32 from = 0;
    u32 to = 0;
    u32 size = 0;
};

class SoundMemory
{
public:
    void Reset(u32 ceiling);

    // The memory's end, which the effect unit's work area sits above. False when the blocks already in the memory reach past
    // the new end, and then nothing changes
    bool SetCeiling(u32 ceiling);
    u32 Ceiling() const { return m_ceiling; }

    // A block for a sound, at the first gap that holds it or past the last block. False when there is no room
    bool Allocate(u32 words, u32& address, u32 sound = NoSound);
    bool Release(u32 sound);
    // Where a sound's block is, for a voice that has to follow it: false when it is not in the memory
    bool AddressOf(u32 sound, u32& address, u32& words) const;

    // Moves every block together over the gaps released ones left. The moves it made are in Moves() until the next one
    void Compact();
    const std::vector<MemoryMove>& Moves() const { return m_moves; }

    u32 Used() const;
    u32 Free() const;

private:
    struct Block
    {
        u32 address = 0;
        u32 words = 0;
        bool used = false;
        u32 sound = NoSound;
    };

    // The free blocks keep their place in the list so that the list is always sorted by address
    void Merge();

    std::vector<Block> m_blocks;
    std::vector<MemoryMove> m_moves;
    u32 m_ceiling = 0;
    // Past the highest block the memory has held: where the next new block goes
    u32 m_top = MemoryBase;
};
}
