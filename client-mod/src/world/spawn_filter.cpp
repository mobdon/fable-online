#include "spawn_filter.h"

namespace fo::world {

bool SpawnFilter::valid_def(const std::string& def) {
    if (def.empty() || def.size() > 64) return false;
    for (char c : def)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

void SpawnFilter::set_server_defs(const std::vector<std::string>& defs) {
    defs_.clear();
    for (const std::string& d : defs)
        if (valid_def(d)) defs_.insert(d);
}

void SpawnFilter::clear() { defs_.clear(); }

bool SpawnFilter::should_drop(bool creature, const std::string& def, const std::string& script_name) const {
    if (!creature || !active()) return false;
    if (!script_name.empty() && script_name != "NULL") return false;
    return owns(def);
}

}  // namespace fo::world
