#include "game/sound.h"

#include "game/chunkdata.h"
#include "game/clock.h"
#include "game/controllers.h"
#include "game/filestream.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/language.h"
#include "game/layout.h"
#include "game/memory.h"
#include "game/movie.h"
#include "game/objects.h"
#include "game/place.h"
#include "game/reference.h"
#include "game/resources.h"
#include "game/stream.h"
#include "platform/audio.h"
#include "platform/time.h"
#include "retail/libc.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

// A scene being skipped runs unheard, and muting what already sounds is only half of it: the scene's scripts go on starting sounds as
// it runs, and one started after a mute was heard. So nothing is played at all while this is on (PlaySoundOnVoice and PlaySoundAt
// refuse), and what was already sounding is released once. The music is not played through those two (its players own their voices),
// so it is untouched
bool g_SceneSilenced = false;

#ifndef __mips__
// TEMPORARY probe for the scene skip's silence (TWINSANITY_TEST_SKIP_AUDIO=1): every start the silence refuses, and every start just
// after it lifts, with the sound it was, which names the caller that got through
// 1 logs only what the skip's silence touches, 2 logs every sound the game starts (the comparison: is a sound repeated this often
// in plain play too)
s32 SkipProbeMode()
{
    static s32 mode = -1;
    if (mode < 0)
    {
        const char* value = std::getenv("TWINSANITY_TEST_SKIP_AUDIO");
        mode = value == nullptr ? 0 : (value[0] == '2' ? 2 : 1);
    }

    return mode;
}

bool SkipProbeOn()
{
    return SkipProbeMode() != 0;
}

s32 g_SkipProbeAfter = 0;

u32 g_SkipProbeCallerLines = 0;

// Mode 2, in the stretch that matters: while a skip holds and just after it lifts
bool SkipProbeWindow()
{
    return SkipProbeMode() == 2 && (g_SceneSilenced || g_SkipProbeAfter > 0);
}

u32 SkipProbeCallerLines()
{
    return g_SkipProbeCallerLines;
}

void SkipProbeCallerLine(const char* where, u32 id, u32 silenced)
{
    g_SkipProbeCallerLines++;
    std::fprintf(stderr, "skip-audio: %s id 0x%x from 0x%lx silenced %u after %d\n", where, id,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(__builtin_return_address(1))), silenced, g_SkipProbeAfter);
}

// Mode 2 counts what is asked for and where from, and prints a summary a second at a time: a sound asked for many times a second is
// a caller that never settles, and the address says which caller
struct SkipProbeEntry
{
    u32 id;
    u32 count;
    uintptr_t caller;
};

SkipProbeEntry g_SkipProbeIds[32];
u32 g_SkipProbeIdCount = 0;
u32 g_SkipProbeCalls = 0;

void SkipProbeFlush()
{
    for (u32 index = 0; index < g_SkipProbeIdCount; index++)
    {
        if (g_SkipProbeIds[index].count != 0)
        {
            std::fprintf(stderr, "skip-audio: id 0x%x asked %u times, caller 0x%lx, silenced %d after %d\n", g_SkipProbeIds[index].id,
                         g_SkipProbeIds[index].count, static_cast<unsigned long>(g_SkipProbeIds[index].caller),
                         g_SceneSilenced != 0 ? 1 : 0, g_SkipProbeAfter);
            g_SkipProbeIds[index].count = 0;
        }
    }
}

void SkipProbe(const char* what, u32 id, s32 group, s32 voice)
{
    if (!SkipProbeOn())
    {
        return;
    }

    static u32 logged = 0;
    if (SkipProbeMode() == 1 && g_SceneSilenced == 0 && g_SkipProbeAfter == 0)
    {
        return;
    }

    if (SkipProbeMode() == 2)
    {
        uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        for (u32 index = 0; index < g_SkipProbeIdCount; index++)
        {
            if (g_SkipProbeIds[index].id == id)
            {
                g_SkipProbeIds[index].count++;
                return;
            }
        }

        if (g_SkipProbeIdCount < 32)
        {
            g_SkipProbeIds[g_SkipProbeIdCount].id = id;
            g_SkipProbeIds[g_SkipProbeIdCount].count = 1;
            g_SkipProbeIds[g_SkipProbeIdCount].caller = caller;
            g_SkipProbeIdCount++;
        }

        return;
    }

    if (++logged > 200)
    {
        return;
    }

    std::fprintf(stderr, "skip-audio: %s id 0x%x group %d voice %d silenced %d after %d\n", what, id, group, voice,
                 g_SceneSilenced != 0 ? 1 : 0, g_SkipProbeAfter);
}
#endif


// The sound code's side of the sound processor: voices, reverbs, groups and music
namespace
{
// What volumes as fractions are scaled by for the sound processor: a voice's full volume, a reverb's and a group's volume of 1
constexpr f32 VoiceVolumeScale = Platform::Audio::MaxVolume;
constexpr f32 ReverbVolumeScale = 32767.0f;
constexpr f32 GroupVolumeScale = Platform::Audio::FullGroupVolume;
// A reverb's delay and feedback go to the sound processor in 7 bits
constexpr f32 ReverbSettingScale = 127.0f;
// A silenced voice is let go of at this release rate once it has played more than 10 frames
constexpr u32 ReleaseRate = 0xD;
constexpr s32 ReleaseFrames = 10;
// A sound's parameter goes out with this value in its high byte
constexpr u32 SoundHighByte = 0xD;
// The sounds the sound processor's table keeps
constexpr u16 ReservedSounds = 0x100;
// A music player's buffers in the sound processor's memory: a half for each of its streams
constexpr u32 MusicBufferSize = 0x8000;
constexpr u32 MusicStreamBuffer = 0x4000;
constexpr s32 KeepAliveFrames = 36;

// A negative volume plays in the other phase
s32 Phase(s32 volume)
{
    return volume < 0 ? volume + Platform::Audio::InvertedVolumeBase : volume;
}

s32 VoiceVolume(f32 volume)
{
    return Phase(static_cast<s32>(volume * VoiceVolumeScale));
}

u32 GroupOf(const MusicPlayer* player)
{
    return SoundGroupOf(static_cast<s32>(player->bits.group));
}

u32 VoiceCount(const MusicPlayer* player)
{
    return player->bits.interleaved != 0 ? 2 : 1;
}

// The size of the blocks an interleaved track's sides alternate in (the music bank header's second word)
u32 MusicBlockSize(const SoundBankFiles* bank)
{
    return reinterpret_cast<const u32*>(bank->header)[1];
}

// The SPU2's reverb mode of a chunk's reverb type
s32 ReverbModeOf(u8 type)
{
    switch (type)
    {
    case 0:
        return ReverbPipe;
    case 1:
        return ReverbRoom;
    case 2:
        return ReverbStudioA;
    case 3:
        return ReverbStudioB;
    case 4:
        return ReverbStudioC;
    case 5:
        return ReverbHall;
    case 6:
        return ReverbSpace;
    default:
        return ReverbOff;
    }
}

f32* GroupScales();

}

extern "C" u32 PlaySound(f32 volume, f32 pitchScale, GameSound* sound, s32 group, s32 voiceKind, s32 last)
{
#ifndef __mips__
    // TEMPORARY probe (TWINSANITY_TEST_SKIP_AUDIO=2): the ids no level holds, with what the game thinks it has for them
    if (sound->soundId >= 0x8000 && SkipProbeOn())
    {
        static u32 told = 0;
        if (told < 8)
        {
            told++;
            std::fprintf(stderr, "skip-audio: high id 0x%x header 0x%x size %d offset %d loaded %d\n", sound->soundId,
                         sound->resourceBits, sound->size, sound->offset, static_cast<u32>(sound->flags.loaded));
        }
    }
#endif
    if (sound->flags.loaded == 0)
    {
        return 0;
    }

    SoundVoice* voice = FindFreeVoice(voiceKind, 0);
    if (voice == nullptr)
    {
        return 0;
    }

    if (PlaySoundOnVoice(volume, pitchScale, voice, sound, SoundGroupOf(group), last) == -1)
    {
        ReleaseVoice(voice);
        return 0;
    }

    SetVoiceReverb(voice, voiceKind);
    return 1;
}

extern "C" void FadeSoundGroups(f32 time)
{
    g_GroupFadeTime = time;
    g_GroupFade = 1.0f;
}

extern "C" void SetMusicBank(const char* name)
{
    LoadSoundBank(&g_MusicBank, name);
    for (MusicPlayer& player : g_Music->players)
    {
        CreateMusicBuffers(&player, &g_MusicBank);
    }
}

extern "C" void SetVoiceBank(const char* name)
{
    LoadSoundBank(&g_VoiceBank, name);
}

extern "C" u32 PlaySoundById(f32 volume, f32 pitchScale, u32 id, s32 group, s32 voiceKind, s32 last)
{
#ifndef __mips__
    // TEMPORARY probe (TWINSANITY_TEST_SKIP_AUDIO=2): who asks for this sound, by return address, for the first calls after a skip
    if (SkipProbeMode() == 2 && (g_SceneSilenced != 0 || g_SkipProbeAfter > 0) && SkipProbeCallerLines() < 80)
    {
        SkipProbeCallerLine("PlaySoundById", id, g_SceneSilenced != 0 ? 1 : 0);
    }
#endif
    GameSound* sound = SoundById(static_cast<u16>(id));
#ifndef __mips__
    // TEMPORARY probe (TWINSANITY_TEST_SKIP_AUDIO=2): the frontend's own ids, and what the game thinks it has for them
    if (id >= 0x8000 && SkipProbeOn())
    {
        static u32 told = 0;
        if (told < 8)
        {
            told++;
            std::fprintf(stderr, "skip-audio: frontend id 0x%x found %d size %d offset %d loaded %d\n", id, sound != nullptr ? 1 : 0,
                         sound != nullptr ? sound->size : -1, sound != nullptr ? sound->offset : -1,
                         sound != nullptr ? static_cast<u32>(sound->flags.loaded) : 0);
        }
    }
#endif
    if (sound == nullptr)
    {
        return 0;
    }

    return PlaySound(volume, pitchScale, sound, group, voiceKind, last);
}

EABI_EXPORT(FUN_001e5d98, PlaySound);
EABI_EXPORT(FUN_001e5d10, PlaySoundById);
EABI_EXPORT(FUN_001e0fd0, FadeMusic);
EABI_EXPORT(FUN_001e6c20, FadeMusicOut);
EABI_EXPORT(FUN_001e6e30, FadeMusicTo);
EABI_EXPORT(FUN_001e60b0, SetReverbVolume);
EABI_EXPORT(FUN_001e6108, SetReverbDepth);
EABI_EXPORT(FUN_001e64f0, SetVoiceVolume);
EABI_EXPORT(FUN_001dfa38, PlaySoundOnVoice);
EABI_EXPORT(FUN_001dfbc0, PlaySoundAt);
EABI_EXPORT(FUN_001e6778, SetGroupVolume);
EABI_EXPORT(FUN_001e6fc0, PlayMusic);
EABI_EXPORT(FUN_001e6e78, SetMusicPitch);
EABI_EXPORT(FUN_001e14b8, SetMusicVolume);
EABI_EXPORT(FUN_001e1730, UpdateMusic);


namespace
{

f32* GroupScales()
{
    return reinterpret_cast<f32*>(g_AlphaPresetsBlock + GroupScalesOffset);
}

void ApplyGroupVolumes()
{
    for (u32 group = 0; group < VolumeGroupCount; group++)
    {
        Platform::Audio::SetGroupVolume(static_cast<s32>(SoundGroupOf(static_cast<s32>(group))),
                                        static_cast<u16>(g_GroupVolumes[group].left),
                                        static_cast<u16>(g_GroupVolumes[group].right));
    }
}

void PlayOnVoice(SoundVoice* voice, GameSound* sound, u32 group, s32 last)
{
    Platform::Audio::PlaySound(sound->soundId, static_cast<u32>(VoiceNumber(voice)) | group, static_cast<s16>(voice->left),
                               static_cast<s16>(voice->right), static_cast<u16>(voice->pitch), sound->parameter1,
                               SoundHighByte, static_cast<u32>(last == -1 ? 1 : last));
    voice->frames = 0;
    // A voice silenced to be let go of and taken again plays: the release that was pending was the silence's, not the new sound's,
    // and leaving it set cut every sound a muting had touched shortly after it started (its owner then started it again, and again)
    voice->bits.released = 0;
}
}

extern "C"
{
    s32 VoiceNumber(const SoundVoice* voice)
    {
        return static_cast<s32>(voice->bits.secondCore * SoundCore::VoiceCount + voice->bits.number);
    }

    u32 SoundGroupOf(s32 group)
    {
        // Handed to the sound processor with the voice's number in the low half
        constexpr u32 GroupNumberShift = 16;
        switch (group)
        {
        case SecondEffectsGroup:
            return 2u << GroupNumberShift;
        case MusicGroup:
            return 3u << GroupNumberShift;
        case MovieGroup:
            return 4u << GroupNumberShift;
        default:
            return 1u << GroupNumberShift;
        }
    }

    void SetReverbVolume(f32 left, f32 right, SoundCore* core)
    {
        s32 rightVolume = static_cast<s32>(right * ReverbVolumeScale);
        s32 leftVolume = static_cast<s32>(left * ReverbVolumeScale);
        core->reverbVolumeRight = rightVolume;
        core->reverbVolumeLeft = leftVolume;
        Platform::Audio::SetReverbVolume(static_cast<u16>(core->bits.number), static_cast<s16>(leftVolume),
                                         static_cast<s16>(rightVolume));
    }

    void SetReverbDepth(f32 left, f32 right, SoundCore* core)
    {
        core->reverbDepthRight = static_cast<s32>(right * ReverbVolumeScale);
        core->reverbDepthLeft = static_cast<s32>(left * ReverbVolumeScale);
    }

    void SetReverb(SoundCore* core, s32 mode, u32 delay, u32 feedback)
    {
        core->reverbMode = mode;
        u16 index = static_cast<u16>(core->bits.number);
        if (mode == ReverbOff)
        {
            Platform::Audio::ClearReverb(index);
            return;
        }

        Platform::Audio::SetReverb(index, mode, static_cast<u16>(core->reverbDepthLeft), static_cast<u16>(core->reverbDepthRight),
                                   static_cast<u16>(delay), static_cast<u16>(feedback));
        core->reverbIdleFrames = 0;
        core->bits.reverbDelay = delay;
        core->bits.reverbFeedback = feedback;
    }

    void SetReverbFromSettings(SoundCore* core, const u8* settings)
    {
        const auto* reverb = reinterpret_cast<const ReverbSettings*>(settings);
        s32 mode = ReverbModeOf(static_cast<u8>(reverb->bits.type));
        if (mode == ReverbOff)
        {
            return;
        }

        s32 feedback = static_cast<s32>(reverb->feedback * ReverbSettingScale);
        s32 depth = static_cast<s32>(reverb->depth * ReverbVolumeScale);
        s32 delay = static_cast<s32>(reverb->delay * ReverbSettingScale);
        core->reverbMode = mode;
        core->reverbDepthLeft = depth;
        core->reverbDepthRight = depth;
        Platform::Audio::SetReverb(static_cast<u16>(core->bits.number), mode, static_cast<u16>(depth), static_cast<u16>(depth),
                                   static_cast<u16>(delay), static_cast<u16>(feedback));
        core->reverbIdleFrames = 0;
        core->bits.reverbDelay = static_cast<u32>(delay);
        core->bits.reverbFeedback = static_cast<u32>(feedback);
    }

    void ClearReverbs()
    {
        Platform::Audio::ClearReverb(0);
        Platform::Audio::ClearReverb(1);
    }

    void RestoreReverbs()
    {
        for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
        {
            if (core->reverbMode != ReverbOff)
            {
                Platform::Audio::SetReverb(static_cast<u16>(core->bits.number), core->reverbMode,
                                           static_cast<u16>(core->reverbDepthLeft), static_cast<u16>(core->reverbDepthRight),
                                           static_cast<u16>(core->bits.reverbDelay), static_cast<u16>(core->bits.reverbFeedback));
            }
        }
    }

    u32 UpdateVoice(SoundVoice* voice)
    {
        SoundVoiceBits bits = voice->bits;
        s32 frames = voice->frames + 1;
        voice->frames = frames;
        s32 number = static_cast<s32>(bits.secondCore * SoundCore::VoiceCount + bits.number);
        if (bits.released != 0 && frames > ReleaseFrames)
        {
            Platform::Audio::ReleaseVoice(number, ReleaseRate);
            voice->bits.released = 0;
        }

        if (Platform::Audio::IsVoiceFree(number) != 0)
        {
            voice->bits.playing = 0;
            voice->bits.use = SoundVoice::UseNone;
        }

        return voice->bits.playing;
    }

    s32 SetVoicePitch(SoundVoice* voice, s32 pitch)
    {
        f32 scaled = g_PitchScale * static_cast<f32>(pitch);
        voice->pitch = pitch;
        return Platform::Audio::SetVoicePitch(VoiceNumber(voice), static_cast<u16>(static_cast<s32>(scaled)));
    }

    s32 SetVoiceVolume(f32 left, f32 right, SoundVoice* voice)
    {
        s32 rightVolume = VoiceVolume(right);
        s32 leftVolume = VoiceVolume(left);
        voice->left = leftVolume;
        voice->right = rightVolume;
        return Platform::Audio::SetVoiceVolume(VoiceNumber(voice), static_cast<s16>(leftVolume), static_cast<s16>(rightVolume));
    }

    void MuteVoice(SoundVoice* voice)
    {
        voice->bits.takenByMusic = 0;
        SetVoiceVolume(0.0f, 0.0f, voice);
        voice->bits.released = 1;
    }

    void SetVoiceReverb(SoundVoice* voice, s32 on)
    {
        u32 send = on != 0 ? 1 : 0;
        if (voice->bits.reverbSend == send)
        {
            return;
        }

        voice->bits.reverbSend = send;
        Platform::Audio::SetVoiceReverb(VoiceNumber(voice), voice->bits.reverbSend != 0);
    }

    s32 PlaySoundOnVoice(f32 volume, f32 pitchScale, SoundVoice* voice, GameSound* sound, u32 group, s32 last)
    {
        // The scene skip's silence is the mixers', not this function's: refusing here leaves the callers that own a sound thinking
        // it never took (an instance sound is then asked for every step and never settles), so what a skipped scene starts is played
        // and mixed at nothing instead
#ifndef __mips__
        SkipProbe(g_SceneSilenced != 0 ? "started" : "after", sound->soundId, static_cast<s32>(group), VoiceNumber(voice));
#endif
        voice->volume = volume;
        voice->pitchScale = pitchScale;
        // Retail clears the voice's use field here (bits & ~0x3E00), so a sound already playing can be taken from
        // (StealableVoice, FindFreeVoice's steal): the sound processor runs out of voices otherwise
        voice->bits.use = SoundVoice::UseNone;
        f32 level = 0.0f < volume ? volume : 1.0f;
        s32 scaled = static_cast<s32>(level * VoiceVolumeScale);
        voice->right = scaled;
        voice->left = scaled;
        u32 basePitch = sound->basePitch;
        voice->pitch = static_cast<s32>(basePitch);
        if (0.0f < pitchScale)
        {
            voice->pitch = static_cast<s32>(static_cast<f32>(basePitch) * voice->pitchScale);
        }

        PlayOnVoice(voice, sound, group, last);
        voice->lastAngle = 0.0f;
        voice->bits.playing = 1;
        return VoiceNumber(voice);
    }

    s32 PlaySoundAt(f32 volume, f32 pitchScale, SoundVoice* voice, GameSound* sound, u32 group, const Vector4* position, s32 last)
    {
#ifndef __mips__
        SkipProbe(g_SceneSilenced != 0 ? "started-at" : "after-at", sound->soundId, static_cast<s32>(group), VoiceNumber(voice));
#endif
        voice->pitchScale = pitchScale;
        voice->volume = volume;
        voice->bits.use = SoundVoice::UseNone;
        voice->lastAngle = 0.0f;
        f32 level = 1.0f;
        if (LoudestLevel < volume)
        {
            level = LoudestLevel;
        }
        else if (voice->lastAngle < volume)
        {
            level = volume;
        }

        s32 pitch = sound->basePitch;
        if (0.0f < voice->pitchScale)
        {
            pitch = static_cast<s32>(static_cast<f32>(pitch) * voice->pitchScale);
        }

        HeardSound heard;
        u32 audible = SoundAtPlace(level, g_SoundRange, voice->lastAngle, pitch, position, reinterpret_cast<f32*>(&heard));
        if (audible != 0)
        {
            voice->lastAngle = heard.angle;
        }

        if ((audible & 0xFF) == 0)
        {
            return -1;
        }

        voice->pitch = static_cast<s32>(heard.pitch);
        voice->left = static_cast<s32>(heard.left * VoiceVolumeScale);
        voice->right = static_cast<s32>(heard.right * VoiceVolumeScale);
        voice->left = Phase(voice->left);
        voice->right = Phase(voice->right);
        PlayOnVoice(voice, sound, group, last);
        voice->bits.playing = 1;
        return VoiceNumber(voice);
    }

    void FreeSoundSamples(u32, u32 sound)
    {
        Platform::Audio::ReleaseSound(sound);
    }

    void PauseSound()
    {
        Platform::Audio::PauseAll();
    }

    void ResumeSound()
    {
        Platform::Audio::ResumeAll();
    }

    void ResetSound()
    {
        Platform::Audio::Reset();
        for (u32 group = 0; group < VolumeGroupCount; group++)
        {
            GroupScales()[group] = 1.0f;
            Platform::Audio::SetGroupVolume(static_cast<s32>(SoundGroupOf(static_cast<s32>(group))),
                                            static_cast<u16>(g_GroupVolumes[group].left),
                                            static_cast<u16>(g_GroupVolumes[group].right));
        }
    }

    void SetGroupVolume(f32 left, f32 right, s32 group)
    {
        f32 rightVolume = right * GroupVolumeScale;
        f32 leftVolume = left * GroupVolumeScale;
        f32 scale = GroupScales()[group];
        g_GroupVolumes[group].right = static_cast<s16>(static_cast<s32>(rightVolume));
        g_GroupVolumes[group].left = static_cast<s16>(static_cast<s32>(leftVolume));
        Platform::Audio::SetGroupVolume(static_cast<s32>(SoundGroupOf(group)), static_cast<u32>(static_cast<s32>(leftVolume * scale)),
                                        static_cast<u32>(static_cast<s32>(rightVolume * scale)));
    }

    void StopAllSound()
    {
        for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
        {
            u32 bit = 1;
            for (u32 voice = 0; voice < SoundCore::VoiceCount; voice++)
            {
                if ((core->voicesInUse & bit) != 0)
                {
                    MuteVoice(&core->voices[voice]);
                }

                bit <<= 1;
            }

            SetReverb(core, ReverbOff, 0, 0);
        }

        for (u32 slot = 0; slot < MusicSlotCount; slot++)
        {
            MusicPlayer* player = g_Music->prepared[slot];
            if (player == nullptr)
            {
                player = g_Music->playing[slot];
            }

            if (player == nullptr)
            {
                continue;
            }

            player->bits.state = MusicPlayer::Stopping;
            while (UpdateMusic(0.0f, player, false))
            {
            }

            g_Music->prepared[slot] = nullptr;
            g_Music->playing[slot] = nullptr;
        }

        ClearInstanceSounds(&g_InstanceSounds);
        StopMusicEmitters();
        g_GroupFadeTime = 0.0f;
        ApplyGroupVolumes();
        for (u32 group = 0; group < VolumeGroupCount; group++)
        {
            GroupScales()[group] = 1.0f;
            f32 level = GroupVolumeLevel(static_cast<s32>(group));
            SetGroupVolume(level, level, static_cast<s32>(group));
        }
    }

    // The scene skip's silence: the platform's mix is held (every voice but the music's at nothing), and that is all it does. The
    // game's own voices are left exactly as the game keeps them: silencing or releasing them here made the objects that own them see
    // their sound gone and start it again, every step, which is the sound that went on after a skip
    extern "C" void SilenceSceneVoices()
    {
#ifndef __mips__
        if (!g_SceneSilenced && SkipProbeOn())
        {
            std::fprintf(stderr, "skip-audio: -- hold on --\n");
        }
#endif
        g_SceneSilenced = true;
        const s32 loud[1] = {static_cast<s32>(SoundGroupOf(MusicGroup))};
        Platform::Audio::HoldSilent(1, loud, 1);
    }

    // What the scene is still saying: the voices the game has playing that a music player does not own
    extern "C" u32 SceneVoicesHeard()
    {
        u32 heard = 0;
        for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
        {
            u32 bit = 1;
            for (u32 voice = 0; voice < SoundCore::VoiceCount; voice++)
            {
                const SoundVoice& entry = core->voices[voice];
                if ((core->voicesInUse & bit) != 0 && entry.bits.takenByMusic == 0 && entry.bits.playing != 0 &&
                    entry.bits.released == 0)
                {
                    heard++;
                }

                bit <<= 1;
            }
        }

        return heard;
    }

    extern "C" void RestoreSceneVoices()
    {
#ifndef __mips__
        if (SkipProbeOn())
        {
            g_SkipProbeAfter = 120;
            std::fprintf(stderr, "skip-audio: -- hold off, %u effect voices still playing --\n", SceneVoicesHeard());
        }
#endif
        g_SceneSilenced = false;
        Platform::Audio::HoldSilent(0, nullptr, 0);
    }

    // Whether the game still has a sound's samples anywhere it can play them from: a level's tables, or the front end's own (which
    // SoundById does not look in, and whose two sounds were being let go of by every prune)
    bool SoundIsStillHeld(u32 id)
    {
        // The table is asked by index (SoundById's low 15 bits), and two IDs can share one index: what it answers with is this
        // sound only when it is the sound the processor knows the block by, or a sound that has left is kept in memory by whatever
        // took its place
        if (id <= 0xFFFF)
        {
            const GameSound* sound = SoundById(static_cast<u16>(id));
            if (sound != nullptr && sound->soundId == id)
            {
                return true;
            }
        }

        // The languages' voice tables: a voice's ID is above the game's table's mask, so SoundById cannot find it there (it stops at
        // the game's table's capacity), and a language's dialogue was being let go of by every prune while its own table still held
        // it -- every later play of it then refused for having no samples, which is a line heard twice in one game
        GameResources* resources = G_GameResourcesObjectPointer;
        if (resources != nullptr && resources->voices != nullptr)
        {
            for (u32 language = 0; language < resources->languageCount; language++)
            {
                ResourceTable* voices = resources->voices[language];
                if (voices == nullptr)
                {
                    continue;
                }

                auto* sound = static_cast<GameSound*>(voices->items[id & ResourceIndexMask]);
                if (sound != nullptr && sound->soundId == id)
                {
                    return true;
                }
            }
        }

        GameController* controller = G_GameController;
        if (controller == nullptr)
        {
            return false;
        }

        // The table's capacity is its word's low 14 bits
        constexpr u32 CapacityMask = 0x3FFF;
        SoundTable& front = controller->frontEndSounds;
        const s32 capacity = static_cast<s32>(front.bits & CapacityMask);
        for (s32 index = 0; index < capacity; index++)
        {
            GameSound* sound = front.sounds[index];
            if (sound != nullptr && sound->soundId == id)
            {
                return true;
            }
        }

        return false;
    }

    // The samples of the sounds the game no longer has are let go of: the module keeps a block for every sound it was handed until it
    // is told, and a level's banks are read again each time it is entered or skipped through, so what no table holds is a block nobody
    // will ask for again. Called after a skip (the moment the pile-up happens) and when the memory is filling
    extern "C" void ReleaseSoundsTheGameLost()
    {
        const u32 total = Platform::Audio::SoundMemoryTotal();
        if (total == 0)
        {
            return;
        }

        // Every loaded ID at once: the module's table has 0x10000 slots and LoadedSoundId scans it from the start for each index,
        // so asking for them one at a time cost a scan a sound -- and a lock a sound -- every keep-alive
        static std::vector<u32> ids;
        const u32 slots = Platform::Audio::LoadedSoundCount();
        ids.resize(slots != 0 ? slots : 1);
        const u32 loaded = slots != 0 ? Platform::Audio::LoadedSoundIds(ids.data(), slots) : 0;
        u32 letGo = 0;
        u32 named = 0;
        // How the sounds the game still names are named: a count above one is a reference nothing lets go of, which is a sound
        // whose block can never be released however long the level has been left
        u32 namedMost = 0;
        u32 namedTwice = 0;
        for (u32 index = 0; index < loaded; index++)
        {
            const u32 id = ids[index];
            if (SoundIsStillHeld(id))
            {
                named++;
                if (id <= 0xFFFF)
                {
                    GameSound* sound = SoundById(static_cast<u16>(id));
                    const u32 references = sound != nullptr ? HeaderOf(sound)->bits.references : 0;
                    if (references > namedMost)
                    {
                        namedMost = references;
                    }

                    if (references > 1)
                    {
                        namedTwice++;
                    }
                }

                continue;
            }

            Platform::Audio::ReleaseSound(id);
            letGo++;
        }

#ifndef __mips__
        // The memory's own account, for a run whose sound is filling up: how much of it is in use, how much of that is the loaded
        // sounds' own blocks (the rest is blocks the module lost its name for), and how many of them the game still names. Blocks the
        // game still names are the game's tables holding references they never let go of: the prune cannot help those, and they are
        // what a level's sounds leaving memory behind looks like. Every sixteenth pass, so a run's log is readable
        static const bool probing = std::getenv("TWINSANITY_TEST_AUDIO") != nullptr;
        static u32 calls = 0;
        const u32 used = Platform::Audio::SoundMemoryUsed();
        if (probing && used * 2 >= total && (calls++ % 16) == 0)
        {
            std::fprintf(stderr,
                         "audio: sound memory %u of %u words: %u in %u loaded sounds, %u let go this pass, %u still named "
                         "(%u of them by more than one reference, up to %u)\n",
                         used, total, Platform::Audio::LoadedSoundWords(), loaded, letGo, named, namedTwice, namedMost);
        }
#endif
    }

    // A table's sounds that nothing references any more, let go of: an entry a reader found already there is kept even with no
    // references (resources.h's kept bit), so a level that is left behind leaves its sounds holding the processor's memory with
    // nothing left to play them. A sound's samples go with its entry: its block is the processor's, and the sound itself is the table's
    void ReleaseUnreferencedIn(ResourceTable* table)
    {
        const u32 capacity = table->bits.capacity;
        for (u32 index = 0; index < capacity; index++)
        {
            auto* sound = static_cast<GameSound*>(table->items[index]);
            if (sound == nullptr || HeaderOf(sound)->bits.references != 0)
            {
                continue;
            }

            table->items[index] = nullptr;
            sound->Destroy(DestroyAndFree);
        }
    }

    // Called where a level is unloaded, before the next one's banks are read, so what is left with no references is the level just
    // left -- and a level the game is still in has its objects' references on everything its scripts can play
    extern "C" void ReleaseUnreferencedSounds()
    {
        GameResources* resources = G_GameResourcesObjectPointer;
        if (resources == nullptr || resources->sounds == nullptr)
        {
            return;
        }

        ReleaseUnreferencedIn(resources->sounds);
        // The played language's voice table is the same kind of leftover, and the bigger one: a level's dialogue is read into it and
        // the table keeps it after the level is gone (and a scene skipped through leaves the lines it never reached)
        if (resources->voices != nullptr)
        {
            for (u32 language = 0; language < resources->languageCount; language++)
            {
                if (resources->voices[language] != nullptr)
                {
                    ReleaseUnreferencedIn(resources->voices[language]);
                }
            }
        }
    }

    u32 UpdateSound(s32 paused, TimeClock* clock)
    {
        f32 time = static_cast<f32>(static_cast<s32>(clock->advance)) * g_SecondsPerClockUnit;
        if (g_SoundLentToMovie != 0)
        {
#ifndef __mips__
            static s32 told = 0;
            if (told == 0 && std::getenv("TWINSANITY_TEST_AUDIO") != nullptr)
            {
                told = 1;
                std::fprintf(stderr, "audio: sound lent, update held\n");
            }
#endif
            return g_SoundLentToMovie;
        }

        for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
        {
            UpdateSoundCore(core);
        }

        if (paused == 0)
        {
            UpdateInstanceSounds();
            UpdateMusicEmitters();
        }

#ifndef __mips__
        if (g_SkipProbeAfter > 0)
        {
            g_SkipProbeAfter--;
        }

        if (SkipProbeOn() && ++g_SkipProbeCalls >= 60)
        {
            g_SkipProbeCalls = 0;
            SkipProbeFlush();
        }
#endif
        UpdatePlayingSounds(time, paused != 0);
        if (0.0f < g_GroupFadeTime)
        {
            g_GroupFade = g_GroupFade - time / g_GroupFadeTime;
            if (g_GroupFade < 0.0f)
            {
                g_GroupFade = 0.0f;
            }

            for (u32 group = 0; group < VolumeGroupCount; group++)
            {
                f32 left = static_cast<f32>(static_cast<s32>(static_cast<u16>(g_GroupVolumes[group].left))) * g_GroupFade;
                f32 right = static_cast<f32>(static_cast<s32>(static_cast<u16>(g_GroupVolumes[group].right))) * g_GroupFade;
                Platform::Audio::SetGroupVolume(static_cast<s32>(SoundGroupOf(static_cast<s32>(group))),
                                                static_cast<u32>(static_cast<s32>(left)), static_cast<u32>(static_cast<s32>(right)));
            }
        }

        UpdateCutsceneVolumes(time);
        g_FramesSinceKeepAlive++;
        if (g_FramesSinceKeepAlive >= KeepAliveFrames)
        {
            g_FramesSinceKeepAlive = 0;
            ReleaseSoundsTheGameLost();
            Platform::Audio::CompactSoundMemory();
        }

        u32 result = 0;
        if (g_MusicRequestPending != 0)
        {
            result = static_cast<u32>(PlayMusicRequest(g_PendingMusicSlot, &g_PendingMusicRequest));
            g_MusicRequestPending = 0;
        }

        return result;
    }

    MusicSystem* ConstructMusic(MusicSystem* music)
    {
        for (u32 i = 0; i < MusicSystem::PlayerCount; i++)
        {
            ConstructMusicPlayer(&music->players[i]);
        }

        for (u32 group = 0; group < VolumeGroupCount; group++)
        {
            g_GroupVolumes[group].left = Platform::Audio::FullGroupVolume;
            g_GroupVolumes[group].right = Platform::Audio::FullGroupVolume;
        }

        ResetSound();
        Platform::Audio::ReserveSounds(ReservedSounds);
        for (u32 core = 0; core < SoundCoreCount; core++)
        {
            g_SoundCores[core].bits.number = core;
        }

        g_SoundRange = DefaultSoundRange;
        for (u32 i = 0; i < MusicSystem::PlayerCount; i++)
        {
            music->players[i].soundBuffer = Platform::Audio::ReserveSoundMemory(MusicBufferSize);
        }

        for (u32 slot = 0; slot < MusicSlotCount; slot++)
        {
            music->playing[slot] = nullptr;
            music->prepared[slot] = nullptr;
            g_MusicSlotsPending[slot] = 0;
        }

        return music;
    }

    void CreateMusicBuffers(MusicPlayer* player, SoundBankFiles* bank)
    {
        if (player->stream != nullptr)
        {
            return;
        }

        // Two of the music's blocks, double buffered
        u32 size = static_cast<u32>(static_cast<s32>(MusicBlockSize(bank)) * 2 * 2);
        if (size >= StreamFreeBufferMemory())
        {
            return;
        }

        player->stream = OpenFileStream(g_StreamSystem, size, player->soundBuffer, MusicStreamBuffer);
        player->secondStream = OpenFileStream(g_StreamSystem, 0, player->soundBuffer + MusicStreamBuffer, MusicStreamBuffer);
        player->iopBuffer = Platform::Audio::ChannelBufferAddress(player->stream->channel);
        player->bits.state = MusicPlayer::Stopped;
    }

    void StartMusic(MusicPlayer* player, SoundBankFiles* bank, const u32* request)
    {
        const auto* music = reinterpret_cast<const MusicRequest*>(request);
        u32 number = music->bits.track;
        player->track = number;
        MusicTrack* entry = FindMusicTrack(bank, number);
        if (entry->kind == MusicTrack::Mono)
        {
            player->bits.interleaved = 0;
        }
        else if (entry->kind == MusicTrack::Interleaved)
        {
            player->bits.interleaved = 1;
        }

        u32 voices = VoiceCount(player);
        for (u32 i = 0; i < voices; i++)
        {
            player->voices[i] = FindFreeVoice(ReverbOff, 0);
            if (player->voices[i] == nullptr)
            {
                player->voices[i] = FindFreeVoice(ReverbOff, 1);
            }

            SoundVoice* voice = player->voices[i];
            voice->bits.takenByMusic = 1;
            if (voice->bits.reverbSend != 0)
            {
                voice->bits.reverbSend = 0;
                Platform::Audio::SetVoiceReverb(VoiceNumber(voice), false);
            }

            voice->bits.use = SoundVoice::UseMusic;
        }

        player->bits.group = music->bits.group;
        player->bits.loops = music->bits.loops;
        player->targetLeft = music->left;
        player->targetRight = music->right;
        player->fadeTime = music->fadeTime;
        player->bits.startsAtOnce = music->bits.startsAtOnce;

        s32 file = bank->samples;
        Platform::Audio::ReadMusic(file, entry->offset, entry->size);
        s32 rate = Platform::Audio::PitchOfRate(static_cast<s32>(entry->rate));
        player->rate = static_cast<u32>(rate);
        u16 pitch = static_cast<u16>(static_cast<s32>(g_PitchScale * static_cast<f32>(rate)));
        s32 channel = player->stream->channel;
        Platform::Audio::StreamMusic(file, channel, static_cast<u32>(VoiceNumber(player->voices[0])) | GroupOf(player), pitch,
                                     player->bits.loops == 0);
        if (player->bits.interleaved != 0)
        {
            Platform::Audio::InterleaveMusic(channel, MusicBlockSize(bank));
            Platform::Audio::AddMusicChannel(player->secondStream->channel, channel,
                                             static_cast<u32>(VoiceNumber(player->voices[1])) | GroupOf(player),
                                             player->soundBuffer + MusicStreamBuffer);
            Platform::Audio::SetMusicEnd(player->stream->channel, entry->value);
        }

        Platform::Audio::PrepareMusic(player->stream->channel);
        player->bits.state = MusicPlayer::Preparing;
    }

    s32 PlayMusic(f32 left, f32 right, f32 fadeTime, MusicPlayer* player)
    {
#ifndef __mips__
        if (std::getenv("TWINSANITY_TEST_AUDIO") != nullptr)
        {
            std::fprintf(stderr, "audio: [%llu ms] music asked, channel %d\n",
                         static_cast<unsigned long long>(Platform::Time::Ticks() / (Platform::Time::TicksPerSecond / 1000)),
                         player->stream != nullptr ? static_cast<s32>(player->stream->channel) : -1);
        }
#endif
        player->left = 0.0f;
        player->fadeTime = fadeTime;
        f32 silent = player->left;
        player->right = 0.0f;
        player->targetLeft = left;
        player->targetRight = right;
        player->fromLeft = silent;
        player->fromRight = silent;
        if (fadeTime <= silent)
        {
            SetMusicVolume(left, right, player);
            player->bits.state = MusicPlayer::Playing;
        }
        else
        {
            SetMusicVolume(silent, silent, player);
            player->bits.state = MusicPlayer::FadingIn;
        }

        return Platform::Audio::PlayMusic(player->stream->channel);
    }

    void StopMusic(MusicPlayer* player)
    {
        u32 voices = VoiceCount(player);
        for (u32 i = 0; i < voices; i++)
        {
            if (player->voices[i] != nullptr)
            {
                player->voices[i]->bits.takenByMusic = 0;
            }
        }

        if (player->stream != nullptr)
        {
            Platform::Audio::StopMusic(player->stream->channel);
        }

        player->bits.state = MusicPlayer::Stopped;
    }

    s32 SetMusicPitch(f32 scale, MusicPlayer* player)
    {
        s32 pitch = static_cast<s32>(scale * static_cast<f32>(player->rate));
        f32 scaled = static_cast<f32>(pitch);
        u32 voices = VoiceCount(player);
        s32 result = 0;
        for (u32 i = 0; i < voices; i++)
        {
            SoundVoice* voice = player->voices[i];
            voice->pitch = pitch;
            result = Platform::Audio::SetVoicePitch(VoiceNumber(voice), static_cast<u16>(static_cast<s32>(g_PitchScale * scaled)));
        }

        return result;
    }

    s32 SetMusicVolume(f32 left, f32 right, MusicPlayer* player)
    {
        SoundVoice* first = player->voices[0];
        if (player->bits.interleaved == 0 || g_MusicStereo == StereoOff)
        {
            s32 rightVolume = static_cast<s32>(right * VoiceVolumeScale);
            s32 leftVolume = static_cast<s32>(left * VoiceVolumeScale);
            first->right = Phase(rightVolume);
            first->left = Phase(leftVolume);
            s32 result = Platform::Audio::SetVoiceVolume(VoiceNumber(first), static_cast<s16>(first->left),
                                                         static_cast<s16>(first->right));
            if (player->bits.interleaved == 0)
            {
                return result;
            }

            // Both voices play both sides
            SoundVoice* second = player->voices[1];
            second->left = Phase(leftVolume);
            second->right = Phase(rightVolume);
            return Platform::Audio::SetVoiceVolume(VoiceNumber(second), static_cast<s16>(second->left),
                                                   static_cast<s16>(second->right));
        }

        // A side each
        first->right = 0;
        first->left = VoiceVolume(left);
        Platform::Audio::SetVoiceVolume(VoiceNumber(first), static_cast<s16>(first->left), 0);
        SoundVoice* second = player->voices[1];
        second->left = 0;
        second->right = VoiceVolume(right);
        return Platform::Audio::SetVoiceVolume(VoiceNumber(second), 0, static_cast<s16>(second->right));
    }

    bool UpdateMusic(f32 time, MusicPlayer* player, s32 paused)
    {
        switch (player->bits.state)
        {
        case MusicPlayer::Preparing:
        {
            u32 state = MusicPlayer::Preparing;
            if (Platform::Audio::IsMusicReady(player->stream->channel) && paused == 0)
            {
                if (player->bits.startsAtOnce == 0)
                {
                    state = MusicPlayer::Ready;
                }
                else
                {
                    PlayMusic(player->targetLeft, player->targetRight, player->fadeTime, player);
                    state = player->bits.state;
                }
            }

            player->bits.state = state;
            break;
        }
        case MusicPlayer::FadingIn:
            player->bits.state = FadeMusic(time, player) != 0 ? MusicPlayer::Playing : MusicPlayer::FadingIn;
            break;
        case MusicPlayer::Playing:
            if (!Platform::Audio::IsMusicPlaying(player->stream->channel))
            {
                player->bits.state = MusicPlayer::Stopping;
                break;
            }

            if (player->left != player->targetLeft || player->right != player->targetRight)
            {
                FadeMusic(time, player);
            }

            break;
        case MusicPlayer::FadingOut:
        {
            u32 state = MusicPlayer::FadingOut;
            if (FadeMusic(time, player) != 0)
            {
                state = player->bits.stopsAfterFade != 0 ? MusicPlayer::Stopping : MusicPlayer::Faded;
            }

            player->bits.state = state;
            break;
        }
        case MusicPlayer::Stopping:
            StopMusic(player);
            break;
        default:
            break;
        }

        return player->bits.state != MusicPlayer::Stopped;
    }

    s32 MoviePlayer::LendSound()
    {
        g_SoundLentToMovie = 1;
        ClearReverbs();
        s32 result = Platform::Audio::LendToMovie();
        flags.soundLent = 1;
        return result;
    }

    void MoviePlayer::ReclaimSound()
    {
        Platform::Audio::ReclaimFromMovie();
        RestoreReverbs();
        g_SoundLentToMovie = 0;
        flags.soundLent = 0;
    }
}

void FadeToCutsceneVolumes()
{
    g_CutsceneVolumeMode = FadingToCutsceneVolumes;
    g_CutsceneVolumeFade = 1.0f;
}

void FadeFromCutsceneVolumes()
{
    g_CutsceneVolumeMode = FadingFromCutsceneVolumes;
    g_CutsceneVolumeFade = 1.0f;
}

namespace
{
// The frames a core keeps its reverb without a voice sending to it
constexpr u32 ReverbKeptFrames = 0x12D;
// A core without reverb counts as this many free voices when the chunk's reverb picks one
constexpr s32 ReverbFreeCore = 10000;

bool StealableVoice(const SoundVoice* voice)
{
    return voice->bits.use == SoundVoice::UseNone;
}

SoundVoice* TakeVoice(SoundCore* core, s32 steal)
{
    u32 bit = 1;
    for (u32 index = 0; index < SoundCore::VoiceCount; index++, bit <<= 1)
    {
        SoundVoice* voice = nullptr;
        if ((core->voicesInUse & bit) == 0)
        {
            voice = &core->voices[index];
        }
        else if (steal != 0)
        {
            voice = &core->voices[index];
            if (!StealableVoice(voice))
            {
                voice = nullptr;
            }
            else
            {
                MuteVoice(voice);
            }
        }

        if (voice != nullptr)
        {
            voice->bits.number = index;
            voice->bits.secondCore = core->bits.number != 0;
            voice->bits.takenByMusic = 0;
            core->voicesInUse |= bit;
            return voice;
        }
    }

    return nullptr;
}

u32 FreeVoices(const SoundCore* core, s32 steal)
{
    u32 count = 0;
    u32 bit = 1;
    for (u32 index = 0; index < SoundCore::VoiceCount; index++, bit <<= 1)
    {
        if ((core->voicesInUse & bit) == 0)
        {
            count++;
        }
        else if (steal != 0 && StealableVoice(&core->voices[index]))
        {
            count++;
        }
    }

    return count;
}
}

// Whether the core is set to the reverb settings: their mode and depth (UseReverb has it inline)
extern "C" bool CoreHasReverb(const SoundCore* core, const ReverbSettings* settings) RETAIL(func_001E61E0);

bool CoreHasReverb(const SoundCore* core, const ReverbSettings* settings)
{
    return core->reverbMode == ReverbModeOf(static_cast<u8>(settings->bits.type)) &&
           static_cast<s32>(settings->depth * ReverbVolumeScale) == core->reverbDepthLeft;
}

SoundVoice* FindFreeVoice(s32 reverbMode, s32 steal)
{
    if (reverbMode != ReverbOff)
    {
        for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
        {
            if (core->reverbMode == reverbMode)
            {
                SoundVoice* voice = TakeVoice(core, steal);
                if (voice != nullptr)
                {
                    return voice;
                }
            }
        }
    }

    SoundCore* plain = nullptr;
    for (s32 index = 0; index < SoundCoreCount; index++)
    {
        if (g_SoundCores[index].reverbMode == ReverbOff && FreeVoices(&g_SoundCores[index], steal) != 0)
        {
            plain = &g_SoundCores[index];
            break;
        }
    }

    if (plain != nullptr)
    {
        return TakeVoice(plain, steal);
    }

    for (SoundCore* core = g_SoundCores; core < g_SoundCores + SoundCoreCount; core++)
    {
        if (FreeVoices(core, steal) != 0)
        {
            SoundVoice* voice = TakeVoice(core, steal);
            if (voice != nullptr)
            {
                return voice;
            }
        }
    }

    return nullptr;
}

void ReleaseVoice(SoundVoice* voice)
{
    SoundVoiceBits bits = voice->bits;
    voice->bits.use = SoundVoice::UseNone;
    voice->volume = OwnScale;
    voice->pitchScale = OwnScale;
    g_SoundCores[bits.secondCore].voicesInUse &= ~(1u << bits.number);
}

SoundVoice* VoiceOfNumber(u32 number)
{
    if (number < SoundCore::VoiceCount)
    {
        return &g_SoundCores[0].voices[number];
    }

    return &g_SoundCores[1].voices[number - SoundCore::VoiceCount];
}

void UpdateSoundCore(SoundCore* core)
{
    bool reverbUsed = false;
    u32 bit = 1;
    for (u32 index = 0; index < SoundCore::VoiceCount; index++, bit <<= 1)
    {
        SoundVoice* voice = &core->voices[index];
        if ((core->voicesInUse & bit) == 0 || voice->bits.takenByMusic != 0)
        {
            continue;
        }

        u32 playing = UpdateVoice(voice);
        SoundVoiceBits bits = voice->bits;
        if (playing == 0)
        {
            voice->pitchScale = OwnScale;
            voice->volume = OwnScale;
            voice->bits.use = SoundVoice::UseNone;
            core->voicesInUse &= ~(1u << bits.number);
        }
        else if (bits.reverbSend != 0)
        {
            reverbUsed = true;
        }
    }

    if (reverbUsed || core->reverbMode == ReverbOff)
    {
        core->reverbIdleFrames = 0;
    }
    else if (static_cast<u32>(core->reverbIdleFrames) < ReverbKeptFrames)
    {
        core->reverbIdleFrames++;
    }
    else
    {
        SetReverb(core, ReverbOff, 0, 0);
        core->reverbIdleFrames = 0;
    }
}

s32 UseReverb(const ReverbSettings* settings)
{
    u8 type = static_cast<u8>(settings->bits.type);
    s32 mode = ReverbModeOf(type);
    for (s32 index = 0; index < SoundCoreCount; index++)
    {
        SoundCore* core = &g_SoundCores[index];
        if (core->reverbMode == ReverbModeOf(type) &&
            static_cast<s32>(settings->depth * ReverbVolumeScale) == core->reverbDepthLeft)
        {
            return mode;
        }
    }

    s32 most = -1;
    s32 chosen = -1;
    for (s32 index = 0; index < SoundCoreCount; index++)
    {
        SoundCore* core = &g_SoundCores[index];
        s32 free = ReverbFreeCore;
        if (core->reverbMode != ReverbOff)
        {
            free = 0;
            for (u32 voice = 0; voice < SoundCore::VoiceCount; voice++)
            {
                if ((core->voicesInUse & 1u << voice) == 0)
                {
                    free++;
                }
            }
        }

        if (most < free)
        {
            most = free;
            chosen = index;
        }
    }

    SetReverbFromSettings(&g_SoundCores[chosen], reinterpret_cast<const u8*>(settings));
    return mode;
}

void SetSoundListener(ReferencedObject* object)
{
    AssignReference(&g_SoundListener, object);
}

s32 ListenerVoiceKind()
{
    auto* listener = static_cast<InstanceContext*>(g_SoundListener != nullptr ? g_SoundListener->object : nullptr);
    if (listener == nullptr)
    {
        return ReverbOff;
    }

    ChunkData* chunk = listener->chunk;
    const ReverbSettings* settings = &chunk->reverb;
    if (chunk->soundBoxCount != 0)
    {
        ObjectPlace* place = listener->place;
        place->SyncPosition();
        Vector4 position = place->position;
        if (chunk->InSoundBox(&position) != 0)
        {
            settings = &chunk->boxReverb;
        }
    }

    if (settings->bits.type == ReverbSettings::NoReverb)
    {
        return ReverbOff;
    }

    return UseReverb(settings);
}

u32 SoundBox::Contains(const Vector4* point)
{
    f32 dy = point->y - position.y;
    if (!((point->x - position.x) * (point->x - position.x) + dy * dy + (point->z - position.z) * (point->z - position.z) <=
          radiusSquared))
    {
        return 0;
    }

    Vector4 local = *point;
    VuTransformPoint(&inverse, &local, &local);
    return reinterpret_cast<Box*>(this)->Contains(&local);
}

void UpdateCutsceneVolumes(f32 time)
{
    // The groups' volumes in cutscenes: a quarter, but the movie group's (the cutscenes' music) left alone
    static constexpr f32 CutsceneLevels[VolumeGroupCount] = {0.25f, 0.25f, 0.25f, 1.0f};
    if (g_CutsceneVolumeMode == OwnVolumes || g_CutsceneVolumeMode == CutsceneVolumes)
    {
        return;
    }

    if (0.0f < g_CutsceneVolumeFade)
    {
        g_CutsceneVolumeFade = g_CutsceneVolumeFade - time / g_CutsceneVolumeFade;
        if (g_CutsceneVolumeFade < 0.0f)
        {
            g_CutsceneVolumeFade = 0.0f;
        }
    }

    f32 share = g_CutsceneVolumeFade;
    if (g_CutsceneVolumeMode == FadingFromCutsceneVolumes)
    {
        share = 1.0f - share;
    }

    f32* scales = GroupScales();
    for (u32 group = 0; group < VolumeGroupCount; group++)
    {
        f32 level = CutsceneLevels[group];
        scales[group] = level + (1.0f - level) * share;
        f32 volume = GroupVolumeLevel(static_cast<s32>(group));
        SetGroupVolume(volume, volume, static_cast<s32>(group));
    }

    if (g_CutsceneVolumeFade == 0.0f)
    {
        g_CutsceneVolumeMode = g_CutsceneVolumeMode == FadingToCutsceneVolumes ? CutsceneVolumes : OwnVolumes;
    }
}

f32 GroupVolumeLevel(s32 group)
{
    constexpr f32 Half = 1.0f / (2.0f * GroupVolumeScale);
    return static_cast<f32>(static_cast<u16>(g_GroupVolumes[group].left)) * Half +
           static_cast<f32>(static_cast<u16>(g_GroupVolumes[group].right)) * Half;
}

void CreateMusic()
{
    g_Music = ConstructMusic(static_cast<MusicSystem*>(MemoryAllocate(sizeof(MusicSystem))));
}

void SetMusicStereo(u32 mode)
{
    g_MusicStereo = mode;
    for (MusicPlayer& player : g_Music->players)
    {
        if (player.bits.state != MusicPlayer::Stopped)
        {
            ApplyMusicVolume(&player);
        }
    }
}

void ReclaimMusicBuffers()
{
    for (MusicPlayer& player : g_Music->players)
    {
        LendMusicPlayer(&player, 0);
    }
}

void LendMusicPlayer(MusicPlayer* player, u32 lend)
{
    if (lend == 0)
    {
        if (player->bits.state != MusicPlayer::LentToMovie)
        {
            return;
        }

        player->bits.state = MusicPlayer::Stopped;
        return;
    }

    if (player->bits.state != MusicPlayer::Stopped)
    {
        StopMusic(player);
    }

    player->bits.state = MusicPlayer::LentToMovie;
}

s32 ApplyMusicVolume(MusicPlayer* player)
{
    return SetMusicVolume(player->left, player->right, player);
}

void FadeMusicOut(f32 time, MusicPlayer* player, u32 stops)
{
    constexpr f32 ShortestFade = Rounded(0.3);
    player->fadeTime = time;
    if (time < ShortestFade)
    {
        player->fadeTime = ShortestFade;
    }

    player->fromLeft = player->left;
    player->bits.state = MusicPlayer::FadingOut;
    player->bits.stopsAfterFade = stops;
    player->fromRight = player->right;
    player->targetLeft = 0.0f;
    player->targetRight = 0.0f;
}

void FadeMusicTo(f32 left, f32 right, f32 time, MusicPlayer* player)
{
    if (player->bits.state == MusicPlayer::FadingOut)
    {
        player->bits.state = MusicPlayer::Playing;
    }

    player->fadeTime = time;
    player->fromRight = player->right;
    player->targetLeft = left;
    player->targetRight = right;
    player->fromLeft = player->left;
}

MusicPlayer* ConstructMusicPlayer(MusicPlayer* player)
{
    player->iopBuffer = 0;
    player->voices[0] = nullptr;
    player->voices[1] = nullptr;
    player->bits.interleaved = 0;
    player->bits.state = MusicPlayer::WithoutBuffers;
    player->stream = nullptr;
    player->secondStream = nullptr;
    return player;
}

u32 FadeMusic(f32 time, MusicPlayer* player)
{
    constexpr f32 Instant = Rounded(1e-05);
    u32 leftDone = 0;
    u32 rightDone = 0;
    if (player->fadeTime < Instant)
    {
        leftDone = 1;
        rightDone = 1;
        player->left = player->targetLeft;
        player->right = player->targetRight;
    }
    else
    {
        f32 step = time / player->fadeTime;
        f32 targetLeft = player->targetLeft;
        f32 left = player->left + (targetLeft - player->fromLeft) * step;
        player->left = left;
        player->right = player->right + (player->targetRight - player->fromRight) * step;
        if (player->fromLeft < targetLeft ? targetLeft <= left : left <= targetLeft)
        {
            player->left = targetLeft;
            leftDone = 1;
        }

        f32 targetRight = player->targetRight;
        f32 right = player->right;
        if (player->fromRight < targetRight ? targetRight <= right : right <= targetRight)
        {
            player->right = targetRight;
            rightDone = 1;
        }
    }

    SetMusicVolume(player->left, player->right, player);
    return leftDone != 0 ? rightDone : 0;
}

void ReadSoundParameters(u16* parameters, Stream* stream)
{
    // The pitch and four parameters
    constexpr u32 SoundParameters = 5;
    for (u32 index = 0; index < SoundParameters; index++)
    {
        stream->ReadS16(reinterpret_cast<s16*>(&parameters[index]));
    }
}

f32 RemainingShare(f32 part, f32 whole, f32 total)
{
    return total - part / whole;
}

GameSound* SoundById(u16 id)
{
    GameSound* sound = nullptr;
    GameResources* resources = G_GameResourcesObjectPointer;
    ResourceTable* sounds = resources->sounds;
    ResourceTable* voices = resources->voices[g_CurrentLanguage];
    if (id < sounds->bits.capacity)
    {
        u16 copy = id;
        if (copy != NoSoundId)
        {
            sound = static_cast<GameSound*>(sounds->items[copy & ResourceIndexMask]);
        }

        if (sound == nullptr && copy != NoSoundId)
        {
            sound = static_cast<GameSound*>(voices->items[copy & ResourceIndexMask]);
        }
    }

    return sound;
}

GameSound* GameSound::Construct(GameSound* sound, Stream* stream)
{
    ConstructResourceHeader(sound);
    sound->name.string = nullptr;
    sound->name.capacity = 0;
    sound->name.length = 0;
    RetailLibc::MemorySet(&sound->flags, 0, sizeof(sound->flags) + sizeof(sound->unused09));
    sound->Read(stream);
    return sound;
}

void GameSound::Read(Stream* stream)
{
    u32 header;
    stream->ReadU32(&header);
    ReadSoundParameters(&basePitch, stream);
    stream->ReadS32(&size);
    stream->ReadS32(&offset);
}

void GameSound::Destroy(u32 destroyFlags)
{
    FreeSoundSamples(reinterpret_cast<u32>(&basePitch), soundId);
    StringDestroy(&name);
    if ((destroyFlags & FreeAfterDestroy) != 0)
    {
        MemoryDeallocate2_(this);
    }
}
