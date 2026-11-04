// Finding Fable: The Lost Chapters and starting it with the mod injected.
#pragma once
#include <string>
#include <utility>
#include <vector>

std::wstring find_game_dir(const std::wstring& given);
// pid of a running Fable.exe, or 0. Fable allows only one copy at a time.
unsigned long running_fable();

// How Fable is started. Online boot (the default) rewrites the player's two start-up files:
// userst.ini so a new game starts at `holySite` (the Guild) and user.ini so the story's
// gameflow quest never runs. The retail engine has no switch to skip its title screen, so
// that is driven by the mod. The originals are kept next to the files as *.fable-original.
void set_online_boot(bool on, const std::wstring& holySite);

struct LaunchResult {
    bool ok = false;
    // What the online boot did to the start-up files (empty when off or when nothing changed).
    std::wstring boot;
    unsigned long pid = 0;
    // The exe unpacks itself at run time (retail discs): the mod went in after that, not at start-up.
    bool unpacked_first = false;
    std::wstring error;
};

// Starts Fable.exe suspended, injects dll with LoadLibraryW, resumes it. A build whose code
// section is empty on disk (the retail discs' wrapper unpacks it in memory) is started first and
// gets the dll once its own code is running; see read_layout in game.cpp.
// env is added to the game's environment (on top of the launcher's own).
LaunchResult launch_fable(const std::wstring& game_dir, const std::wstring& dll,
                          const std::vector<std::pair<std::wstring, std::wstring>>& env);
