// The sound processor's memory, handed out in blocks.
#include "soundmemory.h"

#include <algorithm>

namespace SdlAudio::Spu2
{
void SoundMemory::Reset(u32 ceiling)
{
    m_blocks.clear();
    m_moves.clear();
    m_ceiling = ceiling;
    m_top = MemoryBase;
}

bool SoundMemory::SetCeiling(u32 ceiling)
{
    // A block that reaches past the new end would be left pointing at the effect's work area: nothing moves here
    for (const Block& block : m_blocks)
    {
        if (block.used && block.address + block.words > ceiling)
        {
            return false;
        }
    }

    m_ceiling = ceiling;
    return true;
}

bool SoundMemory::Allocate(u32 words, u32& address, u32 sound)
{
    // The processor reads 8 word blocks, so a sound's block is a whole number of them
    u32 wanted = (words + 7) & ~7u;
    if (wanted == 0)
    {
        address = 0;
        return true;
    }

    for (size_t index = 0; index < m_blocks.size(); index++)
    {
        Block& block = m_blocks[index];
        if (block.used || block.words < wanted)
        {
            continue;
        }
        if (block.address + wanted > m_ceiling)
        {
            continue;
        }

        address = block.address;
        block.used = true;
        block.sound = sound;
        if (block.words > wanted)
        {
            Block rest;
            rest.address = block.address + wanted;
            rest.words = block.words - wanted;
            rest.used = false;
            block.words = wanted;
            m_blocks.insert(m_blocks.begin() + static_cast<std::ptrdiff_t>(index) + 1, rest);
        }
        return true;
    }

    if (m_top + wanted > m_ceiling)
    {
        return false;
    }

    address = m_top;
    m_top += wanted;
    Block block;
    block.address = address;
    block.words = wanted;
    block.used = true;
    block.sound = sound;
    m_blocks.push_back(block);
    return true;
}

bool SoundMemory::Release(u32 sound)
{
    for (size_t index = 0; index < m_blocks.size(); index++)
    {
        Block& block = m_blocks[index];
        if (!block.used || block.sound != sound)
        {
            continue;
        }

        block.used = false;
        block.sound = NoSound;

        // The memory's end comes back to the highest block left, and that block is not a hole in the list any more: it is
        // above the end, which is the free space the arena grows into. A block left listed *and* under the end is memory
        // handed out twice - the next sound that did not fit a hole was given the released block's own address
        if (block.address + block.words == m_top)
        {
            m_top = block.address;
            m_blocks.erase(m_blocks.begin() + static_cast<std::ptrdiff_t>(index));
            Merge();
            return true;
        }

        Merge();
        return true;
    }

    return false;
}

bool SoundMemory::AddressOf(u32 sound, u32& address, u32& words) const
{
    for (const Block& block : m_blocks)
    {
        if (block.used && block.sound == sound)
        {
            address = block.address;
            words = block.words;
            return true;
        }
    }

    return false;
}

// The free blocks either side of a released one are one gap again
void SoundMemory::Merge()
{
    for (size_t index = 0; index + 1 < m_blocks.size();)
    {
        if (!m_blocks[index].used && !m_blocks[index + 1].used)
        {
            m_blocks[index].words += m_blocks[index + 1].words;
            m_blocks.erase(m_blocks.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        }
        else
        {
            index++;
        }
    }
}

void SoundMemory::Compact()
{
    m_moves.clear();

    // Every block goes down onto the one before it: what the retail module does with its sounds, so a session's loads and
    // drops do not leave the memory in pieces
    u32 next = MemoryBase;
    for (Block& block : m_blocks)
    {
        if (!block.used)
        {
            continue;
        }

        if (block.address != next)
        {
            MemoryMove move;
            move.from = block.address;
            move.to = next;
            move.size = block.words;
            m_moves.push_back(move);
            block.address = next;
        }

        next += block.words;
    }

    // The gaps are one free block past the last sound
    m_blocks.erase(std::remove_if(m_blocks.begin(), m_blocks.end(), [](const Block& block) { return !block.used; }),
                   m_blocks.end());

    if (m_top > next)
    {
        m_top = next;
    }
}

u32 SoundMemory::Used() const
{
    u32 words = 0;
    for (const Block& block : m_blocks)
    {
        if (block.used)
        {
            words += block.words;
        }
    }

    return words;
}
}
