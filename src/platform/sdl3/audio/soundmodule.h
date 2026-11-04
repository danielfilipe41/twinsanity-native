// The sound module: what the game's sound code talks to. On the PS2 that is the disc's STREAM.IRX on the I/O processor, and
// this is its side of the bargain, as the game's logic expects it:
//
//  - a table of the sounds in the processor's memory, each under the id its resource header gave it;
//  - a channel per voice, whose key status is what the game reads back as "is this voice free": a voice the game has let go
//    of is only free once its envelope has run out, and only the module can say so;
//  - the volume groups, applied to the channels' volume registers, since the processor has no notion of them;
//  - an effect per core, from libsd's own preset registers;
//  - and the memory, its blocks moved together when the game asks (CompactSoundMemory), with every voice that was reading a
//    moved block following it.
//
// The game never re-feeds a sound or a track: it plays, polls, and expects the module to keep going. Everything here that
// looks like bookkeeping the game could do itself is bookkeeping the game does not do.
#pragma once

#include "common.h"
#include "soundmemory.h"
#include "spu2.h"

#include <vector>

namespace SdlAudio::Spu2
{
// A voice's group goes with its number in the top half of a word (voice | group << GroupShift)
constexpr u32 GroupShift = 16;
constexpr s32 GroupCount = 4;

// The processor's volume registers: full is 0x3FFF, past it the other phase of the wave, and a group's volume of 1 is 0x1000
constexpr s32 MaxVolume = 0x3FFF;
constexpr s32 InvertedVolumeBase = 0x7FFF;
constexpr s16 FullGroupVolume = 0x1000;

// The sounds the game plays are its resources' header ids: the resource tables' place in their low 15 bits, the language's
// spoken lines at bit 15
constexpr u32 SoundSlots = 0x10000;

// The bank tools' header before a bank's samples: "MSVp", the size, the rate and the sound's name, 0x40 bytes, and the first
// block of samples is the one after it. A voice has to start there: the header read as ADPCM is a block of nonsense whose
// flags say the sound ends at once (checked against the disc's own banks)
constexpr u32 BankHeaderBytes = 0x40;

// A channel's key status, as the module reports it: the game's IsVoiceFree reads Off, and a channel that has been played or
// is still fading out is not free
enum class ChannelState : u8
{
    Off,
    // Keyed but not yet started (the module's key-on queue)
    Requested,
    // Playing, or fading out after its key-off
    KeyedOn,
};

// A core's effect, from the game's settings and libsd's preset for the mode
struct CoreEffect
{
    s32 mode = 0;
    u16 depthLeft = 0;
    u16 depthRight = 0;
    u16 delay = 0;
    u16 feedback = 0;
    s16 volumeLeft = 0;
    s16 volumeRight = 0;
};

// The channels a music stream can be opened on (the game's file streams are its channels, one per player and side)
constexpr s32 StreamChannels = 48;

// The ring a music stream plays out of: the game reserves 0x8000 bytes for a player, half for each side of an interleaved track
// (the module's MusicStreamBuffer). A ring this size holds about a third of a second of a track, which is how far the module
// reads ahead of the voice.
constexpr u32 StreamRingBytes = 0x4000;
constexpr u32 StreamRingWords = StreamRingBytes / 2;

// A music stream: a channel's own region of the disc's ADPCM, laid into a ring of the processor's memory as it plays. The
// ring is what the game reserved for the player (0x4000 bytes, the module's MusicStreamBuffer), and the voice reads it exactly
// as it reads a sound in a bank: the blocks' own flags are the loop and the end.
struct MusicStream
{
    bool bound = false;
    // The region is in the host's memory, so the channel can be played (what IsMusicReady reports)
    bool ready = false;
    // The module still has the stream: true from the moment it is started until it ends or is stopped, whatever the pause or
    // a movie does in between
    bool playing = false;
    bool once = false;
    bool looping = true;
    // An interleaved track's second side: the parent reads the disc, the child plays the chunks the parent leaves
    bool child = false;
    s32 voice = -1;
    u32 group = 0;
    u16 pitch = 0x1000;
    u32 blockSize = 0;
    s32 parent = -1;
    s32 childChannel = -1;

    // The region as the game named it, and its bytes (the file, where in it, how much)
    s32 file = -1;
    u32 offset = 0;
    u32 size = 0;
    u32 endOffset = 0;
    // The region's ADPCM, read once when the stream is set up: the ring is filled from here, so the audio thread never reads
    // the disc
    std::vector<u8> data;

    // The ring in the processor's memory
    u32 ringAddress = 0;
    u32 ringWords = 0;
    u32 writeBlock = 0;

    // Where the next block of the region comes from
    u32 nextChunk = 0;
    u32 chunkBlock = 0;
    bool finished = false;
    // Whether the voice has been keyed on. A stream is the module's from the moment it is started (retail marks the channel
    // taken then), but its voice only sounds from the play: until then a voice that is not sounding is nothing wrong, and
    // reading it as a fault called every track's first frame a side that stopped on its own
    bool started = false;
};

class SoundModule
{
public:
    void Reset();

    // One stereo frame at a time into the stream: the processor's own 48 kHz, no resampling
    void Mix(s16* stereo, u32 frames);

    // ---- what the game's sound code calls ----
    void PauseAll();
    void ResumeAll();
    void LendToMovie();
    void ReclaimFromMovie();
    void CompactSoundMemory();

    void SetGroupVolume(s32 group, u32 left, u32 right);
    s32 SetVoiceVolume(s32 voice, s16 left, s16 right);
    s32 SetVoicePitch(s32 voice, u16 pitch);
    void SetVoiceReverb(s32 voice, bool on);
    void ReleaseVoice(s32 voice, u32 rate);
    s32 IsVoiceFree(s32 voice);

    void SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback);
    void ClearReverb(s32 core);
    void SetReverbVolume(s32 core, s16 left, s16 right);

    u32 ReserveSoundMemory(u32 bytes);
    void SoundBankLoaded(u16 bank);
    void ReserveSounds(u16 count);
    void ReleaseSound(u32 sound);
    u32 LoadSoundBank(u32 sound, const u8* data, u32 size);
    s32 PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release, u32 loops);

    s32 PitchOfRate(s32 rate);
    u32 ChannelBufferAddress(s32 channel);

    // ---- the music's streams ----
    // The game opens a file stream on a channel, hands it the ring the player reserved in the processor's memory, then asks
    // for a region of the disc to be streamed through it, prepares it, and plays it. The module keeps the stream's state
    // from the first call to the last, since the game only ever polls it afterwards.
    void SetChannelBuffer(s32 channel, u32 address, u32 words);
    void ReadMusic(s32 file, u32 offset, u32 size);
    // The region ReadMusic named, read where the caller isn't holding the device lock (StreamMusic's own read would park
    // the mixer for the disc read's length)
    void PrepareStream(s32 file, std::vector<u8>* data);
    // prepared: the region read by PrepareStream, taken over here; null reads it in place (the lock is held then)
    s32 StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once,
                    std::vector<u8>* prepared = nullptr);
    void InterleaveMusic(s32 channel, u32 blockSize);
    void AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress);
    void SetMusicEnd(s32 channel, u32 end);
    void PrepareMusic(s32 channel);
    // Lays the first blocks of a stream's ring (and of its other side's) before a voice is let loose on it
    void PrimeStream(s32 channel);
    bool IsMusicReady(s32 channel);
    s32 PlayMusic(s32 channel);
    bool IsMusicPlaying(s32 channel);
    void StopMusic(s32 channel);

    // For the proofs
    u32 MemoryUsed() const { return m_memory.Used(); }
    ChannelState StateOf(s32 voice) const;
    // Whether one of the music's streams is playing on this voice: such a voice is not one of the game's channels, and the
    // group volume has to reach it all the same
    bool IsStreamVoice(s32 voice) const;

private:
    struct Sound
    {
        bool used = false;
        u32 address = 0;
        u32 words = 0;
        // How many 16 byte blocks of samples the sound is: where its data ends, whatever its flags say
        u32 blocks = 0;
    };

    struct Channel
    {
        ChannelState state = ChannelState::Off;
        u32 sound = NoSound;
        s32 group = 0;
        // The volume the game asked for, and the level it comes to with the group's volume applied
        s16 rawLeft = 0;
        s16 rawRight = 0;
        s32 levelLeft = 0;
        s32 levelRight = 0;
        s32 targetLeft = 0;
        s32 targetRight = 0;
        // The passes the sound has left (one unless the game asked for a loop count; never 0, since a 0 would mean the
        // processor's own loop flag went uncounted), and where it was reading last frame
        u32 passesLeft = 1;
        u32 lastNext = 0;
    };

    void TargetChannel(s32 voice, s16 left, s16 right, bool immediate);
    void StepRamps();
    void TrackChannels();
    void UpdateEffectAreas();

    // The streams: a block of a region into its ring, the ring topped up behind the voice's reading, and the channel found to
    // have ended
    void WriteStreamBlock(MusicStream& stream);
    void FillStream(MusicStream& stream);
    void FillStreams();
    void TrackStreams();
    void EndStream(s32 channel);
    // Keys the channel's voice (and its other side's) off, so nothing of a track it was given goes on sounding
    void LetStreamVoiceGo(s32 channel);
    // What a stream lays into its ring: its own region, or for the second side of an interleaved pair the region the parent
    // read (the child reads nothing itself in the retail module either)
    const std::vector<u8>& StreamData(const MusicStream& stream) const;

    SoundMemory m_memory;
    Sound m_sounds[SoundSlots];
    Channel m_channels[VoiceCount];
    MusicStream m_streams[StreamChannels];
    CoreEffect m_effects[CoreCount];
    s16 m_groupLeft[GroupCount] = {};
    s16 m_groupRight[GroupCount] = {};
    // The region the next StreamMusic is handed (the module's one pending read)
    s32 m_pendingFile = -1;
    u32 m_pendingOffset = 0;
    u32 m_pendingSize = 0;

    bool m_paused = false;
    // The module's key-ons are off while a movie has it, but nothing it holds is let go: the game's state machine keeps
    // asking about the same tracks when it gets the module back
    bool m_lent = false;
};

SoundModule& Module();

// The level a 16 bit volume register comes to in the mix
inline s32 LevelOfRegister(s32 raw)
{
    return static_cast<s16>(static_cast<u16>(raw << 1));
}

// The register for a level in the mix. Its top bit is the volume slide's enable, so a level below zero is the register's
// other phase (0x8000 less its magnitude: full inverted is 0x4000, near-silence 0x7FFF), not the level shifted through its sign
inline u16 RegisterOfLevel(s32 level)
{
    return static_cast<u16>(static_cast<u16>(level) >> 1);
}
}
