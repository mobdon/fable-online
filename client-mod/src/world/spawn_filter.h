#pragma once
// Which monsters the local game may still place from its level files.
//
// The server runs the monsters of every region it simulates (server/src/world/spawns.ts).
// It tells the mod, in its welcome, which creature defs it spawns itself; while that list
// is set, a creature the level loader builds from one of those defs is dropped again
// before it does anything, so each player only ever sees the server's copy.
//
// Creatures a quest script names (ScriptName in the level file) stay: quest scripts look
// them up by that name, and the server leaves them to the client too. Creatures a script
// creates at run time never pass through the level loader, so they are not affected.
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace fo::world {

class SpawnFilter {
  public:
    /** The server's list. Bad names are skipped; an empty list turns the filter off. */
    void set_server_defs(const std::vector<std::string>& defs);
    void clear();
    bool active() const { return !defs_.empty(); }
    size_t def_count() const { return defs_.size(); }
    bool owns(const std::string& def) const { return defs_.count(def) != 0; }

    /**
     * Whether to drop a thing the level loader just built. `creature` is true for an
     * AI creature; `script_name` is its ScriptName ("" or "NULL" when it has none).
     */
    bool should_drop(bool creature, const std::string& def, const std::string& script_name) const;

    /** A def name as Fable writes them: A-Z, 0-9 and _, at most 64 characters. */
    static bool valid_def(const std::string& def);

  private:
    std::set<std::string> defs_;
};

}  // namespace fo::world
