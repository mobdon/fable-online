#pragma once
// Mouse-look smoothing. Fable's PC mouse-look camera (NCameraModes::CPCMouseLook, update at
// 0x00873E10) averages the mouse movement over the last N frames, N coming from its camera
// definition (+0xB4). The average makes the view trail the mouse, more so the more frames
// it covers. The patch replaces the load of N at 0x00873EDC with a constant.
#include <windows.h>

namespace fo::display::mouse_look {
void install();
const char* status();
// The pitch drifting back to level while walking ([display] camera_auto_center).
const char* center_status();
const char* aim_status();
// Fable's camera assist that turns and shifts the view on its own ([display] camera_assist).
const char* assist_status();
// Reads the mouse's motion since the last call, from Fable's own mouse device. Render thread.
void poll_mouse();
// True while the mouse-look camera has updated in the last 300 ms (playing, not in a menu).
bool in_game();
// Camera (mouse-look) updates since the last call.
unsigned take_updates();
// Mouse reads (CMouseDX updates) since the last call.
unsigned take_mouse_reads();
// While held (Realm chat open), Fable's mouse reads see no movement and no buttons, so the
// camera stays still and clicks don't swing a weapon, and the Windows cursor is left where
// the player moves it instead of being pulled back to the window centre. Any thread.
void hold(bool on, HWND window);
bool held();
}  // namespace fo::display::mouse_look
