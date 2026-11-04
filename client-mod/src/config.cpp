#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace fo {
namespace {
Config g_cfg;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ini_str(const std::wstring& file, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(sec, key, def, buf, 1024, file.c_str());
    return buf;
}
bool ini_bool(const std::wstring& f, const wchar_t* s, const wchar_t* k, bool def) {
    std::wstring v = ini_str(f, s, k, def ? L"1" : L"0");
    return !(v == L"0" || v == L"false" || v == L"off" || v == L"no");
}
float ini_float(const std::wstring& f, const wchar_t* s, const wchar_t* k, float def) {
    std::wstring v = ini_str(f, s, k, L"");
    return v.empty() ? def : static_cast<float>(_wtof(v.c_str()));
}
std::string env(const char* name) {
    char buf[2048] = {};
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : "";
}
void set_server(Config& c, const std::string& hostport) {
    if (hostport.empty()) return;
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) { c.chat_host = hostport; return; }
    c.chat_host = hostport.substr(0, colon);
    int p = std::atoi(hostport.c_str() + colon + 1);
    if (p > 0 && p < 65536) c.chat_port = static_cast<unsigned short>(p);
}
}  // namespace

std::wstring module_dir(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

void config_load(HMODULE self) {
    Config& c = g_cfg;
    std::wstring f = module_dir(self) + L"\\fable_online.ini";
    c.chat_enabled = ini_bool(f, L"chat", L"enabled", c.chat_enabled);
    set_server(c, narrow(ini_str(f, L"chat", L"server", L"")));
    c.name = narrow(ini_str(f, L"chat", L"name", L""));
    std::wstring key = ini_str(f, L"chat", L"open_key", L"");
    if (!key.empty()) c.open_key = _wtoi(key.c_str());
    c.tags_enabled = ini_bool(f, L"tags", L"enabled", c.tags_enabled);
    c.tags_self = ini_bool(f, L"tags", L"self", c.tags_self);
    c.tag_height = ini_float(f, L"tags", L"height", c.tag_height);
    std::wstring axis = ini_str(f, L"tags", L"up_axis", L"z");
    c.up_axis = axis == L"x" ? 0 : axis == L"y" ? 1 : 2;
    c.tag_full = ini_float(f, L"tags", L"full_distance", c.tag_full);
    c.tag_hidden = ini_float(f, L"tags", L"max_distance", c.tag_hidden);
    c.menu_enabled = ini_bool(f, L"menu", L"enabled", c.menu_enabled);
    c.overlay_enabled = ini_bool(f, L"overlay", L"enabled", c.overlay_enabled);
    c.font_px = static_cast<int>(ini_float(f, L"overlay", L"font_px", static_cast<float>(c.font_px)));
    c.chat_rows = static_cast<int>(ini_float(f, L"overlay", L"chat_rows", static_cast<float>(c.chat_rows)));
    if (c.font_px < 10 || c.font_px > 32) c.font_px = 15;
    if (c.chat_rows < 3 || c.chat_rows > 30) c.chat_rows = 8;
    c.widescreen = ini_bool(f, L"display", L"widescreen", c.widescreen);
    c.fov_scale = ini_float(f, L"display", L"fov_scale", c.fov_scale);
    if (!(c.fov_scale >= 0.7f && c.fov_scale <= 1.6f)) c.fov_scale = 1.0f;
    c.frame_probe = ini_bool(f, L"display", L"frame_probe", c.frame_probe);
    std::wstring win = ini_str(f, L"display", L"window", L"fullscreen");
    c.window_mode = win == L"windowed" ? WindowMode::Windowed : win == L"borderless" ? WindowMode::Borderless : WindowMode::Fullscreen;
    std::wstring res = ini_str(f, L"display", L"resolution", L"desktop");
    if (res == L"game") { c.res_w = c.res_h = 0; }
    else if (res != L"desktop") {
        int w = 0, h = 0;
        if (swscanf(res.c_str(), L"%dx%d", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 16384 && h <= 16384) { c.res_w = w; c.res_h = h; }
    }
    std::wstring hz = ini_str(f, L"display", L"refresh", L"game");
    c.refresh_hz = hz == L"max" ? -1 : hz == L"game" ? 0 : _wtoi(hz.c_str());
    if (c.refresh_hz < -1 || c.refresh_hz > 1000) c.refresh_hz = 0;
    std::wstring vs = ini_str(f, L"display", L"vsync", L"game");
    c.vsync = vs == L"on" || vs == L"1" ? 1 : vs == L"off" || vs == L"0" ? 0 : -1;
    c.camera_auto_center = ini_bool(f, L"display", L"camera_auto_center", c.camera_auto_center);
    std::wstring ms = ini_str(f, L"display", L"mouse_smoothing", L"off");
    c.mouse_smoothing = ms == L"off" ? 0 : ms == L"game" ? -1 : _wtoi(ms.c_str());
    if (c.mouse_smoothing < -1 || c.mouse_smoothing > 60) c.mouse_smoothing = 0;
    std::wstring hs = ini_str(f, L"display", L"hud_scale", L"off");
    c.hud_scale = hs == L"auto" ? 0.0f : hs == L"off" ? -1.0f : static_cast<float>(_wtof(hs.c_str()));
    if (c.hud_scale > 4.0f) c.hud_scale = 4.0f;
    c.low_latency = ini_bool(f, L"display", L"low_latency", c.low_latency);
    c.aim_at_crosshair = ini_bool(f, L"display", L"aim_at_crosshair", c.aim_at_crosshair);
    std::wstring ch = ini_str(f, L"overlay", L"crosshair", L"off");
    c.crosshair = ch == L"ring" ? Crosshair::Ring : ch == L"dot" ? Crosshair::Dot : ch == L"cross" ? Crosshair::Cross : Crosshair::Off;

    set_server(c, env("FABLE_ONLINE_CHAT_SERVER"));
    if (auto n = env("FABLE_ONLINE_NAME"); !n.empty()) c.name = n;
    c.token = env("FABLE_ONLINE_TOKEN");
    if (auto w = env("FABLE_ONLINE_WIDESCREEN"); !w.empty()) c.widescreen = w != "0";

    if (c.name.empty()) {  // dev fallback: the Windows user name
        wchar_t user[64] = {};
        DWORD n = 64;
        if (GetUserNameW(user, &n)) c.name = narrow(user);
    }
}

const Config& config() { return g_cfg; }
}  // namespace fo
