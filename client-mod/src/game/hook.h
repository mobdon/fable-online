#pragma once
// The one place the mod attaches to Fable's window, so everything that has to run on the
// game's own thread has somewhere to run.
//
// Engine calls (spawning, commands) are only safe on the thread that owns the game loop.
// The window's message handler is that thread, so work is registered here and runs from a
// timer message between frames, rather than from the mod's background threads.
#include <functional>

namespace fo::game::hook {

/** Finds the game window (it appears a few seconds after launch) and attaches. */
bool install();
void remove();

/** Runs `fn` on the game thread about every `everyMs`. Registration order is kept. */
void on_tick(std::function<void()> fn, unsigned everyMs);
/**
 * Runs `fn` once a frame, just before the frame is shown, when that is the game thread.
 * For memory writes only (no engine calls): it runs in the middle of the renderer's work.
 */
void on_frame(std::function<void()> fn);
/** Called by the Present hook. */
void run_frame();
/** Runs `fn` on the game thread when a key goes down for the first time. */
void on_key(std::function<void(int)> fn);

bool attached();

}  // namespace fo::game::hook
