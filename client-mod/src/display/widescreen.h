#pragma once
// Widescreen support for Fable.exe: a Hor+ field of view for the 3D camera and an optional
// unstretched HUD. See camera_patch.h for the camera maths.
#include <windows.h>

namespace fo::display {
// Patches the camera code. Call while the game is still suspended (from DllMain): it only
// allocates memory and writes a few code bytes. The result is logged by log_status().
void install();
// The game's back-buffer size, from device creation and every Reset.
void on_backbuffer(UINT w, UINT h, HWND window);
void log_status();
}  // namespace fo::display
