// The host's side of the sound: the module behind Platform::Audio, the module's mix handed to SDL, and the movie's PCM mixed
// on top of it.
#include "audio.h"

#include "soundmodule.h"

#include "debug.h"
#include "platform/audio.h"
#include "platform/sound.h"
#include "../settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
using namespace SdlAudio::Spu2;

constexpr s32 MixRate = 48000;
constexpr s32 MixChannels = 2;
constexpr s32 MixFrames = 1024;

SDL_AudioStream* g_AudioStream = nullptr;
SDL_Mutex* g_AudioLock = nullptr;

// A dump of the mixed output (a debugging aid): TS_AUDIODUMP=<file> writes the stream's raw s16 stereo at 48 kHz
std::FILE* g_AudioDump = nullptr;
bool g_AudioDumpRead = false;

std::FILE* AudioDump()
{
    if (!g_AudioDumpRead)
    {
        g_AudioDumpRead = true;
        const char* path = std::getenv("TS_AUDIODUMP");
        if (path != nullptr && path[0] != '\0')
        {
            g_AudioDump = std::fopen(path, "wb");
            if (g_AudioDump != nullptr)
            {
                std::setvbuf(g_AudioDump, nullptr, _IONBF, 0);
            }
        }
    }

    return g_AudioDump;
}

SDL_Mutex* AudioLock()
{
    if (g_AudioLock == nullptr)
    {
        g_AudioLock = SDL_CreateMutex();
    }

    return g_AudioLock;
}

} // namespace

void SdlAudio::LockAudio()
{
    SDL_LockMutex(AudioLock());
}

void SdlAudio::UnlockAudio()
{
    SDL_UnlockMutex(AudioLock());
}

namespace
{
using SdlAudio::LockAudio;
using SdlAudio::UnlockAudio;


// The movie's sound: its frames, how far through them the device is, and the volume being ramped to (a movie stopped with its
// sound still sounding would otherwise be cut mid-waveform)
constexpr s32 MovieVolumeShift = 12;

struct MoviePcm
{
    bool playing = false;
    std::vector<s16> frames;
    u32 position = 0;
    s32 volume = 0;
    s32 targetVolume = 0;
};

MoviePcm g_MoviePcm;

// The movie's frames added to the module's, both held to the range as the DAC does
void MixMoviePcm(s16* stereo, u32 frames)
{
    if (!g_MoviePcm.playing || g_MoviePcm.frames.empty())
    {
        return;
    }

    for (u32 frame = 0; frame < frames; frame++)
    {
        if (g_MoviePcm.volume != g_MoviePcm.targetVolume)
        {
            s32 step = (g_MoviePcm.targetVolume - g_MoviePcm.volume) / 64;
            g_MoviePcm.volume += step != 0 ? step : (g_MoviePcm.volume < g_MoviePcm.targetVolume ? 1 : -1);
        }

        u32 at = g_MoviePcm.position * 2;
        if (at + 1 >= g_MoviePcm.frames.size())
        {
            // Its sound is over: the movie's sound runs its length and does not start again
            g_MoviePcm.targetVolume = 0;
            return;
        }

        s32 left = stereo[frame * 2] + g_MoviePcm.frames[at] * g_MoviePcm.volume / (1 << MovieVolumeShift);
        s32 right = stereo[frame * 2 + 1] + g_MoviePcm.frames[at + 1] * g_MoviePcm.volume / (1 << MovieVolumeShift);
        stereo[frame * 2] = static_cast<s16>(Saturate(left));
        stereo[frame * 2 + 1] = static_cast<s16>(Saturate(right));
        g_MoviePcm.position++;
    }
}

void SDLCALL FeedAudio(void* userdata, SDL_AudioStream* stream, int additional, int total)
{
    (void)userdata;
    (void)total;
    if (additional <= 0)
    {
        return;
    }

    s32 frames = additional / (MixChannels * static_cast<s32>(sizeof(s16)));
    if (frames <= 0)
    {
        return;
    }

    // SDL asking for a lot at once means the device already ran dry (its buffer drained while this thread was held up)
    if (frames > MixFrames * 2 && std::getenv("TS_AUDIOLOG") != nullptr)
    {
        std::fprintf(stderr, "audio: stream asked for %d frames at once (the device ran dry)\n", frames);
    }

    // The options the player set are written by the game thread's own tick (HostSettingsTick, Platform::Graphics::
    // BeginLogicFrame), not here: this callback must not read the game's state while the game thread writes it, nor
    // touch the filesystem while the device waits on it

    // SDL asks for what the stream needs right now: filling less than asked leaves it short and the device underruns (the
    // audible pop). The request is served whole, one MixFrames chunk at a time, rather than truncated at a cap
    static s16 mixed[MixFrames * 2];
    std::FILE* dump = AudioDump();
    while (frames > 0)
    {
        s32 chunk = frames > MixFrames ? MixFrames : frames;
        LockAudio();
        Module().Mix(mixed, static_cast<u32>(chunk));
        MixMoviePcm(mixed, static_cast<u32>(chunk));
        UnlockAudio();

        SDL_PutAudioStreamData(stream, mixed, chunk * MixChannels * static_cast<s32>(sizeof(s16)));
        if (dump != nullptr)
        {
            std::fwrite(mixed, sizeof(s16) * MixChannels, static_cast<size_t>(chunk), dump);
        }

        frames -= chunk;
    }
}

bool StartAudio()
{
    if (g_AudioStream != nullptr)
    {
        return true;
    }

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = MixChannels;
    spec.freq = MixRate;
    g_AudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedAudio, nullptr);
    if (g_AudioStream == nullptr)
    {
        std::fprintf(stderr, "sdl3: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(g_AudioStream))
    {
        std::fprintf(stderr, "sdl3: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_AudioStream);
        g_AudioStream = nullptr;
        return false;
    }

    return true;
}
}

void SdlAudio::MoviePcmOpen(u32 frames, const s16* stereo)
{
    LockAudio();
    g_MoviePcm = MoviePcm();
    if (frames != 0 && stereo != nullptr)
    {
        g_MoviePcm.frames.assign(stereo, stereo + static_cast<size_t>(frames) * 2);
    }

    UnlockAudio();
}

void SdlAudio::MoviePcmStart(f32 volume)
{
    LockAudio();
    g_MoviePcm.playing = !g_MoviePcm.frames.empty();
    g_MoviePcm.position = 0;
    s32 scaled = static_cast<s32>(volume * (1 << MovieVolumeShift));
    g_MoviePcm.volume = scaled < 0 ? 0 : scaled;
    g_MoviePcm.targetVolume = g_MoviePcm.volume;
    UnlockAudio();
}

void SdlAudio::MoviePcmStop()
{
    // Let go of at once (the game has the screen back) and wipe the state so a silent movie after one with PCM does not replay
    // the previous soundtrack (the retail audio init zeroes the IOP ring size and flags for any mode other than 48 kHz stereo)
    LockAudio();
    g_MoviePcm.playing = false;
    g_MoviePcm.frames.clear();
    g_MoviePcm.frames.shrink_to_fit();
    g_MoviePcm.position = 0;
    g_MoviePcm.volume = g_MoviePcm.targetVolume = 0;
    UnlockAudio();
}

s32 Platform::Sound::InitialiseRemote()
{
    // The game sound code's own log (what every play was handed and every check that turned one down), if a run asks for it.
    // TS_AUDIOLOG turns it on too: a sound that went missing is either dropped by the game before the module ever sees it (no
    // voice free, the sound not loaded) or turned down by the module, and one log that has both sides says which on the line
    if (std::getenv("TS_SOUNDLOG") != nullptr || std::getenv("TS_AUDIOLOG") != nullptr)
    {
        g_SoundLog = true;
    }

    return StartAudio() ? 0 : -1;
}

// ---- Platform::Audio, over the module ----

void Platform::Audio::Reset()
{
    // The sound processor starting fresh is where the game puts its defaults: the host's settings file, if there is one, is put
    // in first (it calls SetGroupVolume)
    HostSettingsApplyOnce();
    LockAudio();
    Module().Reset();
    UnlockAudio();
}

void Platform::Audio::PauseAll()
{
    LockAudio();
    Module().PauseAll();
    UnlockAudio();
}

void Platform::Audio::ResumeAll()
{
    LockAudio();
    Module().ResumeAll();
    UnlockAudio();
}

s32 Platform::Audio::LendToMovie()
{
    // The module's key-ons are taken away for the movie. Nothing it holds is let go of: the game's music players are still in
    // their playing state when the module comes back, and the game never re-starts a track it believes is playing
    LockAudio();
    Module().LendToMovie();
    UnlockAudio();
    return 0;
}

s32 Platform::Audio::ReclaimFromMovie()
{
    LockAudio();
    Module().ReclaimFromMovie();
    UnlockAudio();
    return 0;
}

void Platform::Audio::CompactSoundMemory()
{
    LockAudio();
    Module().CompactSoundMemory();
    UnlockAudio();
}

void Platform::Audio::SetGroupVolume(s32 group, u32 left, u32 right)
{
    LockAudio();
    Module().SetGroupVolume(group, left, right);
    UnlockAudio();
}

s32 Platform::Audio::SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    LockAudio();
    s32 result = Module().SetVoiceVolume(voice, left, right);
    UnlockAudio();
    return result;
}

s32 Platform::Audio::SetVoicePitch(s32 voice, u16 pitch)
{
    LockAudio();
    s32 result = Module().SetVoicePitch(voice, pitch);
    UnlockAudio();
    return result;
}

void Platform::Audio::SetVoiceReverb(s32 voice, bool on)
{
    LockAudio();
    Module().SetVoiceReverb(voice, on);
    UnlockAudio();
}

void Platform::Audio::ReleaseVoice(s32 voice, u32 rate)
{
    LockAudio();
    Module().ReleaseVoice(voice, rate);
    UnlockAudio();
}

s32 Platform::Audio::IsVoiceFree(s32 voice)
{
    LockAudio();
    s32 result = Module().IsVoiceFree(voice);
    UnlockAudio();
    return result;
}

void Platform::Audio::SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    LockAudio();
    Module().SetReverb(core, mode, depthLeft, depthRight, delay, feedback);
    UnlockAudio();
}

void Platform::Audio::ClearReverb(s32 core)
{
    LockAudio();
    Module().ClearReverb(core);
    UnlockAudio();
}

void Platform::Audio::SetReverbVolume(s32 core, s16 left, s16 right)
{
    LockAudio();
    Module().SetReverbVolume(core, left, right);
    UnlockAudio();
}

u32 Platform::Audio::ReserveSoundMemory(u32 size)
{
    LockAudio();
    u32 address = Module().ReserveSoundMemory(size);
    UnlockAudio();
    return address;
}

void Platform::Audio::SoundBankLoaded(u16 bank)
{
    LockAudio();
    Module().SoundBankLoaded(bank);
    UnlockAudio();
}

void Platform::Audio::ReserveSounds(u16 count)
{
    LockAudio();
    Module().ReserveSounds(count);
    UnlockAudio();
}

void Platform::Audio::ReleaseSound(u32 sound)
{
    LockAudio();
    Module().ReleaseSound(sound);
    UnlockAudio();
}

s32 Platform::Audio::PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release,
                               u32 loops)
{
    LockAudio();
    // Where in the game this play came from: a sound nobody can account for is traced from the log to the call that made it
    Module().SetSoundCaller(__builtin_return_address(0));
    s32 result = Module().PlaySound(sound, voiceAndGroup, left, right, pitch, attack, release, loops);
    UnlockAudio();
    return result;
}

s32 Platform::Audio::PitchOfRate(s32 rate)
{
    return Module().PitchOfRate(rate);
}

u32 Platform::Audio::ChannelBufferAddress(s32 channel)
{
    LockAudio();
    u32 address = Module().ChannelBufferAddress(channel);
    UnlockAudio();
    return address;
}

void Platform::Audio::ReadMusic(s32 file, u32 offset, u32 size)
{
    LockAudio();
    Module().ReadMusic(file, offset, size);
    UnlockAudio();
}

s32 Platform::Audio::StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once)
{
    // The track's region is read here, with the device lock not held: a disc read under it parks the mixer for the read's
    // length, and the device underruns while it waits. The stream's start then only installs bytes it was handed
    std::vector<u8> prepared;
    Module().PrepareStream(file, &prepared);
    LockAudio();
    s32 result = Module().StreamMusic(file, channel, voiceAndGroup, pitch, once, &prepared);
    UnlockAudio();
    return result;
}

void Platform::Audio::InterleaveMusic(s32 channel, u32 blockSize)
{
    LockAudio();
    Module().InterleaveMusic(channel, blockSize);
    UnlockAudio();
}

void Platform::Audio::AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    LockAudio();
    Module().AddMusicChannel(child, parent, voiceAndGroup, soundAddress);
    UnlockAudio();
}

void Platform::Audio::SetMusicEnd(s32 channel, u32 end)
{
    LockAudio();
    Module().SetMusicEnd(channel, end);
    UnlockAudio();
}

void Platform::Audio::PrepareMusic(s32 channel)
{
    LockAudio();
    Module().PrepareMusic(channel);
    UnlockAudio();
}

bool Platform::Audio::IsMusicReady(s32 channel)
{
    LockAudio();
    bool ready = Module().IsMusicReady(channel);
    UnlockAudio();
    return ready;
}

s32 Platform::Audio::PlayMusic(s32 channel)
{
    LockAudio();
    s32 result = Module().PlayMusic(channel);
    UnlockAudio();
    return result;
}

bool Platform::Audio::IsMusicPlaying(s32 channel)
{
    LockAudio();
    bool playing = Module().IsMusicPlaying(channel);
    UnlockAudio();
    return playing;
}

void Platform::Audio::StopMusic(s32 channel)
{
    LockAudio();
    Module().StopMusic(channel);
    UnlockAudio();
}
