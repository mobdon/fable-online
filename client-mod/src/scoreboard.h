#pragma once
// Points Fable's leftover online scoreboard and clan client at our server.
namespace fo {
// Patch the scoreboard host and port in Fable.exe. Call from DllMain, before the game runs.
// Returns false (and changes nothing) if the bytes don't match the Steam Fable.exe.
bool scoreboard_patch();
// Log what scoreboard_patch() did (the log isn't open yet inside DllMain).
void scoreboard_report();
}
