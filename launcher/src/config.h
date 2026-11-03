// Launcher settings in %APPDATA%\FableOnline\launcher.ini.
#pragma once
#include <string>

#ifndef FO_DEFAULT_GATEWAY
#define FO_DEFAULT_GATEWAY "http://127.0.0.1:8787"
#endif

struct Config {
    std::string gateway = FO_DEFAULT_GATEWAY;  // base URL, no trailing slash
    std::string channel = "live";
    std::wstring game_dir;                      // empty = auto-detect
    std::string username;
    std::string refresh_token;                  // plain in memory, DPAPI on disk
    std::string region;                         // last region picked
    std::string client_version;                 // version of the last full patch

    static std::wstring path();
    void load();
    void save() const;
    std::string api(const std::string& route) const { return gateway + "/launcher/v1" + route; }
};
