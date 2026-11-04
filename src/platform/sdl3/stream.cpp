#include "platform/stream.h"
#include "platform/files.h"

#include "audio/soundmodule.h"
#include "audio/audio.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The sdl3 host's side of Platform::Stream: reads from the disc files on the host filesystem through Platform::Files
// (the null platform's own stream.cpp returns 0 from FreeBufferMemory and drops sound banks, which stops music playing).
//
// The PS2 reads on the I/O processor while the EE goes on: a channel's read is in flight until IsReading turns false (or
// Wait returns), and the game steps its readers meanwhile. One worker thread per channel does the same here, so a chunk
// load or a music block doesn't hold the logic step up on the disc.
namespace Platform::Stream
{
namespace
{
struct FileInfo
{
    s32 fd;
    u32 size;
    // Kept so a bank read can say which disc file its offset belongs to: a sound looked at offline at the wrong offset is a
    // sound that reads as noise and says nothing about the sound
    std::string path;
};

std::vector<FileInfo> g_OpenFiles;
// The size of the file opened last: FileSize is its size until the next file is opened (platform/stream.h)
u32 g_LastFileSize = 0;

// A failed or short read, said once in a while: a truncated dump otherwise decodes zeros and the first symptom is
// scenery drawn out of nonsense, with nothing pointing at the read that failed
void ReportReadFailure(const char* path, u32 offset, u32 wanted, s32 got)
{
    static std::atomic<u32> reported{0};
    if (reported.load() >= 8)
    {
        return;
    }

    reported.fetch_add(1);
    std::fprintf(stderr, "stream: %s: %u bytes at %u, got %d\n", path != nullptr ? path : "(no path)", wanted, offset,
                 got);
}

// One read handed to a channel's worker: the file's number and descriptor are taken when it's asked for, so the worker
// never reads the open files' table (which the game thread grows)
struct ReadJob
{
    s32 file = -1;
    s32 descriptor = -1;
    u32 offset = 0;
    u32 size = 0;
    u8* destination = nullptr;
    std::string path;
    // A sound bank: the bytes go to the sound processor's module instead of the game's memory
    bool soundBank = false;
    u32 bank = 0;
    std::vector<u8> samples;
};

struct Channel
{
    std::mutex mutex;
    std::condition_variable ready;
    bool quit = false;
    bool busy = false;
    bool pending = false;
    // The file the job in flight reads (the job itself is moved out to the worker)
    s32 activeFile = -1;
    ReadJob job;
};

// The channels' workers and their state: made once, and never let go (the process exits with the threads detached)
Channel* g_Channels = nullptr;
s32 g_ChannelCount = 0;

Channel* ChannelAt(s32 channel)
{
    return channel >= 0 && channel < g_ChannelCount ? &g_Channels[channel] : nullptr;
}

void Work(ReadJob* job)
{
    if (job->descriptor < 0)
    {
        return;
    }

    s32 got = 0;
    if (job->soundBank)
    {
        got = Platform::Files::ReadAt(job->descriptor, job->offset, job->samples.data(), static_cast<s32>(job->size));
        if (got != static_cast<s32>(job->size))
        {
            ReportReadFailure(job->path.c_str(), job->offset, job->size, got);
        }

        // The module's memory is behind the audio lock (the mixer reads it): registering the bank takes it as the game's
        // own synchronous read did
        SdlAudio::LockAudio();
        SdlAudio::Spu2::Module().LoadSoundBank(job->bank, job->samples.data(), job->size);
        SdlAudio::UnlockAudio();
        return;
    }

    got = Platform::Files::ReadAt(job->descriptor, job->offset, job->destination, static_cast<s32>(job->size));
    if (got != static_cast<s32>(job->size))
    {
        ReportReadFailure(job->path.c_str(), job->offset, job->size, got);
    }
}

void Worker(Channel* channel)
{
    for (;;)
    {
        ReadJob job;
        {
            std::unique_lock<std::mutex> lock(channel->mutex);
            channel->ready.wait(lock, [channel] { return channel->quit || channel->pending; });
            if (channel->quit)
            {
                return;
            }
            job = std::move(channel->job);
            channel->job = ReadJob();
            channel->pending = false;
        }

        Work(&job);
        job.samples.clear();

        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            channel->busy = false;
            channel->activeFile = -1;
        }
        channel->ready.notify_all();
    }
}

// Hands a read to a channel (waiting for the previous one, as a channel reads one thing at a time)
void Submit(s32 channel, ReadJob job)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    state->ready.wait(lock, [state] { return !state->busy; });
    state->job = std::move(job);
    state->busy = true;
    state->pending = true;
    state->activeFile = state->job.file;
    lock.unlock();
    state->ready.notify_all();
}

s32 GetFreeSlot()
{
    for (size_t i = 0; i < g_OpenFiles.size(); i++)
    {
        if (g_OpenFiles[i].fd < 0)
        {
            return static_cast<s32>(i);
        }
    }
    g_OpenFiles.push_back({-1, 0, {}});
    return static_cast<s32>(g_OpenFiles.size() - 1);
}
}

s32 Initialise(s32 channels, void* workMemory)
{
    (void)workMemory;
    g_OpenFiles.clear();
    g_LastFileSize = 0;
    if (g_Channels != nullptr)
    {
        return 0;
    }

    g_ChannelCount = channels > 0 ? channels : 1;
    g_Channels = new Channel[static_cast<size_t>(g_ChannelCount)];
    for (s32 channel = 0; channel < g_ChannelCount; channel++)
    {
        std::thread(Worker, &g_Channels[channel]).detach();
    }
    return 0;
}

s32 Update()
{
    return 0;
}

State GetState()
{
    return State::Running;
}

void AttachBuffer(s32 channel, u32 size, u32 soundAddress, u32 soundSize)
{
    (void)size;
    // A channel that plays music out of the sound processor's memory is given the ring the player reserved there: the module
    // streams the track's blocks through it (the game's own file stream, whose buffer this is, reads nothing itself)
    if (soundAddress != 0 && soundSize != 0)
    {
        SdlAudio::LockAudio();
        SdlAudio::Spu2::Module().SetChannelBuffer(channel, soundAddress, soundSize);
        SdlAudio::UnlockAudio();
    }
}

void DetachBuffer(s32 channel)
{
    (void)channel;
}

u32 FreeBufferMemory()
{
    return 0x1000000; // large amount
}

s32 OpenFile(const char* path)
{
    if (path == nullptr)
    {
        return NoFile;
    }
    // The disc path straight to the host's files (Platform::Files matches the case)
    s32 fd = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (fd < 0)
    {
        g_LastFileSize = 0;
        return NoFile;
    }

    // The file's size, the read position left where it was
    s32 old = Platform::Files::Seek(fd, 0, Platform::Files::SeekCurrent);
    s32 end = Platform::Files::Seek(fd, 0, Platform::Files::SeekEnd);
    Platform::Files::Seek(fd, old, Platform::Files::SeekSet);
    if (end < 0)
    {
        end = 0;
    }

    g_LastFileSize = static_cast<u32>(end);
    s32 slot = GetFreeSlot();
    g_OpenFiles[slot] = {fd, g_LastFileSize, path};
    return slot;
}

u32 FileSize()
{
    return g_LastFileSize;
}

void CloseFile(s32 file)
{
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size()))
    {
        return;
    }

    // A read of this file still in flight has to land before its descriptor goes: the game closes a file once its reads
    // are done, and this waits for the ones a channel still holds
    for (s32 channel = 0; channel < g_ChannelCount; channel++)
    {
        Channel* state = &g_Channels[channel];
        std::unique_lock<std::mutex> lock(state->mutex);
        state->ready.wait(lock, [state, file] { return !state->busy || state->activeFile != file; });
    }

    if (g_OpenFiles[file].fd >= 0)
    {
        Platform::Files::Close(g_OpenFiles[file].fd);
        g_OpenFiles[file].fd = -1;
        g_OpenFiles[file].size = 0;
    }
}

void Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size()))
    {
        return;
    }
    const FileInfo& info = g_OpenFiles[file];
    if (info.fd < 0)
    {
        return;
    }

    ReadJob job;
    job.file = file;
    job.descriptor = info.fd;
    job.offset = offset;
    job.size = size;
    job.destination = static_cast<u8*>(destination);
    job.path = info.path;
    Submit(channel, std::move(job));
}

void ReadSoundBank(s32 channel, u32 bank, s32 file, u32 offset, u32 size)
{
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size()) || g_OpenFiles[file].fd < 0 || size == 0)
    {
        return;
    }

    // Retail loads the bank's samples straight into the sound processor's memory (ps2/stream.cpp's LoadToSpu, the module
    // writing at g_MsSoundBankAddress and MsSetBankAddress giving them their sound's ID). The host reads them and hands them
    // to the module, which puts them in its own memory and registers them under the sound's ID.
    ReadJob job;
    job.file = file;
    job.descriptor = g_OpenFiles[file].fd;
    job.offset = offset;
    job.size = size;
    job.path = g_OpenFiles[file].path;
    job.soundBank = true;
    job.bank = bank;
    job.samples.assign(size, 0);
    Submit(channel, std::move(job));
}

bool IsReading(s32 channel)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    return state->busy;
}

s32 Wait(s32 channel)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return 0;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    state->ready.wait(lock, [state] { return !state->busy; });
    return 0;
}
}
