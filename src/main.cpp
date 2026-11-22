#include "common.h"
#include "gcc2.h"
#include "game/context.h"
#include "game/controllers.h"
#include "game/filestream.h"
#include "platform/hostframe.h"
#include "game/instances.h"
#include "game/stream.h"
#include "game/memory.h"
#include "game/movie.h"
#include "game/particles.h"
#include "game/string.h"
#include "platform/graphics.h"
#include "platform/system.h"
#include "platform/files.h"
#include "platform/time.h"
#include "retail/libc.h"

#ifndef __mips__
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace
{
constexpr u32 MaxArguments = 256;
// The file streams' channels
constexpr s32 StreamChannels = 9;
// The game's clock goes by the display's frames: 50 a second on a PAL TV, 60 on an NTSC one
constexpr s32 PalFramesPerSecond = 50;
constexpr s32 NtscFramesPerSecond = 60;
// One pad, so pad 1's bit (of GamePadController's bit per pad made) is never set
constexpr s32 PadCount = 1;
constexpr u32 SecondPadBit = 1u << 1;

bool IsPlayingMovie(const GameMovieController* movie)
{
    return movie != nullptr && movie->IsPlaying();
}

#ifndef __mips__
// Where a host frame's time goes, printed with the frame log once a second: the game's step (update and end), its draw, the
// wait for the display's turn, and a redraw between two steps. A load's worst frame is then read beside the page's own disc
// line, which says how much of the step was the disc
struct FramePhases
{
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    double logic = 0.0;
    double draw = 0.0;
    double present = 0.0;
    double redraw = 0.0;
    double worstLogic = 0.0;
    double worstDraw = 0.0;
    u32 frames = 0;
};
FramePhases g_Phases;

double PhaseMs(std::chrono::steady_clock::time_point at)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count();
}

void CountFrame(double logic, double draw, double present, double redraw)
{
    g_Phases.frames++;
    g_Phases.logic += logic;
    g_Phases.draw += draw;
    g_Phases.present += present;
    g_Phases.redraw += redraw;
    g_Phases.worstLogic = logic > g_Phases.worstLogic ? logic : g_Phases.worstLogic;
    g_Phases.worstDraw = draw > g_Phases.worstDraw ? draw : g_Phases.worstDraw;

    static const bool logged = [] {
        const char* value = std::getenv("TWINSANITY_FRAME_LOG");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    if (!logged)
    {
        return;
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_Phases.since).count();
    if (elapsed < 1000)
    {
        return;
    }

    const double frames = static_cast<double>(g_Phases.frames);
    std::fprintf(stderr,
                 "host phases: %.2f logic %.2f draw %.2f present %.2f redraw ms a frame, worst %.2f logic %.2f draw, over %u "
                 "frames\n",
                 g_Phases.logic / frames, g_Phases.draw / frames, g_Phases.present / frames, g_Phases.redraw / frames,
                 g_Phases.worstLogic, g_Phases.worstDraw, g_Phases.frames);
    g_Phases = FramePhases();
}
#endif
}

extern "C"
{
    // "rb batch=Crash6\Crash": the arguments the game takes when it's given none. ParseArguments cuts the words out of it
    extern char* g_DefaultArguments RETAIL(D_00309884);
}

namespace
{
// One display frame: the picture's boundary, then either a step of the game or (on the hosts, between two steps) a redraw of
// the last step eased towards the next. The console's frame is its field, so there it is one step a call. Returns whether the
// run is still going
bool GameFrame(GameContext* context, bool& drawn)
{
#ifndef __mips__
    const std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
#endif
    Platform::Graphics::WaitVSync();
    const bool playingMovie = IsPlayingMovie(G_GameMovieController);

#ifndef __mips__
    const double present = PhaseMs(frameStart);
    if (!Platform::Time::LogicStepDue())
    {
        double redraw = 0.0;
        if (drawn && !playingMovie)
        {
            const std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
            HostRedrawFrame(Platform::Time::LogicAlpha());
            redraw = PhaseMs(at);
        }
        CountFrame(0.0, 0.0, present, redraw);
        return g_GameState != GameStateQuitting;
    }

    Platform::Time::AdvanceFrame();
    HostFrame::SetAlpha(0.0f);
    HostFrame::SetRedraw(false);
    HostFrame::BeginLogicStep();
    Platform::Graphics::BeginLogicFrame();
    drawn = true;

    const s32 logicRate = Platform::Time::FrameRate();
    if (logicRate > 0)
    {
        g_ParticleFrameDelta = 1.0f / static_cast<f32>(logicRate);
    }
#else
    (void)drawn;
#endif

#ifndef __mips__
    const std::chrono::steady_clock::time_point stepStart = std::chrono::steady_clock::now();
#endif
    context->BeginFrame(playingMovie);
    context->Update(playingMovie);
    context->EndFrame(playingMovie);
#ifndef __mips__
    const double logic = PhaseMs(stepStart);
    const std::chrono::steady_clock::time_point drawStart = std::chrono::steady_clock::now();
#endif
    context->Render(playingMovie);
#ifndef __mips__
    CountFrame(logic, PhaseMs(drawStart), present, 0.0);
#endif
    return g_GameState != GameStateQuitting;
}
}

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cstdio>
namespace
{
GameContext* g_WebContext = nullptr;
bool g_WebDrawn = false;

// The page's frame: the browser calls back once a display frame, because the run cannot be left blocking in a loop there
void WebFrame()
{
    // A page's frame is one the browser calls back: saying so now and then is how a run's log shows it is drawing rather
    // than stopped in its start-up
    static u32 frames = 0;
    if (frames % 120 == 0)
    {
        std::fprintf(stderr, "web: frame %u\n", frames);
    }
    frames++;

    if (!GameFrame(g_WebContext, g_WebDrawn))
    {
        emscripten_cancel_main_loop();
    }
}
}
#endif

extern "C" int Main(u32 argc, char** argv)
{
    RunStaticConstructors();
    Platform::System::Initialise();
    // The game's own flag for the disc's field rate: a word of the retail data (D_00309AEC, 1 in the European build and 0 in
    // the others). The retail start-up never wrote it because the data carried the value; this port's data is the European
    // build's, so the disc being played is what decides: 50 fields a second on the European release, 60 on the North American
    // and Japanese ones. It reaches the renderer's PAL framing, the clock's rate and the particle emitters' frame (1/50 s
    // against particles.cpp's PalFrame, 1/60 against NtscFrame)
    g_Pal = Platform::Files::DiscIsPal();
    GameContext* context = ParseArguments(argc, argv);
    Platform::System::StartServices();
    InitStreamSystem(StreamChannels);
    CreateMusic();
    UpdateStreamSystem(g_StreamSystem);

    if (G_GameRendererController == nullptr)
    {
        void* memory = MemoryAllocate(sizeof(GameRendererController));
        G_GameRendererController = GameRendererController::Construct(memory, g_ScreenWidth, g_ScreenHeight, g_Pal);
    }

    if (G_GameClockController == nullptr)
    {
        // The game's own field rate: the disc's, 50 on PAL, on every platform. The display's rate never reaches this
        s32 framesPerSecond = g_Pal ? PalFramesPerSecond : NtscFramesPerSecond;
#ifndef __mips__
        framesPerSecond = Platform::Time::FrameRate();
#endif
        G_GameClockController = GameTimeConstruct(static_cast<GameTimeController*>(MemoryAllocate(sizeof(GameTimeController))),
                                                  framesPerSecond);
    }

    if (G_GamePadController == nullptr)
    {
        GamePadController* pads = GamePadController::Construct(MemoryAllocate(sizeof(GamePadController)), PadCount);
        pads->flags.pads &= ~SecondPadBit;
        G_GamePadController = pads;
    }

    context->StartUp();
    // Whether a step has drawn its own frame: the display frames before the first step have nothing to show yet
    bool drawn = false;
#if defined(__EMSCRIPTEN__)
    std::fprintf(stderr, "web: start-up done, the browser drives the frames\n");
    g_WebContext = context;
    g_WebDrawn = false;
    emscripten_set_main_loop(WebFrame, 0, true);
    return 0;
#else
    while (g_GameState != GameStateQuitting)
    {
        // The frame's own boundary: retail's frame IS the vblank (the PS2 build waits for one, sceGsSyncV) and the whole
        // game is written for that step - the particle emitters age a fiftieth of a second a frame, the formula a collected
        // pickup appeals to is linear in the frame's elapsed time, the movers' state machines advance one step a frame.
        // The host renders as fast as the GPU allows, and this loop is where its frames begin, so it waits the frame's turn
        // out here. (The pads' own call to this is in their setup loop, not their per-frame path: it cannot pace anything.)
        Platform::Graphics::WaitVSync();

        GameMovieController* movie = G_GameMovieController;
        bool playingMovie = IsPlayingMovie(movie);

#ifndef __mips__
        // Presenting is the player's rate (settings.txt's `render`) and running the game is the disc's. A display frame
        // between two steps has no new game state of its own: it draws the last step's state eased towards the next, so
        // the picture moves at the display's rate while the logic keeps the field's. While a movie plays it presents
        // itself, at its own rate, and repeats nothing
        if (!Platform::Time::LogicStepDue())
        {
            if (drawn && !playingMovie)
            {
                HostRedrawFrame(Platform::Time::LogicAlpha());
            }

            continue;
        }

        // The logic boundary, exactly one field on from the last: the game's frameTime is this step and nothing else, so
        // nothing the game counts per frame inherits the display's rate
        Platform::Time::AdvanceFrame();
        // A step's own frame draws the stepped state, not an ease of it
        HostFrame::SetAlpha(0.0f);
        HostFrame::SetRedraw(false);
        HostFrame::BeginLogicStep();
        // The step before's frame memory is only now done with: its extra display frames redrew what it made (the joint
        // matrices the models draw with), so this is where the frame's memory starts over
        Platform::Graphics::BeginLogicFrame();
        drawn = true;

        // The particle system ages by one global step an update, which retail fixes to the disc's field (particles.cpp's
        // PalFrame/NtscFrame). That is the logic's step, so the aging follows the disc and not the display
        const s32 logicRate = Platform::Time::FrameRate();
        if (logicRate > 0)
        {
            g_ParticleFrameDelta = 1.0f / static_cast<f32>(logicRate);
        }
#endif

        // Each step asks again, the movie can start or end in between
        context->BeginFrame(playingMovie);
        context->Update(playingMovie);
        context->EndFrame(playingMovie);
        context->Render(playingMovie);
    }

    return 0;
#endif
}

// The launch arguments, made into the game's context once
extern "C" GameContext* ParseArguments(u32 argc, char** argv)
{
    if (g_GameContext != nullptr)
    {
        return g_GameContext;
    }

    char* words[MaxArguments];
    u32 count = 0;
    if (argc < 2)
    {
        // The game's own arguments, split at white space in place. Like a program's name, words[0] is left out. The file
        // made around it is never opened: what's left of reading them from one
        File file;
        File::Construct(&file);
#ifdef __mips__
        char* text = g_DefaultArguments;
#else
        // The retail string lives in read-only .rodata on the host (and the split is authoritative): splitting writes
        // terminators into it, so copy it to a writable buffer first
        static char g_HostDefaultArguments[512];
        char* text = g_HostDefaultArguments;
        for (u32 i = 0; i < sizeof(g_HostDefaultArguments) - 1 && g_DefaultArguments[i] != '\0'; i++)
        {
            text[i] = g_DefaultArguments[i];
            text[i + 1] = '\0';
        }
#endif
        u32 length = RetailLibc::StringLength(text);
        u32 wordStart = 0;
        bool betweenWords = true;
        count = 1;
        for (u32 i = 0; i < length; i++)
        {
            char character = text[i];
            if (character == '\n' || character == '\r' || character == '\t' || character == ' ')
            {
                if (!betweenWords)
                {
                    text[i] = '\0';
                    words[count++] = text + wordStart;
                }

                wordStart = i + 1;
                betweenWords = true;
            }
            else
            {
                betweenWords = false;
            }
        }

        if (!betweenWords)
        {
            words[count++] = text + wordStart;
        }

        file.Destroy(DestroyOnly);
    }
    else
    {
        do
        {
            words[count] = argv[count];
            count++;
        } while (count < argc && count < MaxArguments);
    }

    g_GameContext = CreateGameContext(count, words);
    return g_GameContext;
}
