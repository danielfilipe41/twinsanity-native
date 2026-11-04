#pragma once

#include "common.h"
#include "game/math.h"

struct InstanceContext;

// The host's picture between the game's steps. The game steps its logic at the disc's field rate (50 a second on this PAL
// disc) and the host presents at the player's rate (settings.txt's `render`). A display frame that falls between two steps
// draws the state eased by the share of a step gone, so the picture moves at the display's rate while everything the game
// counts per frame keeps the field's. A step's own frame draws with none of it (the stepped state, as retail drew it).
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
}
