// Sound processor: silent, every voice missing.
#include "platform/audio.h"

void Platform::Audio::Reset()
{
}

void Platform::Audio::PauseAll()
{
}

void Platform::Audio::ResumeAll()
{
}

s32 Platform::Audio::LendToMovie()
{
    return 0;
}

s32 Platform::Audio::ReclaimFromMovie()
{
    return 0;
}

void Platform::Audio::CompactSoundMemory()
{
}

void Platform::Audio::SetGroupVolume(s32 group, u32 left, u32 right)
{
    (void)group;
    (void)left;
    (void)right;
}

s32 Platform::Audio::SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    (void)voice;
    (void)left;
    (void)right;
    return -1;
}

s32 Platform::Audio::SetVoicePitch(s32 voice, u16 pitch)
{
    (void)voice;
    (void)pitch;
    return -1;
}

void Platform::Audio::SetVoiceReverb(s32 voice, bool on)
{
    (void)voice;
    (void)on;
}

void Platform::Audio::ReleaseVoice(s32 voice, u32 rate)
{
    (void)voice;
    (void)rate;
}

s32 Platform::Audio::IsVoiceFree(s32 voice)
{
    (void)voice;
    return -1;
}

void Platform::Audio::SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    (void)core;
    (void)mode;
    (void)depthLeft;
    (void)depthRight;
    (void)delay;
    (void)feedback;
}

void Platform::Audio::ClearReverb(s32 core)
{
    (void)core;
}

void Platform::Audio::SetReverbVolume(s32 core, s16 left, s16 right)
{
    (void)core;
    (void)left;
    (void)right;
}

u32 Platform::Audio::ReserveSoundMemory(u32 size)
{
    (void)size;
    return 0;
}

void Platform::Audio::SoundBankLoaded(u16 bank)
{
    (void)bank;
}

void Platform::Audio::ReserveSounds(u16 count)
{
    (void)count;
}

void Platform::Audio::ReleaseSound(u32 sound)
{
    (void)sound;
}

s32 Platform::Audio::PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release, u32 loops)
{
    (void)sound;
    (void)voiceAndGroup;
    (void)left;
    (void)right;
    (void)pitch;
    (void)attack;
    (void)release;
    (void)loops;
    return -1;
}

s32 Platform::Audio::PitchOfRate(s32 rate)
{
    (void)rate;
    return -1;
}

u32 Platform::Audio::ChannelBufferAddress(s32 channel)
{
    (void)channel;
    return 0;
}

void Platform::Audio::ReadMusic(s32 file, u32 offset, u32 size)
{
    (void)file;
    (void)offset;
    (void)size;
}

s32 Platform::Audio::StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once)
{
    (void)file;
    (void)channel;
    (void)voiceAndGroup;
    (void)pitch;
    (void)once;
    return -1;
}

void Platform::Audio::InterleaveMusic(s32 channel, u32 blockSize)
{
    (void)channel;
    (void)blockSize;
}

void Platform::Audio::AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    (void)child;
    (void)parent;
    (void)voiceAndGroup;
    (void)soundAddress;
}

void Platform::Audio::SetMusicEnd(s32 channel, u32 end)
{
    (void)channel;
    (void)end;
}

void Platform::Audio::PrepareMusic(s32 channel)
{
    (void)channel;
}

bool Platform::Audio::IsMusicReady(s32 channel)
{
    (void)channel;
    return false;
}

s32 Platform::Audio::PlayMusic(s32 channel)
{
    (void)channel;
    return -1;
}

bool Platform::Audio::IsMusicPlaying(s32 channel)
{
    (void)channel;
    return false;
}

void Platform::Audio::StopMusic(s32 channel)
{
    (void)channel;
}
