#include "game/cutscenereader.h"

#include "game/animation.h"
#include "game/chunkloading.h"
#include "game/clock.h"
#include "game/colour.h"
#include "game/controllers.h"
#include "game/instanceparticles.h"
#include "game/instances.h"
#include "game/math.h"
#include "game/memory.h"
#include "game/objectnode.h"
#include "game/gamecontroller.h"
#include "game/language.h"
#include "game/overlay.h"
#include "game/porttext.h"
#include "game/place.h"
#include "game/renderer.h"
#include "game/sound.h"

#include "platform/system.h"

#include <cstdio>
#include <cstring>

// The video controller around the cutscenes' playing (game/cutscenes.h): cutscenes and music-only ones queued, started and stopped
// for the scripts, the parts read handed over, its state's update, its tracks made, played a frame at a time and let go of, and the
// instances it was given for models

extern "C"
{
    // A vector's 16 bytes copied (one quadword)
    Vector4* CopyQuadword(Vector4* to, const Vector4* from) RETAIL(MovePositionFromPos2ToPos1);
}

namespace
{
// The track a queued cutscene asks for (its music plays in the context music's slot, in the movie group)
constexpr u32 QueuedTrack = 1;
// The skip hint and the disc error's title: their colours (the colour table's), size and places (the hint's further left)
constexpr ColourIndex SkipHintColour = ColourYellow;
constexpr ColourIndex DiscErrorColour = ColourMagenta;
constexpr f32 TextScale = 0.5f;
constexpr f32 SkipHintX = Rounded(0.83);
constexpr f32 DiscErrorX = Rounded(0.85);
constexpr f32 CutsceneTextY = Rounded(0.12);
// The scripted scenes' hint: the bottom right corner of the picture. The text is anchored by that corner (BottomRight), so a word in a
// language with wide glyphs runs left across the picture instead of off its edge, and every language ends the same distance from it
constexpr f32 ScriptedHintX = Rounded(0.95);
constexpr f32 ScriptedHintY = Rounded(0.9);

// A track's place made the origin's (its w kept)
void PlaceAtOrigin(Vector4* position)
{
    f32 w = position->w;
    CopyQuadword(position, &g_DefaultBox.min);
    position->w = w;
}

const Matrix4x4* TrackSpace(VideoController* controller)
{
    return controller->bits.relative != 0 ? &controller->origin : nullptr;
}
}

u32 QueueObjectMovie(VideoController* controller, GameObject* object, InstanceContext* instance, s32 number)
{
    controller->object = object;
    controller->currentPart = -1;
    controller->instance = instance;
    controller->readPart = 0;
    ObjectPlace* place = instance->place;
    RotateAndTranslate(place);
    controller->origin = place->matrix;
    controller->bits.relative = 1;
    controller->chunk = instance->chunk;
    Cutscene* cutscene = ConstructCutscene(static_cast<Cutscene*>(MemoryAllocate(sizeof(Cutscene))));
    controller->cutscene = cutscene;
    LoadCutscenePart(controller, cutscene, number, static_cast<s32>(controller->readPart));
    controller->bits.firstPartRead = 0;
    controller->bits.nextPartRead = 0;
    controller->bits.waitingForMusic = 0;
    controller->bits.stopped = 0;
    controller->bits.musicOnly = 0;
    controller->state = VideoController::StateQueued;
    // Retail bug: the request's bits 20-31 are what the stack held, its loop bit (20) among them: the music loops or not by
    // chance (here it doesn't)
    MusicRequest request;
    request.bits.value = 0;
    request.bits.track = QueuedTrack;
    request.bits.group = MovieGroup;
    request.left = 1.0f;
    request.right = 1.0f;
    request.fadeTime = 0.0f;
    PlayMusicRequest(ContextMusicSlot, &request);
    return 1;
}

u32 QueueMovie(VideoController* controller, s32 track, TimeClock* clock, u32 loops)
{
    // The request's bits 21-31 are what the stack held in retail (the music doesn't read them)
    MusicRequest request;
    request.bits.value = 0;
    request.bits.track = static_cast<u32>(track);
    request.bits.group = MovieGroup;
    request.bits.loops = loops;
    request.left = 1.0f;
    request.right = 1.0f;
    request.fadeTime = 0.0f;
    u32 asked = static_cast<u32>(PlayMusicRequest(ContextMusicSlot, &request)) & 1;
    controller->bits.musicAsked = asked != 0;
    if (asked != 0)
    {
        controller->bits.musicOnly = 1;
        controller->bits.waitingForMusic = 0;
        controller->state = VideoController::StateQueued;
        controller->clock = clock;
    }

    return asked;
}

u32 VideoReady(VideoController* controller)
{
    if (controller->bits.musicOnly == 0)
    {
        return 0;
    }

    return MusicSlotPrepared(ContextMusicSlot);
}

u32 StartQueuedMovie(VideoController* controller, TimeClock* clock)
{
    if (controller->bits.musicAsked == 0)
    {
        return 0;
    }

    if (MusicSlotPrepared(ContextMusicSlot) == 0)
    {
        controller->bits.waitingForMusic = 1;
        controller->waitedFrames++;
        return 0;
    }

    controller->clock = clock;
    controller->bits.waitingForMusic = 0;
    controller->startTime = clock->time;
    controller->frameShare = 0.0f;
    controller->partFrame = 0;
    controller->frame = 0;
    controller->part = 0;
    controller->now = clock->time;
    PlayPreparedMusic(1.0f, 1.0f, 0.0f, ContextMusicSlot);
    controller->bits.musicAsked = 0;
    controller->state = VideoController::StatePlaying;
    return 1;
}

u32 CutsceneSeen(VideoController* controller)
{
    auto* node = static_cast<ObjectNodeBase*>(GetGameNode(&controller->instance->nodes, NodeObject));
    ChunkEntry* chunk = ChunkOfIndex(g_ChunkManager, node->agent->chunkIndex);
    return GetPersistentFlag(chunk->savedFlags, node->agent->id);
}

void StopScriptMovie(VideoController* controller)
{
    controller->bits.stopped = 1;
    if (controller->state == VideoController::StateReady)
    {
        if (controller->cutscene != nullptr)
        {
            DestroyCutscene(controller->cutscene, DestroyAndFree);
            controller->cutscene = nullptr;
        }

        if (controller->nextPart != nullptr)
        {
            DestroyCutscene(controller->nextPart, DestroyAndFree);
            controller->nextPart = nullptr;
        }

        controller->state = VideoController::StateIdle;
    }

    StopCutsceneMusic(controller);
    controller->waitedFrames = 0;
}

void CutscenePartRead(VideoController* controller, Cutscene* cutscene)
{
    u32 state = controller->state;
    if (state == VideoController::StateIdle || state == VideoController::StateFinished ||
        controller->bits.stopped != 0)
    {
        if (controller->cutscene != nullptr)
        {
            DestroyCutscene(controller->cutscene, DestroyAndFree);
            controller->cutscene = nullptr;
        }

        Cutscene* next = controller->nextPart;
        if (next != nullptr && cutscene == next && controller->state == VideoController::StateQueued)
        {
            DestroyCutscene(cutscene, DestroyAndFree);
            controller->nextPart = nullptr;
        }

        controller->waitedFrames = 0;
        controller->state = VideoController::StateIdle;
        return;
    }

    if (cutscene == controller->cutscene)
    {
        controller->bits.firstPartRead = 1;
        controller->state = VideoController::StateReady;
        if (controller->bits.waitingForMusic != 0)
        {
            StartReadCutscene(controller, GetContextClock(controller->instance));
        }

        controller->waitedFrames = 0;
        return;
    }

    if (cutscene == controller->nextPart)
    {
        controller->waitedFrames = 0;
        controller->bits.nextPartRead = 1;
    }
}

void StopCutsceneMusic(VideoController*)
{
    // The cutscene's music let go of (retail's own: the slot fades over the shortest fade and stops). The report that had
    // emptied this body blamed a deadlock, but a stopped player is handed to the next request (FirstStoppedPlayer), so the
    // slot is reusable: the music the level plays is the main slot's, untouched here
    FadeOutMusicSlot(0.0f, ContextMusicSlot);
}

InstanceContext* ModelInstance(VideoController* controller, u16 model)
{
    for (u32 index = 0; index < controller->modelCount; index++)
    {
        if (controller->models[index] == model)
        {
            return controller->modelInstances[index];
        }
    }

    return nullptr;
}

void SetVideoModelInstance(VideoController* controller, u32 model, InstanceContext* instance)
{
    u16 id = static_cast<u16>(model);
    bool found = false;
    for (u32 index = 0; index < controller->modelCount; index++)
    {
        if (controller->models[index] == id)
        {
            controller->modelInstances[index] = instance;
            found = true;
        }
    }

    if (!found && controller->modelCount < VideoController::ModelCount)
    {
        controller->models[controller->modelCount] = id;
        controller->modelInstances[controller->modelCount] = instance;
        controller->modelCount++;
    }
}

void GiveBackInstances(VideoController* controller)
{
    for (u32 index = 0; index < controller->cutscene->instanceTrackCount; index++)
    {
        PlayedInstanceTrack* track = &controller->instanceTracks[index];
        if (track->bits.given != 0)
        {
            GiveBackInstance(controller, track->instance);
        }
    }
}

void GiveBackInstance(VideoController*, InstanceContext* instance)
{
    Agent* agent = static_cast<ObjectNodeBase*>(GetGameNode(&instance->nodes, NodeObject))->agent;
    CallVirtual<void>(agent, agent->vtable, Agent::UnfreezeSlot);
    auto* model = static_cast<ModelNode*>(GetGameNode(&instance->nodes, NodeModel));
    if (model->animator != nullptr)
    {
        StopOgiAnimation(model->animator, 0, OgiAnimator::RootJoint);
    }
}

void DrawSkipHint(VideoController* controller)
{
    Renderer* renderer = G_Renderer_;
    renderer->textScale.y = TextScale;
    renderer->textScale.x = TextScale;
    u32 colour;
    GetColor(&colour, SkipHintColour);
    renderer->colour = colour;
    QueueText(renderer, controller->skipHint.string, SkipHintX, CutsceneTextY);
}

// The cross button's character. It is 0x5C in the game's font on every disc: the European and American select prompts end with it,
// and so does the Japanese back prompt (a Japanese game confirms with the circle, so its select prompt ends with the circle's 0x5D
// instead: the cross is still 0x5C there, which is what a skip wants)
constexpr char CrossGlyph = 0x5C;

// The hint a scripted cutscene gets, queued on the frames that should show it: the video controller is not playing a cutscene file,
// skipping is on, and the game is watching. Queued once a step (GameController::Watching) as the file's hint is and again by every
// display frame between the steps (HostQueueCutsceneHints), so it holds still instead of blinking on every other frame
void QueueScriptedSkipHint()
{
    // A cutscene file playing is its own hint's business (DrawSkipHint); a scripted scene is watched with the video controller idle, or
    // playing its music only, so that bit says nothing about whether a scene is on. The hint waits for a script of the scene to ask
    // condition 572: only a scene that carries a skip of its own asks, so a scene without one is never offered a skip it cannot take
    // (the scripts ask every step while their loop is in it, so on a scene that has a skip the hint is there from the first step)
    if (G_VideoController == nullptr ||
        (G_VideoController->state == VideoController::StatePlaying && G_VideoController->cutscene != nullptr) ||
        !Platform::System::CutsceneSkip() || G_GameController == nullptr ||
        G_GameController->State() != GameController::StateWatching || CutsceneSkipAsked() == 0)
    {
        return;
    }

    DrawScriptedSkipHint();
}

void DrawScriptedSkipHint()
{
    Renderer* renderer = G_Renderer_;
    renderer->textScale.y = TextScale;
    renderer->textScale.x = TextScale;
    u32 colour;
    GetColor(&colour, SkipHintColour);
    renderer->colour = colour;
    renderer->textAlignment.value = TextAlignment::BottomRight;
    char hint[64];
    std::snprintf(hint, sizeof(hint), "%c  %s", CrossGlyph, PortTextString(PortTextSkip));
    QueueText(renderer, hint, ScriptedHintX, ScriptedHintY);
}

void DrawDiscError(VideoController* controller)
{
    Renderer* renderer = G_Renderer_;
    renderer->textScale.y = TextScale;
    renderer->textScale.x = TextScale;
    u32 colour;
    GetColor(&colour, DiscErrorColour);
    renderer->colour = colour;
    QueueText(renderer, controller->discErrorTitle.string, DiscErrorX, CutsceneTextY);
}

void VideoController::Pause()
{
    pausedState = state;
    state = StatePaused;
}

void VideoController::Resume()
{
    state = pausedState;
}

void VideoController::Update()
{
    switch (state)
    {
    case StateIdle:
        UpdateIdleCutscene(this);
        break;
    case StateQueued:
        UpdateQueuedCutscene(this);
        break;
    case StateReady:
        UpdateReadyCutscene(this);
        break;
    case StatePlaying:
        UpdatePlayingCutscene(this);
        break;
    case StatePaused:
        UpdatePausedCutscene(this);
        break;
    case StateFinished:
        UpdateFinishedCutscene(this);
        break;
    }
}

void VideoController::Reset()
{
    StopCutscene(this);
    bits.musicOnly = 0;
    state = StateIdle;
    currentPart = -1;
    pausedState = StateIdle;
    readPart = 0;
}

void UpdateIdleCutscene(VideoController*)
{
}

void UpdateReadyCutscene(VideoController*)
{
}

void UpdatePausedCutscene(VideoController* controller)
{
    TimeClock* clock = controller->clock;
    if (clock != nullptr && clock->flags.running == 0)
    {
        controller->state = controller->pausedState;
    }
}

void UpdateFinishedCutscene(VideoController*)
{
}

void PlayInstanceTracks(VideoController* controller)
{
    if (controller->cutscene == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < controller->cutscene->instanceTrackCount; index++)
    {
        PlayInstanceTrackFrame(controller->frameShare, &controller->instanceTracks[index], &controller->cutscene->instanceTracks[index],
                               controller->partFrame, TrackSpace(controller));
    }
}

void PlayEmitterTracks(VideoController* controller)
{
    if (controller->cutscene == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < controller->cutscene->emitterTrackCount; index++)
    {
        PlayEmitterTrackFrame(controller->frameShare, &controller->emitterTracks[index], &controller->cutscene->emitterTracks[index],
                              controller->partFrame, TrackSpace(controller));
    }
}

void PlaySoundTracks(VideoController* controller)
{
    if (controller->cutscene == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < controller->cutscene->soundTrackCount; index++)
    {
        PlaySoundTrackFrame(controller->frameShare, &controller->soundTracks[index], &controller->cutscene->soundTracks[index],
                            controller->partFrame, TrackSpace(controller));
    }
}

void PlayCameraTrack(VideoController* controller)
{
    PlayCameraTrackFrame(controller->frameShare, &controller->cameraTrack, &controller->cutscene->camera, controller->partFrame,
                         TrackSpace(controller));
}

void ConstructCameraTrack(PlayedCameraTrack* track)
{
    track->cutting = false;
    track->track = nullptr;
    track->values = nullptr;
    track->cuts = nullptr;
    track->shot = 0;
}

void NextShot(PlayedCameraTrack* track)
{
    if (track->values != nullptr)
    {
        MemoryDeallocate2_(track->values);
    }

    track->values = nullptr;
    track->shot++;
}

PlayedInstanceTrack* ConstructInstanceTrack(PlayedInstanceTrack* track)
{
    ResetInstanceTrack(track);
    return track;
}

void ResetInstanceTrack(PlayedInstanceTrack* track)
{
    track->track = nullptr;
    track->bits.given = 0;
    track->values = nullptr;
    track->instance = nullptr;
    track->animation = nullptr;
    track->rotation.z = 0.0f;
    track->rotation.y = 0.0f;
    track->rotation.x = 0.0f;
    track->rotation.w = 1.0f;
    PlaceAtOrigin(&track->position);
}

// Retail bug: the data of its values (made by its frames) isn't freed: every cutscene loses 0x14 bytes per instance track
void ReleaseInstanceTrack(PlayedInstanceTrack* track)
{
    if (track->animation != nullptr)
    {
        DestroyAnimation(track->animation, DestroyAndFree);
    }

    InstanceContext* instance = track->instance;
    if (instance != nullptr && track->bits.given == 0)
    {
        CallVirtual<u32>(instance, instance->vtable, InstanceContext::ReleaseSlot);
    }

    ResetInstanceTrack(track);
}

void SetTrackInstance(PlayedInstanceTrack* track, InstanceContext* instance, u32 given)
{
    track->instance = instance;
    track->bits.given = given;
}

PlayedEmitterTrack* ConstructEmitterTrack(PlayedEmitterTrack* track)
{
    track->emitter = NoEmitter;
    track->track = nullptr;
    track->values = nullptr;
    for (s32& angle : track->angles)
    {
        angle = 0;
    }

    PlaceAtOrigin(&track->position);
    return track;
}

void ReleaseEmitterTrack(PlayedEmitterTrack* track)
{
    if (track->emitter != NoEmitter)
    {
        StopEmitter(track->emitter);
    }

    track->emitter = NoEmitter;
}

PlayedSoundTrack* ConstructSoundTrack(PlayedSoundTrack* track)
{
    ResetSoundTrack(track);
    return track;
}

void ResetSoundTrack(PlayedSoundTrack* track)
{
    track->track = nullptr;
    track->unused00 = -1;
    track->values = nullptr;
    PlaceAtOrigin(&track->position);
    track->volume = -1.0f;
}

void ReleaseSoundTrack(PlayedSoundTrack*)
{
}
