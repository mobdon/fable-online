#pragma once
// Mouse-look smoothing. Fable's PC mouse-look camera (NCameraModes::CPCMouseLook, update at
// 0x00873E10) averages the mouse movement over the last N frames, N coming from its camera
// definition (+0xB4). The average makes the view trail the mouse, more so the more frames
// it covers. The patch replaces the load of N at 0x00873EDC with a constant.
namespace fo::display::mouse_look {
void install();
const char* status();
// The pitch drifting back to level while walking ([display] camera_auto_center).
const char* center_status();
const char* aim_status();
// Camera (mouse-look) updates since the last call.
unsigned take_updates();
}  // namespace fo::display::mouse_look
