#pragma once
// Settings from fable_online.ini, next to fable_online.dll (the launcher's folder).
// Environment variables win, so the launcher can pass a login token without writing it to disk.
#include <string>
#include <windows.h>

namespace fo {
struct Config {
    // [chat]
    bool chat_enabled = true;
    std::string chat_host = "127.0.0.1";
    unsigned short chat_port = 7781;
    std::string name;    // dev servers log in by name
    std::string token;   // production: signed by the gateway (FABLE_ONLINE_TOKEN)
    int open_key = VK_RETURN;
    // [tags]
    bool tags_enabled = true;
    bool tags_self = false;      // also tag your own hero, to check placement alone
    float tag_height = 2.1f;
    int up_axis = 2;             // x=0, y=1, z=2
    float tag_full = 25.0f;
    float tag_hidden = 55.0f;
    // [overlay]
    bool overlay_enabled = true;
    int font_px = 15;
    int chat_rows = 8;
};

void config_load(HMODULE self);
const Config& config();
std::wstring module_dir(HMODULE m);
}  // namespace fo
