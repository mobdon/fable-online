#pragma once
// Fable's menu cursor (NUISystem::CMouseCursor) only moves when the game reads the mouse, 15 times
// a second, so at high frame rates the drawn cursor trails the hand in steps. The game reads the
// mouse as GetCursorPos minus where it last put the cursor, then warps it back. Between reads,
// the movement Fable will see next is already known, so the cursor is drawn there now.
//
// CMouseCursor's vtable (0x0124C22C) slot 23 (0x0055C600) gives the cursor's position scaled for
// the screen. The mod wraps that slot: it adds the movement since Fable's last read, times a
// factor it learns by comparing how far Fable moves the cursor with the movement each read saw.
// Until that factor fits well, nothing changes.
namespace fo::display::menu_cursor {
void install();
void before_read();   // around CMouseDX's read (mouse_look's hook)
void after_read();
}  // namespace fo::display::menu_cursor
