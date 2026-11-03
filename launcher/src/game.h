// Finding Fable: The Lost Chapters and starting it with the mod injected.
#pragma once
#include <string>
#include <utility>
#include <vector>

std::wstring find_game_dir(const std::wstring& given);
// pid of a running Fable.exe, or 0. Fable allows only one copy at a time.
unsigned long running_fable();

struct LaunchResult {
    bool ok = false;
    unsigned long pid = 0;
    std::wstring error;
};

// Starts Fable.exe suspended, injects dll with LoadLibraryW, resumes it.
// env is added to the game's environment (on top of the launcher's own).
LaunchResult launch_fable(const std::wstring& game_dir, const std::wstring& dll,
                          const std::vector<std::pair<std::wstring, std::wstring>>& env);
