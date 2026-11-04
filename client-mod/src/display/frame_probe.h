#pragma once
// One-frame Direct3D capture for working out how Fable draws its HUD and menus.
// Mod settings > Developer > Capture one frame (or F10 with [dev] hotkeys=1): the next frame's render targets, viewports, shaders, shader constants
// and draw calls go to fable_online.log, with the disassembly of every vertex shader used.
// Off unless [display] frame_probe=1, and costs one compare per call when idle.
#include <d3d9.h>

namespace fo::display::probe {
void hook(IDirect3DDevice9* dev);
void on_present(IDirect3DDevice9* dev);   // call at the top of the game's Present
void request_capture();                   // capture a frame a few frames from now
bool ready();                             // hooked (needs frame_probe=1 at start-up)
void note(const char* what);              // free-text marker in a capture (overlay drawing etc.)
void on_transform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m);
}  // namespace fo::display::probe
