// Patching: makes the files next to the launcher match the server's manifest.
#pragma once
#include "config.h"
#include <functional>
#include <string>
#include <vector>

namespace updater {

struct File {
    std::string path;    // relative, '/'-separated, checked by safe_path()
    unsigned long long size = 0;
    std::string sha256;
    std::string url;
};

struct Manifest {
    std::string version;
    int protocol = 0;
    std::vector<File> files;
};

// Paths stay inside the launcher folder: no "..", no drive or root, plain characters only.
bool safe_path(const std::string& p);

bool fetch(const Config& c, Manifest& out, std::string& error);
// Files whose local copy is missing or has the wrong size or hash.
std::vector<File> outdated(const Manifest& m);

using Progress = std::function<void(const std::string& file, unsigned long long done, unsigned long long total)>;

struct Result {
    bool ok = false;
    bool launcher_replaced = false;  // restart the launcher to run the new one
    std::string error;
};
Result apply(const std::vector<File>& files, const Progress& progress);

// Removes the previous launcher left behind by a self-update.
void cleanup_old_launcher();
// Starts the (new) launcher with the same arguments. Caller then exits.
bool restart_self();

}  // namespace updater
