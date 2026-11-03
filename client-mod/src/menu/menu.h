#pragma once
// The start-up menu (Chapters of the Lost Online). Covers Fable's own title screen with
// Fable's front-end art, read from the player's install (data/graphics/pc/frontend.big),
// and the fonts shipped in fonts/ next to the DLL. Drawn from the overlay's EndScene hook.
#include <windows.h>
#include <d3d9.h>

namespace fo::menu {
void set_module(HMODULE self);            // DllMain: where fonts/ lives. No I/O.
bool active();                            // true while the menu covers the screen
void draw(IDirect3DDevice9* dev);         // render thread, once a frame, while active()
void on_device_lost();                    // before IDirect3DDevice9::Reset
// Window messages while active(): returns true when the menu used the message.
bool on_message(HWND w, UINT msg, WPARAM wp, LPARAM lp);
// DirectInput mouse reads while active(), before they are zeroed for the game.
void on_mouse_state(const void* state, DWORD size);
void on_mouse_data(const void* objects, DWORD object_size, DWORD count);
void log_status();
}  // namespace fo::menu
