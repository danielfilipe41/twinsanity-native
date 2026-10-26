// Sound processor: the SPU mixer (spu_mixer.cpp) behind
// Platform::Audio, fed to a 48 kHz stereo stream.
#include "spu_mixer.h"

#include "../graphics/host_graphics.h"
#include "debug.h"
#include "platform/sound.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace
{
constexpr s32 MixRate = 48000;
constexpr s32 MixChannels = 2;
constexpr s32 MixFrames = 1024;

SDL_AudioStream* g_AudioStream = nullptr;

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

    // SDL asking for a lot at once means the device already ran dry (its
    // buffer drained while this thread was held up): worth saying when a run
    // asks for the sound log, since that is the moment everything stops
    if (frames > MixFrames * 2 && std::getenv("TS_AUDIOLOG") != nullptr)
    {
        std::fprintf(stderr, "audio: stream asked for %d frames at once (the device ran dry)\n", frames);
    }

    // SDL asks for what the stream needs right now: filling less than asked leaves it short and the device underruns
    // (the audible pop). The request is served whole, one MixFrames chunk at a time, rather than truncated at a cap
    static s16 mixed[MixFrames * 2];
    std::FILE* dump = AudioDump();
    while (frames > 0)
    {
        s32 chunk = frames > MixFrames ? MixFrames : frames;
        SdlAudio::MixStereo(mixed, static_cast<u32>(chunk));
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
    g_AudioStream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedAudio, nullptr);
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

s32 Platform::Sound::InitialiseRemote()
{
    // The game sound code's own log (what every play was handed and every check that turned one down), if a run asks for it
    if (std::getenv("TS_SOUNDLOG") != nullptr)
    {
        g_SoundLog = true;
    }

    // TS_NO=audio: up without sound while a crash is traced elsewhere
    if (Platform::Graphics::HostStageDisabled("audio"))
    {
        std::fprintf(stderr, "audio: disabled via TS_NO=audio\n");
        return 0;
    }

    return StartAudio() ? 0 : -1;
}
