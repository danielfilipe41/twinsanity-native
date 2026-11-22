#include "platform/stream.h"
#include "platform/files.h"

#include "settings.h"

#include "audio/soundmodule.h"
#include "audio/audio.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

// The sdl3 host's side of Platform::Stream: reads from the disc files on the host filesystem through Platform::Files
// (the null platform's own stream.cpp returns 0 from FreeBufferMemory and drops sound banks, which stops music playing).
//
// The PS2 reads on the I/O processor while the EE goes on: a channel's read is in flight until IsReading turns false (or
// Wait returns), and the game steps its readers meanwhile. One worker thread per channel does the same here, so a chunk
// load or a music block doesn't hold the logic step up on the disc.
namespace Platform::Stream
{
// The chunk depth the pop-in distance asks for: the focus chunk and its links (2, retail's), and one link further for every
// whole step above it (1.5x reads 3, 2x 3, 3x 4, 4x 5). Capped, since each depth is a whole ring of chunks read from the disc
f32 FrameBudgetScale()
{
    // Eight times the console's slice: some 13 ms of a 60 Hz frame, so a chunk that took two seconds of slivers is read in a
    // quarter of a second
    return 8.0f;
}

u32 ChunkDepth()
{
    const f32 reach = HostSettingsDrawDistance();
    s32 depth = 2;
    if (reach > 1.0f)
    {
        depth = 2 + static_cast<s32>(reach - 1.0f + 0.5f);
    }

    constexpr s32 Deepest = 6;
    if (depth > Deepest)
    {
        depth = Deepest;
    }

    return static_cast<u32>(depth);
}

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
    // The disc image's own offset of the bytes, when the page is asked for them instead (the web's stream), and whether the
    // read is one of those (offset 0 is a real image offset, so the offset alone cannot say)
    u32 imageOffset = 0;
    bool image = false;
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
    // Counts the reads the channel has started: a page's copy of one it has since finished here (Wait) is dropped by it
    u32 generation = 0;
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

// The bytes themselves, on the calling thread: into the sound processor's bank or into the game's memory
s32 ReadBytes(ReadJob* job)
{
    if (job->descriptor < 0)
    {
        return 0;
    }

    if (job->soundBank)
    {
        return Platform::Files::ReadAt(job->descriptor, job->offset, job->samples.data(), static_cast<s32>(job->size));
    }

    return Platform::Files::ReadAt(job->descriptor, job->offset, job->destination, static_cast<s32>(job->size));
}

// What follows the bytes being in place: a short read said once in a while, and a bank's samples given to the module
void Finish(const ReadJob& job, s32 got)
{
    if (got != static_cast<s32>(job.size))
    {
        ReportReadFailure(job.path.c_str(), job.offset, job.size, got);
    }

    if (job.soundBank)
    {
        // The module's memory is behind the audio lock (the mixer reads it): registering the bank takes it as the game's
        // own synchronous read did
        SdlAudio::LockAudio();
        SdlAudio::Spu2::Module().LoadSoundBank(job.bank, job.samples.data(), job.size);
        SdlAudio::UnlockAudio();
    }
}

void Work(ReadJob* job)
{
    const s32 got = ReadBytes(job);
    Finish(*job, got);
}

#if defined(__EMSCRIPTEN__)
// The page's chunk cache, asked for a read's bytes: it copies them where they were asked for and answers how many. The copy is
// all or nothing -- 0 means a chunk the range needs is not held -- so a destination is never left half written
EM_JS(s32, CopyCached, (u32 offset, u32 size, void* destination), {
    return Module.twinsanityCopyCached ? Module.twinsanityCopyCached(offset, size, destination) : 0;
});

// Hands a read to the page, which fetches the chunks it needs and calls HostReadDone once they are held. Answers -1 when the
// page has no disc to read that way (an image it copied into its own file system), so the read runs on this thread instead
EM_JS(s32, ReadInto, (s32 channel, u32 generation, u32 offset, u32 size), {
    if (!Module.twinsanityReadInto) {
        return -1;
    }
    Module.twinsanityReadInto(channel, generation, offset, size);
    return 0;
});

// A read in flight, finished here on the calling thread instead of by the page: a page cannot wait on a fetch (the game's
// thread is the page's event loop), and the game's blocking readers (Wait, and closing a file a channel is reading) need the
// bytes before they go on. The generation moves on first, so the fetch the page has on the way is dropped rather than looked
// at when it lands -- and the page checks that first, so a read waited for in the same frame is never even fetched
void FinishChannel(Channel* state)
{
    ReadJob job;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->busy)
        {
            return;
        }
        state->generation++;
        job = std::move(state->job);
        state->job = ReadJob();
    }

    // What the page already brought is taken as it is: a fetch that landed during the frame needs no request of its own
    s32 got = -1;
    if (job.image && !job.soundBank)
    {
        got = CopyCached(job.imageOffset, job.size, job.destination);
        if (got != static_cast<s32>(job.size))
        {
            got = -1;
        }
    }
    if (got < 0)
    {
        got = ReadBytes(&job);
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->busy = false;
        state->activeFile = -1;
    }
    Finish(job, got);
    job.samples.clear();
}
#endif

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

// A read run on the calling thread, the way a page's is: the channel is marked busy for as long as it takes, so IsReading and
// Wait answer the same as they would with a worker behind them
void ReadNow(Channel* state, ReadJob& job)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->job = std::move(job);
        state->busy = true;
        state->activeFile = state->job.file;
    }
    Work(&state->job);
    state->job.samples.clear();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->busy = false;
        state->activeFile = -1;
    }
}

// Whether a channel's reads run on the calling thread instead of a worker: on by necessity on the web (a page's wasm cannot
// start a thread without the page being cross-origin isolated) and by request on a desktop build, through
// TWINSANITY_STREAM_INLINE, so the stutter a page shows when it reaches a new area is reproducible where the frame log can
// measure it. A local file read of a few hundred KB is not what such a frame spends, so the desktop keeps the part that matters
// (the parse and upload the read unblocks) even with the disc's speed out of the picture
bool InlineReads()
{
#if defined(__EMSCRIPTEN__)
    return true;
#else
    static const bool inlineReads = [] {
        const char* value = std::getenv("TWINSANITY_STREAM_INLINE");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return inlineReads;
#endif
}

// Hands a read to a channel (waiting for the previous one, as a channel reads one thing at a time)
void Submit(s32 channel, ReadJob job)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return;
    }

#if defined(__EMSCRIPTEN__)
    // A page reads the disc itself: a read whose chunks the page already holds is copied here, on this thread, and finishes at
    // once; one that has to fetch them is handed to the page and finished by HostReadDone when they arrive, while the game goes
    // on stepping. Everything else (a sound bank, a mod's file, a raw dump) is read on this thread, as it was
    if (state->busy)
    {
        // A channel reads one thing at a time, and a read in flight cannot be waited for on a page: this one is finished here
        // first, and the page's copy of it is dropped
        FinishChannel(state);
    }

    const u32 size = job.size;
    u32 imageOffset = 0;
    if (!job.soundBank && Platform::Files::ImageRange(job.file, job.offset, size, &imageOffset))
    {
        job.imageOffset = imageOffset;
        job.image = true;
        const s32 copied = CopyCached(imageOffset, size, job.destination);
        if (copied == static_cast<s32>(size))
        {
            Finish(job, copied);
            return;
        }

        u32 generation = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            generation = ++state->generation;
            state->job = std::move(job);
            state->busy = true;
            state->activeFile = state->job.file;
        }
        if (ReadInto(channel, generation, imageOffset, size) == 0)
        {
            return;
        }

        // The page cannot read it: the job comes back and is read here
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            job = std::move(state->job);
            state->job = ReadJob();
            state->busy = false;
            state->activeFile = -1;
        }
    }

    ReadNow(state, job);
    return;
#endif

    if (InlineReads())
    {
        ReadNow(state, job);
        return;
    }

#if !defined(__EMSCRIPTEN__)
    std::unique_lock<std::mutex> lock(state->mutex);
    state->ready.wait(lock, [state] { return !state->busy; });
    state->job = std::move(job);
    state->busy = true;
    state->pending = true;
    state->activeFile = state->job.file;
    lock.unlock();
    state->ready.notify_all();
#endif
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
#if !defined(__EMSCRIPTEN__)
    for (s32 channel = 0; channel < g_ChannelCount; channel++)
    {
        std::thread(Worker, &g_Channels[channel]).detach();
    }
#endif
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
#if defined(__EMSCRIPTEN__)
        // A page cannot wait on a fetch: a read of this file still in flight is finished here instead
        bool reading = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            reading = state->busy && state->activeFile == file;
        }
        if (reading)
        {
            FinishChannel(state);
        }
#else
        std::unique_lock<std::mutex> lock(state->mutex);
        state->ready.wait(lock, [state, file] { return !state->busy || state->activeFile != file; });
#endif
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

#if defined(__EMSCRIPTEN__)
    // A page cannot block on a fetch: the read is finished here, on this thread, and the frame that asked for it goes on with
    // the bytes where it wanted them
    FinishChannel(state);
    return 0;
#else
    std::unique_lock<std::mutex> lock(state->mutex);
    state->ready.wait(lock, [state] { return !state->busy; });
    return 0;
#endif
}

#if defined(__EMSCRIPTEN__)
// The page's answer to ReadInto: the read's chunks are held, so they are copied out of the page's cache here and the read
// finishes. A read this channel has since done itself has moved the generation on, and the copy is dropped: the destination
// is the game's memory, and what it holds now is not this read's. Called from the page, so it is a C symbol of the module's
// own (stream.cpp's EM_JS ReadInto is what starts the read it answers)
// Whether the read the page is about to fetch is still the one its channel wants. A read the game waits for in the frame it
// was asked for is finished here, and the page asks this before spending a request on it
extern "C" EMSCRIPTEN_KEEPALIVE s32 HostReadValid(s32 channel, u32 generation)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    return state->busy && state->generation == generation;
}

extern "C" EMSCRIPTEN_KEEPALIVE void HostReadDone(s32 channel, u32 generation)
{
    Channel* state = ChannelAt(channel);
    if (state == nullptr)
    {
        return;
    }

    ReadJob job;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->busy || state->generation != generation)
        {
            return;
        }
        job = std::move(state->job);
        state->job = ReadJob();
    }

    const s32 got = CopyCached(job.imageOffset, job.size, job.destination);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->busy = false;
        state->activeFile = -1;
    }
    Finish(job, got);
    job.samples.clear();
}
#endif
}
