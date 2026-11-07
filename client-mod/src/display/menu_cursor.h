#pragma once
// Fable's menu cursor moves only when the game reads the mouse (DirectInput, about 28 times a
// second in menus; the Windows cursor is held at the window centre), so at high frame rates it
// trails the hand. Its picture is drawn every frame as one textured quad, the last sprite of
// the frame (CEnginePrimitiveManager2DSprites, DrawIndexedPrimitiveUP returning to 0x00A0AF2C).
//
// The mod learns which texture that quad uses (the same texture is the frame's last single-quad
// sprite for a second, outside play), learns how far the quad moves per unit of mouse motion
// Fable takes in, and then draws the quad ahead by the motion polled since Fable's last read.
// Until both are learned, nothing changes.
#include <d3d9.h>

namespace fo::display::menu_cursor {
// From the sprite draw hook. Returns moved vertices, or nullptr to draw them as they are.
const void* sprite(IDirect3DDevice9* d, const void* data, UINT first, UINT count, UINT prims, UINT stride);
void on_motion(long dx, long dy);   // mouse motion polled this frame
void on_present();                  // once a frame
void before_read();                 // around CMouseDX's read (mouse_look's hook)
}  // namespace fo::display::menu_cursor
