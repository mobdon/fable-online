#pragma once
// One-frame Direct3D capture for working out how Fable draws its HUD and menus.
// Press F10 in game: the next frame's render targets, viewports, shaders, shader constants
// and draw calls go to fable_online.log, with the disassembly of every vertex shader used.
// Off unless [display] frame_probe=1, and costs one compare per call when idle.
#include <d3d9.h>

namespace fo::display::probe {
void hook(IDirect3DDevice9* dev);
void on_present(IDirect3DDevice9* dev);   // call at the top of the game's Present
void note(const char* what);              // free-text marker in a capture (overlay drawing etc.)
void on_transform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m);
}  // namespace fo::display::probe
