#pragma once
// Mod settings: one screen, in the look of Fable's own menus, for everything the mod lets a
// player change while the game runs. Extras is for every player (camera, targeting, HUD);
// Developer appears only in dev builds (FABLE_ONLINE_DEV=1) and holds the test tools.
//
// Changes take effect at once where the code reads the setting each frame, and are written to
// fable_online.ini next to the DLL so they are kept for the next session. Rows marked
// "after restart" are read once when the game starts.
//
// Opened from the start-up menu's MOD SETTINGS button and from the row added to Fable's own
// menu (open()). Drawn by the overlay; input comes from the overlay's window and DirectInput hooks.
#include <windows.h>
#include <string>

namespace fo::overlay { class Renderer; }

namespace fo::modsettings {

enum PageId { kExtras = 0, kDeveloper = 1 };

/** From the mod thread, once: attaches the game-thread work (dev actions) to the game window. */
void install();

/** Opens the screen on a page (kDeveloper falls back to Extras outside dev builds). Any thread. */
void open(int page = kExtras);
void close();
bool is_open();
/** True while open, and for a moment after closing, so the closing key doesn't reach the game. */
bool input_blocked();

/** Overlay, render thread, between Renderer::begin and end. */
void draw(overlay::Renderer& r);
/** Overlay, while drawing in game: dev readouts that sit on the HUD (hero position). */
void draw_hud(overlay::Renderer& r, bool hero_valid, float x, float y, float z, unsigned area);

/** A line from elsewhere (such as the server's answer to a dev spawn) shown on the screen. Any thread. */
void notify(const std::string& text);
/** Overlay, every frame: draws the MOD SETTINGS button on Fable's pause screen. */
void draw_pause_button(overlay::Renderer& r);

/** Window messages while open: true when the screen used the message. */
bool on_message(HWND w, UINT msg, WPARAM wp, LPARAM lp);
/** DirectInput mouse reads while open (when the window gets no mouse messages). */
void on_mouse_state(const void* state, DWORD size);
void on_mouse_data(const void* objects, DWORD object_size, DWORD count);

}  // namespace fo::modsettings
