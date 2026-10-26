// The sound processor's mixer: 48 SPU2 voices (2 cores of 24) playing
// PlayStation ADPCM out of its memory, or a music stream decoded from its
// region, through SDL3's 48 kHz stereo stream.
#pragma once

#include "common.h"
#include <atomic>

#include "platform/audio.h"

#include <vector>
namespace SdlAudio
{
// The sound processor's memory, where the sound banks' samples go
constexpr u32 SpuMemorySize = 2 * 1024 * 1024;

// What a voice reads: its memory (a sound bank's ADPCM blocks) or a music
// channel's stream
enum class VoiceSource : u8
{
    None,
    Spu,
    Stream,
};

// A music channel's region, decoded once into PCM. Interleaved music has two
// channels: the parent reads the even chunks of the region, the child the odd
// ones (blockSize bytes each).
struct MusicChannel
{
    bool bound = false;
    bool ready = false;
    bool playing = false;
    bool looping = true;
    s32 parent = -1;
    s32 child = -1;
    u32 voice = 0;
    u32 group = 0;
    u32 blockSize = 0;
    // The region's file and place in it
    s32 file = -1;
    u32 offset = 0;
    u32 size = 0;
    // The channel's decoded samples: the whole region, looped from the start
    // when the stream loops
    std::vector<s16> samples;
    // The last chunk's real data length (SetMusicEnd's: the music's data can
    // end part way into the final block)
    u32 endOffset = 0;
};

// A voice's envelope, as the SPU2's ADSR registers describe it. The game's
// sounds only set an attack rate and a release rate, so they sustain at full
// level until let go.
struct Envelope
{
    u32 value = 0; // 0 to 0x7FFFFFFF
    u8 phase = 0;  // 1 attack, 2 decay, 3 sustain, 5 release, 6 off
    u32 attackRate = 0;
    u32 decayRate = 0x1F;
    u32 sustainRate = 0x7F;
    u32 sustainLevel = 0xF;
    u32 releaseRate = 0xD;
    bool attackExponential = false;
    bool sustainExponential = false;
    bool releaseExponential = false;
    bool sustainDecreasing = false;
};

// A voice: its source and place, its pitch counter and four sample history,
// its envelope, and its volumes. The volumes are kept twice: the game's raw
// register values and the values being mixed (the module's OpSetVolumeSmooth
// ramps the second to the first).
struct Voice
{
    std::atomic<bool> active = false;
    VoiceSource source = VoiceSource::None;
    s32 stream = -1;
    bool paused = false;

    // A sound in the sound processor's memory
    u32 spuAddress = 0;
    u32 spuSize = 0;
    u32 loopStartBlock = 0;
    bool hasLoop = false;

    // Playback: an index and a 12-bit fraction, and the pitch added each
    // output frame (0x1000 plays the sample at its own rate)
    u64 index = 0;
    u32 fraction = 0;
    s32 pitch = 0x1000;
    s32 history[4] = {}; // oldest to newest

    // The decoded block being played (an SPU source)
    u32 blockIndex = 0xFFFFFFFF;
    u8 blockFlags = 0;
    s16 blockSamples[28] = {};
    s32 blockOld = 0;
    s32 blockOlder = 0;

    // A finite play: the passes left (0 loops forever)
    u32 loopsLeft = 0;
    bool loopForever = false;

    Envelope envelope;
    bool releasing = false;
    // The source has nothing left to give: the level it stopped at is held
    // while the envelope's release fades it (a cut to silence the same frame
    // ends the waveform mid-cycle, which is heard as a click)
    bool sourceOver = false;
    s32 holdSample = 0;
    // The frames the held sample takes to fade away on its own: the envelope's
    // release carries the rest of it
    s32 holdFade = 0;

    // A key-on while the voice was still sounding (the module steals voices):
    // the frames of fade the new sound comes in over the old one's level, and
    // what that level was. The voice's own contribution of the last frame,
    // which the next key-on starts its fade from
    s32 guardFrames = 0;
    s32 guardLeft = 0;
    s32 guardRight = 0;
    s32 lastLeft = 0;
    s32 lastRight = 0;

    // Volume: the game's register, its group, and the mixed value ramping to
    // the scaled target (16-bit signed; past MaxVolume is inverted phase)
    s16 rawLeft = 0;
    s16 rawRight = 0;
    s32 group = 0;
    s32 left = 0;
    s32 right = 0;
    s32 targetLeft = 0;
    s32 targetRight = 0;
    bool reverbSend = false;
};

// A core's reverb: its mode (0 off, the SPU2's 10), settings and volumes. The mixer runs the SPU family's Schroeder
// network at 22050 Hz (same/different-side IIR reflections, a 4-tap comb early echo, two all-pass stages) over a
// work area sized like retail's, with register sets from the disc's LIBSD.IRX. Voices with reverb send feed it
// post-volume (EON routing), and its output goes through EVOLL/R into the mix. Depth scales the input drive, delay
// the line lengths, feedback the wall reflection.
struct Reverb
{
    s32 mode = 0;
    u16 depthLeft = 0;
    u16 depthRight = 0;
    u16 delay = 0;
    u16 feedback = 0;
    s16 volumeLeft = 0;
    s16 volumeRight = 0;
};

void LockMixer();
void UnlockMixer();
// Mixes frames stereo s16 samples of 48 kHz into out. The caller holds no
// lock; the mixer takes it
void MixStereo(s16* out, u32 frames);
// The sound processor's memory, where the sound banks' samples go (a
// test/debug view; the stream layer loads through LoadSoundBankSamples)
u8* SpuMemory();
// A sound bank's samples arrived from the disc (the stream layer's
// ReadSoundBank): copies size bytes into the sound processor's memory,
// finds the sound's loop points in its ADPCM blocks and registers it under
// its bank's ID (the resource header's). Returns where it landed, 0 when
// there was no room
u32 LoadSoundBankSamples(u32 sound, const u8* data, u32 size);

// A movie's PCM (the PSS's 48 kHz stereo SShd sound once de-interleaved):
// handed over whole at Open, looped from StartSound, stopped at Close. The
// host's shape of retail's sound ring (player.cpp's 0x6000 looped block
// transfer): the whole movie is demuxed up front like its pictures, so the
// ring holds the whole sound instead of a window of it.
void MoviePcmOpen(u32 frames, const s16* stereo);
void MoviePcmStart(f32 volume);
void MoviePcmStop();
// Aliases matching the player's Start/Stop naming (Main's callers): same as MoviePcmStart/Stop.
void StartMoviePcm(f32 volume);
void StopMoviePcm();
// FNV-1a over one second of the mode's wet output answering a full-scale
// impulse with the settings (depths, delay, feedback, full output volumes):
// the reverb network's checkable answer (tools/audio_proof.cpp prints it).
u32 ReverbImpulseChecksum(s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback);
}
