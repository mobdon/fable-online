#pragma once
// Bigger HUD and menus: the runtime half of hud_patch.h.
#include <windows.h>
#include <d3d9.h>

namespace fo::display::hud {
// Patches Fable's screen-size function and the cursor imports. Call from DllMain.
void install();
// The size the device is about to get (before CreateDevice / Reset).
void set_screen(UINT w, UINT h, HWND window);
// Hooks the 2D constant upload on the game's device.
void hook_device(IDirect3DDevice9* dev);
// Mouse messages on their way to the game: scales the position in lParam down.
LPARAM map_mouse(UINT msg, LPARAM l);
float scale();
void log_status();
}  // namespace fo::display::hud
