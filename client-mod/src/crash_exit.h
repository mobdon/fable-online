#pragma once
// Ends Fable.exe when it has crashed, instead of leaving a frozen full-screen window that
// can only be cleared by restarting the PC. See crash_exit.cpp.
#include <cstdint>

namespace fo::crash_exit {

/** Starts the watchdog. Call once, from the mod's own start-up. */
void install();
/** The game thread is alive: call from its regular tick. */
void game_alive(uint32_t nowMs);

}  // namespace fo::crash_exit
