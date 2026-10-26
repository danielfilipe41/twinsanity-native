// Movie player: Platform::Movie over the software MPEG-2 core (mpeg2.h). The PS2's player
// (src/platform/ps2/movie/player.cpp) streams sectors, decodes on the IPU into the depth buffer's page and draws
// bucket-27 sprites from the vertical blank; the host reads the whole PSS through Platform::Files, demuxes up front,
// decodes to planar YUV and presents through bgfx view 27 (the Movies bucket), converting to RGBA
// on the CPU (BT.601 studio swing, clamped; one 640x480 convert per frame) because the shared 2D program only samples
// RGBA. LentMemorySize bytes are lent until Close and ignored (heap vectors, so nullptr is safe); the selected PCM
// sub-stream goes to the mixer's movie PCM at Open, looping like retail's ring until Close.
#include "../audio/spu_mixer.h"
#include "../graphics/host_graphics.h"
#include "../graphics/draws_host.h"
#include "movie_debug.h"
#include "mpeg2.h"
#include "platform/files.h"
#include "platform/movie.h"

#include <bgfx/bgfx.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// BgfxWindowWidth/Height live in the graphics context's header, which pulls SDL headers; the movie only needs the
// two sizes, restated here (entry.cpp's fixed 960x540 window)
// The Movies bucket's view: the picture quad draws here, over the frame's view 0
constexpr bgfx::ViewId MovieView = 27;

// A quad vertex matching the shared 2D program's layout (Position 3f, TexCoord0 2f, Color0 4f): kept local so this
// file never touches the sibling-owned 2D objects
struct MovieVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 s;
    f32 t;
    f32 red;
    f32 green;
    f32 blue;
    f32 alpha;
};

bgfx::VertexLayout MovieLayout()
{
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .end();
    return layout;
}

MovieVertex MovieCorner(f32 fractionX, f32 fractionY, f32 s, f32 t)
{
    f32 clipX = 0.0f;
    f32 clipY = 0.0f;
    Platform::Graphics::HostMapToClip(fractionX, fractionY, &clipX, &clipY);
    return {clipX, clipY, 0.0f, s, t, 1.0f, 1.0f, 1.0f, 1.0f};
}
namespace
{
// The host player, kept in the game's PlayerSize bytes (Construct zero-fills then placement-news it)
constexpr u32 QueueSlots = 2;
constexpr u32 AudioProbeBytes = 64;
// The selected PCM sub-stream's header (SShd then SSbd: rate, channels,
// interleave and body size, retail's MovieAudioHeader) and its body's bytes
constexpr u32 AudioHeaderBytes = 0x28;
constexpr u32 AudioHeaderRate = 12;
constexpr u32 AudioHeaderChannels = 16;
constexpr u32 AudioHeaderInterleave = 20;
constexpr s32 MoviePcmRate = 48000;
constexpr s32 MoviePcmChannels = 2;

struct HostPlayer
{
    bool opened = false;
    bool finished = false;
    void (*present)() = nullptr;
    u32 audioChannel = 0;
    // The file's bytes (the demuxer points into these, so they outlive it), the demuxer and the video decoder
    std::vector<u8> fileBytes;
    std::unique_ptr<HostMovie::Demux> demux;
    HostMovie::Decoder decoder;
    // The selected PCM sub-stream: its total bytes, its first bytes, its
    // header's bytes so far and its body's bytes past the header
    u32 audioBytes = 0;
    u8 audioProbe[AudioProbeBytes] = {};
    u32 audioProbeLength = 0;
    u8 audioHeader[AudioHeaderBytes] = {};
    u32 audioHeaderBytes = 0;
    std::vector<u8> audioBody;
    // Retail's staging, exactly: the last decoded picture (player->decoded) waits while fewer than two queue
    HostMovie::YuvFrame staging;
    bool stagingValid = false;
    HostMovie::YuvFrame queue[QueueSlots];
    u32 queued = 0;
    // The picture on screen (QueuePicture's), its CPU-converted RGBA, its checksum and its bgfx texture
    HostMovie::YuvFrame current;
    bool hasCurrent = false;
    std::vector<u8> rgba;
    u32 lastChecksum = 0;
    u32 presented = 0;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    u32 textureWidth = 0;
    u32 textureHeight = 0;
    u32 frames = 0;
    // The next present's scheduled time (WaitFrame paces to the movie's frame rate; the host loop is unpaced)
    std::chrono::steady_clock::time_point nextPresent{};
};
static_assert(sizeof(HostPlayer) <= Platform::Movie::PlayerSize, "host player must fit the game's storage");
HostPlayer* AsHost(Platform::Movie::Player* player)
{
    return reinterpret_cast<HostPlayer*>(player);
}

const HostPlayer* AsHost(const Platform::Movie::Player* player)
{
    return reinterpret_cast<const HostPlayer*>(player);
}

// Whether bgfx runs (a display's GL context exists): every GPU call goes through this, so the headless proof
// (no BgfxInit) still decodes, converts and checksums without touching bgfx
bool GpuReady()
{
    const bgfx::InternalData* data = bgfx::getInternalData();
    return data != nullptr && data->context != nullptr;
}

u32 Fnv1a(const u8* bytes, u32 length)
{
    u32 hash = 2166136261u;
    for (u32 i = 0; i < length; i++)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// The PSS's disc name ("\\FMV\\INTRO.PSS;1") to a host path: slashes as they are, the leading root and the ";1"
// suffix stripped (the null files layer then matches case-insensitively)
std::string HostMoviePath(const char* file)
{
    std::string path(file != nullptr ? file : "");
    u32 start = 0;
    while (start < path.size() && (path[start] == '\\' || path[start] == '/'))
    {
        start++;
    }
    if (start != 0)
    {
        path = path.substr(start);
    }
    const char* suffix = Platform::Movie::FileSuffix;
    u32 suffixLength = static_cast<u32>(std::strlen(suffix));
    if (path.size() >= suffixLength && path.compare(path.size() - suffixLength, suffixLength, suffix) == 0)
    {
        path.resize(path.size() - suffixLength);
    }
    return path;
}

// BT.601 studio swing to RGBA, clamped: the CPU convert QueuePicture uploads
void ConvertYuvToRgba(const HostMovie::YuvFrame& frame, std::vector<u8>* rgba)
{
    u32 width = frame.width;
    u32 height = frame.height;
    rgba->assign(static_cast<size_t>(width) * height * 4, 0);
    if (width == 0 || height == 0 || frame.pixels.size() < static_cast<size_t>(width) * height)
    {
        return;
    }
    const u8* yPlane = frame.pixels.data();
    size_t ySize = static_cast<size_t>(width) * height;
    size_t chromaSize = static_cast<size_t>(width / 2) * (height / 2);
    const u8* cbPlane = yPlane + ySize;
    const u8* crPlane = frame.pixels.size() >= ySize + chromaSize * 2 ? cbPlane + chromaSize : nullptr;
    u8* out = rgba->data();
    for (u32 y = 0; y < height; y++)
    {
        for (u32 x = 0; x < width; x++)
        {
            s32 c = static_cast<s32>(yPlane[static_cast<size_t>(y) * width + x]) - 16;
            s32 d = 0;
            s32 e = 0;
            if (crPlane != nullptr)
            {
                u32 chromaX = x / 2;
                u32 chromaY = y / 2;
                if (chromaX >= width / 2)
                {
                    chromaX = width / 2 - 1;
                }
                if (chromaY >= height / 2)
                {
                    chromaY = height / 2 - 1;
                }
                size_t chromaAt = static_cast<size_t>(chromaY) * (width / 2) + chromaX;
                d = static_cast<s32>(cbPlane[chromaAt]) - 128;
                e = static_cast<s32>(crPlane[chromaAt]) - 128;
            }
            s32 red = (298 * c + 409 * e + 128) >> 8;
            s32 green = (298 * c - 100 * d - 208 * e + 128) >> 8;
            s32 blue = (298 * c + 516 * d + 128) >> 8;
            size_t at = (static_cast<size_t>(y) * width + x) * 4;
            out[at] = static_cast<u8>(red < 0 ? 0 : red > 255 ? 255 : red);
            out[at + 1] = static_cast<u8>(green < 0 ? 0 : green > 255 ? 255 : green);
            out[at + 2] = static_cast<u8>(blue < 0 ? 0 : blue > 255 ? 255 : blue);
            out[at + 3] = 0xFF;
        }
    }
}

void DropTexture(HostPlayer* host)
{
    if (bgfx::isValid(host->texture))
    {
        if (GpuReady())
        {
            bgfx::destroy(host->texture);
        }
        host->texture = BGFX_INVALID_HANDLE;
    }
    host->textureWidth = 0;
    host->textureHeight = 0;
}

// The next display-order picture into staging; on stream end (or a short buffer) staging goes invalid and the
// movie is finished
void DecodeNext(HostPlayer* host)
{
    HostMovie::YuvFrame frame;
    if (host->decoder.NextFrame(&frame) == 1)
    {
        host->staging = frame;
        host->stagingValid = true;
        return;
    }
    host->stagingValid = false;
    host->finished = true;
}

// Retail's QueueDecoded: the staged picture waits while fewer than two queue
bool QueueDecoded(HostPlayer* host)
{
    if (!host->stagingValid || host->queued >= QueueSlots)
    {
        return false;
    }
    host->queue[host->queued] = host->staging;
    host->queued++;
    return true;
}

// Hands the selected channel's sound to the mixer: parses its header (rate,
// channels, interleave) and de-interleaves its body (blocks of interleave
// bytes alternating left, right, like the music banks') into stereo frames.
// Anything but 48 kHz stereo stays silent: every retail PSS is that.
void PushMoviePcm(HostPlayer* host)
{
    SdlAudio::MoviePcmStop();
    if (host->audioHeaderBytes < AudioHeaderBytes || host->audioBody.empty())
    {
        return;
    }

    auto field = [&](u32 at)
    {
        return static_cast<s32>(host->audioHeader[at]) | (static_cast<s32>(host->audioHeader[at + 1]) << 8) |
               (static_cast<s32>(host->audioHeader[at + 2]) << 16) | (static_cast<s32>(host->audioHeader[at + 3]) << 24);
    };
    s32 rate = field(AudioHeaderRate);
    s32 channels = field(AudioHeaderChannels);
    s32 interleave = field(AudioHeaderInterleave);
    if (rate != MoviePcmRate || channels != MoviePcmChannels || interleave <= 0)
    {
        return;
    }

    u32 blockSamples = static_cast<u32>(interleave) / 2;
    u32 blocks = static_cast<u32>(host->audioBody.size()) / static_cast<u32>(interleave);
    if (blocks < MoviePcmChannels)
    {
        return;
    }

    const auto* raw = reinterpret_cast<const s16*>(host->audioBody.data());
    std::vector<s16> stereo;
    stereo.reserve(static_cast<size_t>(blocks) / MoviePcmChannels * blockSamples * MoviePcmChannels);
    for (u32 pair = 0; pair + 1 < blocks; pair += 2)
    {
        const s16* leftBlock = raw + static_cast<size_t>(pair) * blockSamples;
        const s16* rightBlock = raw + static_cast<size_t>(pair + 1) * blockSamples;
        for (u32 sample = 0; sample < blockSamples; sample++)
        {
            stereo.push_back(leftBlock[sample]);
            stereo.push_back(rightBlock[sample]);
        }
    }
    if (!stereo.empty())
    {
        SdlAudio::MoviePcmOpen(static_cast<u32>(stereo.size()) / MoviePcmChannels, stereo.data());
    }
}
}

void Platform::Movie::Construct(Player* player)
{
    std::memset(player, 0, PlayerSize);
    new (player) HostPlayer();
}

void Platform::Movie::BeginPresenting(Player* player, void (*present)())
{
    HostPlayer* host = AsHost(player);
    host->present = present;
    host->queued = 0;
}

bool Platform::Movie::Open(Player* player, const char* file, u32 audioChannel, s32 width, void* lentMemory)
{
    (void)width;
    (void)lentMemory; // LentMemorySize semantics: the heap holds the stream, the lent pointer stays untouched
    HostPlayer* host = AsHost(player);
    if (host->opened)
    {
        Close(player);
    }
    std::string path = HostMoviePath(file);
    s32 descriptor = Platform::Files::Open(path.c_str(), Platform::Files::OpenRead);
    if (descriptor < 0)
    {
        return false;
    }
    s32 size = Platform::Files::Seek(descriptor, 0, Platform::Files::SeekEnd);
    Platform::Files::Seek(descriptor, 0, Platform::Files::SeekSet);
    if (size <= 0)
    {
        Platform::Files::Close(descriptor);
        return false;
    }
    host->fileBytes.assign(static_cast<size_t>(size), 0);
    size_t done = 0;
    while (done < host->fileBytes.size())
    {
        s32 want = static_cast<s32>(host->fileBytes.size() - done);
        if (want > 0x10000)
        {
            want = 0x10000;
        }
        s32 got = Platform::Files::Read(descriptor, host->fileBytes.data() + done, want);
        if (got <= 0)
        {
            break;
        }
        done += static_cast<size_t>(got);
    }
    Platform::Files::Close(descriptor);
    if (done != host->fileBytes.size())
    {
        host->fileBytes.clear();
        return false;
    }
    host->audioChannel = audioChannel;
    host->audioBytes = 0;
    host->audioProbeLength = 0;
    host->audioHeaderBytes = 0;
    host->audioBody.clear();
    host->decoder = HostMovie::Decoder();
    host->demux.reset(
        new HostMovie::Demux(host->fileBytes.data(), static_cast<u32>(host->fileBytes.size())));
    // Retail's channel: the game's audioChannel picks the PCM sub-stream (one
    // per language, FF A0 00 channel), 0 when the movie has one language
    host->demux->SelectAudioChannel(audioChannel);
    // The selected channel's stream: its 0x28 header (SShd then SSbd, like
    // retail's MovieAudioHeader) then its body. The header can span packets,
    // so it accumulates aside until complete, the body after it.
    while (!host->demux->Ended())
    {
        auto video = [&](const HostMovie::VideoPacket& packet)
        {
            host->decoder.Feed(packet.data, packet.length);
        };
        auto audio = [&](const u8* data, u32 length)
        {
            host->audioBytes += length;
            u32 take = length;
            if (take > AudioProbeBytes - host->audioProbeLength)
            {
                take = AudioProbeBytes - host->audioProbeLength;
            }
            if (take != 0)
            {
                std::memcpy(host->audioProbe + host->audioProbeLength, data, take);
                host->audioProbeLength += take;
            }
            u32 headerMissing = AudioHeaderBytes > host->audioHeaderBytes ? AudioHeaderBytes - host->audioHeaderBytes : 0;
            if (headerMissing != 0)
            {
                u32 headerTake = length < headerMissing ? length : headerMissing;
                std::memcpy(host->audioHeader + host->audioHeaderBytes, data, headerTake);
                host->audioHeaderBytes += headerTake;
                data += headerTake;
                length -= headerTake;
            }
            if (length != 0)
            {
                host->audioBody.insert(host->audioBody.end(), data, data + length);
            }
        };
        if (!host->demux->Next(video, audio))
        {
            break;
        }
    }
    PushMoviePcm(host);
    // Retail's two-picture prime: the first waits, the second stages
    host->stagingValid = false;
    host->queued = 0;
    host->finished = false;
    host->frames = 0;
    host->hasCurrent = false;
    host->nextPresent = std::chrono::steady_clock::now();
    DecodeNext(host);
    QueueDecoded(host);
    DecodeNext(host);
    host->opened = true;
    return true;
}

void Platform::Movie::StartSound(Player* player, f32 volume)
{
    (void)player;
    // Retail loops its filled ring from here with the movie group's volume
    SdlAudio::MoviePcmStart(volume);
}

bool Platform::Movie::Step(Player* player)
{
    HostPlayer* host = AsHost(player);
    if (!host->opened || host->finished)
    {
        return false;
    }
    if (QueueDecoded(host))
    {
        DecodeNext(host);
        host->frames++;
    }
    return true;
}

void Platform::Movie::WaitFrame(Player* player)
{
    // Retail's vertical blank presents every second one (the handler's g_BlankToggle), so a picture shows every
    // two TV fields; WaitFrame waits for that present while reading on. The host has no vertical blank (the frame
    // loop is unpaced), so pace the presents to the movie's own frame rate, else the whole loop runs many times a
    // frame and the movie plays at several times its speed. Headless (no presenter) nothing blocks.
    HostPlayer* host = AsHost(player);
    if (host->present == nullptr)
    {
        return;
    }
    // frame_rate_code (6.2.2.1): 1 = 23.976, 2 = 24, 3 = 25, 4 = 29.97, 5 = 30, 6 = 50, 7 = 59.94, 8 = 60
    static constexpr double Rates[] = {0.0,          24000.0 / 1001.0, 24.0,          25.0, 30000.0 / 1001.0,
                                       30.0,         50.0,            60000.0 / 1001.0, 60.0};
    u32 code = host->decoder.FrameRateCode();
    double rate = code < sizeof(Rates) / sizeof(Rates[0]) && Rates[code] > 0.0 ? Rates[code] : 25.0;
    host->nextPresent += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / rate));
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (host->nextPresent > now)
    {
        std::this_thread::sleep_for(host->nextPresent - now);
    }
    else
    {
        // Fallen behind (a stall): start the schedule again from now rather than racing to catch up
        host->nextPresent = now;
    }
    host->present();
}

void Platform::Movie::QueuePicture(Player* player)
{
    HostPlayer* host = AsHost(player);
    if (host->queued == 0)
    {
        return;
    }
    host->current = host->queue[0];
    host->hasCurrent = true;
    if (host->queued > 1)
    {
        host->queue[0] = host->queue[1];
    }
    host->queued--;
    ConvertYuvToRgba(host->current, &host->rgba);
    host->lastChecksum = Fnv1a(host->rgba.data(), static_cast<u32>(host->rgba.size()));
    host->presented++;
    if (!GpuReady() || host->current.width == 0 || host->current.height == 0)
    {
        return;
    }
    if (!bgfx::isValid(host->texture) || host->textureWidth != host->current.width ||
        host->textureHeight != host->current.height)
    {
        DropTexture(host);
        host->texture = bgfx::createTexture2D(static_cast<u16>(host->current.width),
                                              static_cast<u16>(host->current.height), false, 1,
                                              bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, nullptr);
        host->textureWidth = host->current.width;
        host->textureHeight = host->current.height;
    }
    if (bgfx::isValid(host->texture))
    {
        const bgfx::Memory* memory = bgfx::copy(host->rgba.data(), static_cast<u32>(host->rgba.size()));
        u16 pitch = static_cast<u16>(host->current.width * 4);
        bgfx::updateTexture2D(host->texture, 0, 0, 0, 0, static_cast<u16>(host->current.width),
                              static_cast<u16>(host->current.height), memory, pitch);
    }
}

void Platform::Movie::Draw(Player* player, const Area& area, s32 width, s32 height, u32 skippedRows)
{
    HostPlayer* host = AsHost(player);
    // Nothing until two pictures decoded since Open, nothing without a queued picture or a display
    if (host->frames < 2 || !host->hasCurrent || !GpuReady() || !bgfx::isValid(host->texture))
    {
        return;
    }
    // The area in screen fractions: the game's area is in the display's pixels (MoviePlayer::Draw measures it from
    // g_DisplayWidth/Height), which the frame start reported to HostSetDisplaySize. skippedRows crops the picture's
    // top and bottom, not the area
    f32 displayWidth = static_cast<f32>(Platform::Graphics::HostDisplayWidth());
    f32 displayHeight = static_cast<f32>(Platform::Graphics::HostDisplayHeight());
    if (displayWidth <= 0.0f || displayHeight <= 0.0f)
    {
        return;
    }
    f32 left = static_cast<f32>(area.x) / displayWidth;
    f32 top = static_cast<f32>(area.y) / displayHeight;
    f32 right = static_cast<f32>(area.x + area.width) / displayWidth;
    f32 bottom = static_cast<f32>(area.y + area.height) / displayHeight;
    f32 pictureHeight = height != 0 ? static_cast<f32>(height) : static_cast<f32>(host->current.height);
    f32 vTop = (static_cast<f32>(skippedRows) + 0.5f) / pictureHeight;
    f32 vBottom = (pictureHeight - static_cast<f32>(skippedRows) - 0.5f) / pictureHeight;
    (void)width;
    const MovieVertex vertices[4] = {
        MovieCorner(left, top, 0.0f, vTop),
        MovieCorner(left, bottom, 0.0f, vBottom),
        MovieCorner(right, top, 1.0f, vTop),
        MovieCorner(right, bottom, 1.0f, vBottom),
    };
    if (!bgfx::getAvailTransientVertexBuffer(4, MovieLayout()))
    {
        return;
    }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 4, MovieLayout());
    auto* at = reinterpret_cast<MovieVertex*>(buffer.data);
    for (u32 vertex = 0; vertex < 4; vertex++)
    {
        at[vertex] = vertices[vertex];
    }
    // The TS_NO switch: the movie kept off the frame
    if (Platform::Graphics::HostStageDisabled("movie"))
    {
        return;
    }

    bgfx::setViewMode(MovieView, bgfx::ViewMode::Sequential);
    // The movie lands on the scene target, like every other pass: the composite at the frame's end copies the whole
    // frame (the PS2 draws everything into one frame buffer), so drawing to the backbuffer would be covered
    Platform::Graphics::HostSceneSetView(MovieView);
    // The bars around a movie are black (a 4:3 movie on a 16:9 TV, or a wide one on a 4:3 TV): the picture alone doesn't cover
    // the frame, and what's under it is the frame's debug clear colour
    bgfx::setViewClear(MovieView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA |
                   BGFX_STATE_PT_TRISTRIP);
    bgfx::setTexture(0, Platform::Graphics::HostDraw2DSampler(), host->texture);
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(MovieView, Platform::Graphics::HostDraw2DProgram());
}

void Platform::Movie::Close(Player* player)
{
    SdlAudio::MoviePcmStop();
    HostPlayer* host = AsHost(player);
    DropTexture(host);
    host->demux.reset();
    host->fileBytes.clear();
    host->fileBytes.shrink_to_fit();
    host->audioBody.clear();
    host->audioBody.shrink_to_fit();
    host->audioHeaderBytes = 0;
    host->rgba.clear();
    host->rgba.shrink_to_fit();
    host->opened = false;
    host->finished = false;
    host->stagingValid = false;
    host->queued = 0;
    host->hasCurrent = false;
    host->frames = 0;
}

u32 HostMovieDebug_LastChecksum(const Platform::Movie::Player* player)
{
    return AsHost(player)->lastChecksum;
}

u32 HostMovieDebug_LastYuvChecksum(const Platform::Movie::Player* player)
{
    const HostPlayer* host = AsHost(player);
    if (!host->hasCurrent || host->current.pixels.empty())
    {
        return 0;
    }
    return Fnv1a(host->current.pixels.data(), static_cast<u32>(host->current.pixels.size()));
}

u32 HostMovieDebug_Presented(const Platform::Movie::Player* player)
{
    return AsHost(player)->presented;
}

u32 HostMovieDebug_AudioBytes(const Platform::Movie::Player* player)
{
    return AsHost(player)->audioBytes;
}
