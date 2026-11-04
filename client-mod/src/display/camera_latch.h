#pragma once
// Turns the rendered view with the mouse on every frame.
//
// Fable updates its world, and the mouse-look camera with it, 15 times a second, and draws
// the frames in between by blending the last two updates. So the view on screen trails the
// mouse by one to two updates (70 to 130 ms). Just before the engine takes the frame's
// camera (CEngine::SetCamera, vtable slot 0x012A0F4C), this turns the camera about the
// vertical axis through the hero by the turn Fable hasn't shown yet: the rest of the last
// update's turn still being blended in, plus the mouse motion since that update. The
// camera description is put back right after, so the game itself never sees the change.
//
// Scales are learnt while playing (yaw per mouse count, and which way the view turns), so
// the view only starts leading once there has been enough mouse movement to measure them.

namespace fo::display::camera_latch {
void install();
// After each mouse-look camera update: how far its yaw turned (turns), and the hero focus
// point the camera turns around. Game thread.
void on_update(float yaw_turns, const float focus[3]);
// Mouse motion read this frame (counts). Game thread.
void on_mouse(long dx);
const char* status();
// One line with what it has learnt and how often it turned the view. Game thread.
void log_stats();
}  // namespace fo::display::camera_latch
