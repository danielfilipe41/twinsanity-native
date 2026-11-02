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

// The camera's place matrix as the step left it (the view's update calls it once a step)
void SampleCamera(const void* camera, const Matrix4x4* matrix);
// The camera's matrix eased between its samples: false when there is nothing to ease (no camera, a new one, a jump, or a
// step's own draw), and the caller keeps the place as it stands
bool InterpolateCameraMatrix(const void* camera, const Matrix4x4* current, Matrix4x4* out);
// An instance's matrix eased between its movement node's last place and this step's: false when it has no movement node
// captured (a thing that does not move, or the step's own draw), and the caller keeps the place's matrix
bool InterpolateInstanceMatrix(const InstanceContext* instance, Matrix4x4* out);
}
