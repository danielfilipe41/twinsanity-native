#pragma once

#include "common.h"

struct Matrix4x4;
struct InstanceContext;
struct ShadowEntry;

// The host's picture between the game's steps. The game steps its logic at the disc's field rate (50 a second on this PAL
// disc) and the host presents at the player's rate (settings.txt's `render`). A display frame that falls between two steps
// draws the state eased by the share of a step gone, so the picture moves at the display's rate while everything the game
// counts per frame keeps the field's. A step's own frame draws with none of it (the stepped state, as retail drew it).
//
// The state lives in the platform (src/platform/sdl3/hostframe.cpp): the game only says what happened and when. The console
// has no display frames between steps, so there every call below is a no-op and every ease says it has nothing to ease.
namespace HostFrame
{
// The share of the step gone since the last logic boundary, 0 to 1
void SetAlpha(f32 alpha);

// The logic boundary: the samples taken for a step belong to it
void BeginLogicStep();

// Whether this draw is an extra display frame (between two steps) rather than a step's own. The draws that happen once a
// rendered frame (the chunk scenery's draw stamp) let a redraw through: the game's own frame count does not move between
// the steps, and moving it would age every game value counted in rendered frames
void SetRedraw(bool redraw);
bool IsRedraw();

// The current logic step number (incremented in BeginLogicStep)
u32 GetCurrentStep();

// A chunk's shadow lists take turns: a step casts into one while the other is drawn, and a display frame between steps
// redraws the list the step's own frame drew -- only if it is the current step's (one from an earlier step belongs to a chunk
// the step did not draw). The list's address keys it (a chunk's two lists are separate allocations, and the addresses may be
// a freed chunk's): a chunk resets both when its shadows are made and forgets them when they're freed
void SetShadowStep(const ShadowEntry* list, u32 step);
u32 ShadowStep(const ShadowEntry* list);
void ForgetShadowSteps(const ShadowEntry* first, const ShadowEntry* second);

// The camera's place matrix as the step left it (the view's update calls it once a step)
void SampleCamera(const void* camera, const Matrix4x4* matrix);
// The camera's matrix eased between its samples: false when there is nothing to ease (no camera, a new one, a jump, or a
// step's own draw), and the caller keeps the place as it stands
bool InterpolateCameraMatrix(const void* camera, const Matrix4x4* current, Matrix4x4* out);
// An instance's matrix eased between its movement node's last place and this step's: false when it has no movement node
// captured (a thing that does not move, or the step's own draw), and the caller keeps the place's matrix
bool InterpolateInstanceMatrix(const InstanceContext* instance, Matrix4x4* out);

// The instances a step's own frame drew are remembered here, so the ones without a movement node (the played character,
// the things moved by scripts or attachments) ease too. Remembered on the step's own draw; the samples turn over at the
// next step's boundary and stale entries are dropped
void RememberInstance(const InstanceContext* instance, const Matrix4x4* matrix);
// An instance's matrix eased between the one its last step's draw remembered and the one it stands at now: false when it
// wasn't drawn last step
bool InterpolateRememberedMatrix(const InstanceContext* instance, Matrix4x4* out);

// The joints a step's draw made: a display frame between steps draws the skeleton eased between the step before's pose and
// this step's, so the limbs move at the display's rate as the place does. `joints` is the animator's matrices (the key) and
// `model` the OGI the pose is one of (an animator's memory may be another's by now), out takes count matrices. Remembered on
// the step's own draw; false when there is no pose from the step before
bool EaseJoints(const void* joints, const void* model, const Matrix4x4* current, u32 count, Matrix4x4* out);

#ifdef __mips__
// The console draws once a step, with no frame in between to ease into: the calls stay in the game's code (so both targets
// read the same) and compile to nothing
inline void SetAlpha(f32) {}
inline void BeginLogicStep() {}
inline void SetRedraw(bool) {}
inline bool IsRedraw() { return false; }
inline u32 GetCurrentStep() { return 0; }
inline void SetShadowStep(const ShadowEntry*, u32) {}
inline u32 ShadowStep(const ShadowEntry*) { return 0; }
inline void ForgetShadowSteps(const ShadowEntry*, const ShadowEntry*) {}
inline void SampleCamera(const void*, const Matrix4x4*) {}
inline bool InterpolateCameraMatrix(const void*, const Matrix4x4*, Matrix4x4*) { return false; }
inline bool InterpolateInstanceMatrix(const InstanceContext*, Matrix4x4*) { return false; }
inline void RememberInstance(const InstanceContext*, const Matrix4x4*) {}
inline bool InterpolateRememberedMatrix(const InstanceContext*, Matrix4x4*) { return false; }
inline bool EaseJoints(const void*, const void*, const Matrix4x4*, u32, Matrix4x4*) { return false; }
#endif
}
