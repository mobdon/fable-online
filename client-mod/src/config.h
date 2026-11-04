#pragma once
// Settings from fable_online.ini, next to fable_online.dll (the launcher's folder).
// Environment variables win, so the launcher can pass a login token without writing it to disk.
#include <string>
#include <windows.h>

namespace fo {
enum class WindowMode { Fullscreen, Windowed, Borderless };

enum class Crosshair { Off, Ring, Dot, Cross };

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
    // [menu]
    bool menu_enabled = true;    // the start-up menu over Fable's title screen
    // [overlay]
    bool overlay_enabled = true;
    int font_px = 15;
    int chat_rows = 8;
    // [display]
    bool widescreen = true;      // Hor+ field of view on screens wider than 4:3
    float fov_scale = 1.0f;      // 1 = Fable's own view; 1.1 = 10% more in every direction
    bool frame_probe = false;    // F10 logs one frame's Direct3D calls (for HUD work)
    WindowMode window_mode = WindowMode::Fullscreen;
    int res_w = -1, res_h = -1;  // -1 = the desktop's resolution, 0 = the game's, else pixels
    int refresh_hz = 0;          // -1 = the monitor's highest at that resolution, 0 = the game's, else Hz
    int vsync = -1;              // -1 = the game's setting, 0 = off, 1 = on
    bool low_latency = false;    // keep the CPU at most one frame ahead of the GPU (less mouse lag)
    bool camera_auto_center = false;  // Fable eases the camera pitch back while walking
    bool camera_latch = false;       // turn the view with the mouse every frame between world updates
    bool camera_assist = false;       // Fable swings the camera toward the hero's facing and target
    int mouse_smoothing = 0;     // mouse-look frames averaged: 0 = off, -1 = Fable's own, else N
    float hud_scale = -1;        // 0 = automatic (as on a 900-pixel-tall screen), -1 = off, else the factor
    bool aim_at_crosshair = false;    // pick melee, spell and lock targets along the camera's view
    // [overlay]
    Crosshair crosshair = Crosshair::Off;  // a mark at the centre of the view
};

void config_load(HMODULE self);
const Config& config();
std::wstring module_dir(HMODULE m);
}  // namespace fo
