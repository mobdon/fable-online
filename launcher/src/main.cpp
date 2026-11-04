// Fable Online launcher.
//
// With no flags it opens the launcher window: news, login, region select,
// patching and Play (see ui.cpp). Play asks the gateway for a join ticket,
// then starts Fable.exe suspended with the mod DLL injected.
//
// Command-line mode skips the window and the gateway, for development and for
// region servers:
//   fable_online_launcher.exe --offline   start Fable with the mod, no login
//   --fable-boot                          leave userst.ini and user.ini alone (default: Guild start, no story gameflow)
//   --start-site GuildHSP                 holy site a new hero starts at
//   fable_online_launcher.exe --probe     same, with the field probe on (probe.bat)
//   fable_online_launcher.exe --server    server-mode Fable.exe for a region host
// Other flags:
//   --game "C:\path\to\Fable The Lost Chapters"   (a bare path also works)
//   --gateway https://gateway.example.net         (saved for next time)
//   --channel live|test                           (saved for next time)
#include "config.h"
#include "game.h"
#include "http.h"
#include "ui.h"
#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

namespace {

// The launcher is a GUI program; give command-line mode a console to print to.
void attach_console() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
}

int run_cli(const std::wstring& given, bool server, bool probe) {
    attach_console();
    std::wstring game = find_game_dir(given);
    if (game.empty()) {
        std::fwprintf(stderr, L"error: Fable.exe not found. Pass the game folder with --game.\n");
        return 1;
    }
    std::wstring dll = self_dir() + L"\\fable_online.dll";
    std::wprintf(L"game: %ls\\Fable.exe\nmod:  %ls\nmode: %ls\nprobe: %ls\n", game.c_str(), dll.c_str(),
                 server ? L"server" : L"client", probe ? L"on" : L"off");
    LaunchResult r = launch_fable(game, dll, {
        {L"FABLE_ONLINE_MODE", server ? L"server" : L"client"},
        {L"FABLE_ONLINE_PROBE", probe ? L"1" : L"0"},
    });
    if (!r.ok) {
        std::fwprintf(stderr, L"error: %ls\n", r.error.c_str());
        return 1;
    }
    std::wprintf(L"Fable started with Fable Online loaded (pid %lu)%ls.\n", r.pid,
                 r.unpacked_first ? L", injected after the game unpacked itself" : L"");
    if (!r.boot.empty()) std::wprintf(L"boot: new heroes start at the Guild, story gameflow off: %ls\n", r.boot.c_str());
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Config cfg;
    cfg.load();
    std::wstring given;
    bool server = false, probe = false, offline = false, changed = false, fableBoot = false;
    std::wstring startSite;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        bool has_value = i + 1 < argc;
        if (a == L"--server") server = true;
        else if (a == L"--probe") probe = true;
        else if (a == L"--offline") offline = true;
        else if (a == L"--fable-boot") fableBoot = true;              // keep Fable's own intro and front end
        else if (a == L"--start-site" && has_value) startSite = argv[++i];  // holy site a new hero starts at
        else if (a == L"--game" && has_value) given = argv[++i];
        else if (a == L"--gateway" && has_value) {
            std::string g = narrow(argv[++i]);
            while (!g.empty() && g.back() == '/') g.pop_back();
            if (!http::url_allowed(g)) {
                MessageBoxW(nullptr, L"The gateway must be an https:// URL (plain http only for localhost or a home/VPN network).",
                            L"Chapters of the Lost", MB_ICONERROR);
                return 1;
            }
            cfg.gateway = g;
            changed = true;
        } else if (a == L"--channel" && has_value) { cfg.channel = narrow(argv[++i]); changed = true; }
        else if (!a.empty() && a[0] != L'-') given = a;
    }
    LocalFree(argv);
    set_online_boot(!fableBoot, startSite);
    if (server || probe || offline) return run_cli(given, server, probe);
    if (!given.empty()) { cfg.game_dir = given; changed = true; }
    if (changed) cfg.save();
    return run_ui(cfg);
}
