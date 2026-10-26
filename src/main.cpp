#include "common.h"
#include "gcc2.h"
#include "game/context.h"
#include "game/controllers.h"
#include "game/filestream.h"
#include "game/stream.h"
#include "game/memory.h"
#include "game/movie.h"
#include "game/particles.h"
#include "game/string.h"
#include "platform/graphics.h"
#include "platform/system.h"
#include "platform/time.h"
#include "retail/libc.h"

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
}

extern "C"
{
    // "rb batch=Crash6\Crash": the arguments the game takes when it's given none. ParseArguments cuts the words out of it
    extern char* g_DefaultArguments RETAIL(D_00309884);
}

extern "C" int Main(u32 argc, char** argv)
{
    RunStaticConstructors();
    Platform::System::Initialise();
    // The disc is PAL (SLES-52568, disc/SYSTEM.CNF's VMODE), and the game's own flag for it is a word of the retail
    // data (D_00309AEC, 1 in the PAL build). The host has no SYSTEM.CNF reader and never writes the flag, so it stayed
    // at the zero of the process's memory and the whole game ran at 60 Hz: the renderer and the clock below took NTSC,
    // the particle emitters aged 1/60 s a frame (particles.cpp's NtscFrame) and every per-frame formula was evaluated
    // 1.2x less often than retail did. Set it from the disc the build carries the data of
    g_Pal = true;
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
    while (g_GameState != GameStateQuitting)
    {
        // The frame's own boundary: retail's frame IS the vblank (the PS2 build waits for one, sceGsSyncV) and the whole
        // game is written for that step - the particle emitters age a fiftieth of a second a frame, the formula a collected
        // pickup appeals to is linear in the frame's elapsed time, the movers' state machines advance one step a frame.
        // The host renders as fast as the GPU allows, and this loop is where its frames begin, so it waits the frame's turn
        // out here. (The pads' own call to this is in their setup loop, not their per-frame path: it cannot pace anything.)
        Platform::Graphics::WaitVSync();

#ifndef __mips__
        // Presenting is the display's rate and running the game is the disc's: a display frame that falls between two of
        // the game's steps has nothing new to draw, so it is skipped and the window keeps the last picture. This is where
        // the two rates part, and it is why the logic does not run 1.2x fast on a 60 Hz monitor
        if (!Platform::Time::LogicStepDue())
        {
            continue;
        }

        // The logic boundary, exactly one field on from the last: the game's frameTime is this step and nothing else, so
        // nothing the game counts per frame inherits the display's rate
        Platform::Time::AdvanceFrame();

        // The particle system ages by one global step an update, which retail fixes to the disc's field (particles.cpp's
        // PalFrame/NtscFrame). That is the logic's step, so the aging follows the disc and not the display
        const s32 logicRate = Platform::Time::FrameRate();
        if (logicRate > 0)
        {
            g_ParticleFrameDelta = 1.0f / static_cast<f32>(logicRate);
        }
#endif

        // Each step asks again, the movie can start or end in between
        GameMovieController* movie = G_GameMovieController;
        context->BeginFrame(IsPlayingMovie(movie));
        context->Update(IsPlayingMovie(movie));
        context->EndFrame(IsPlayingMovie(movie));
        context->Render(IsPlayingMovie(movie));
    }

    return 0;
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
