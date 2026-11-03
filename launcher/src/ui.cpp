// The launcher window: news, account, region list, patching and Play.
// All network work runs on worker threads that post results back as WM_APP messages,
// so the window never freezes.
#include "ui.h"
#include "api.h"
#include "game.h"
#include "updater.h"
#include "util.h"
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <memory>
#include <thread>

namespace {

enum : int {
    ID_NEWS = 100, ID_USER, ID_PASS, ID_REMEMBER, ID_LOGIN, ID_REGISTER, ID_LOGOUT, ID_ACCOUNT,
    ID_REGIONS, ID_PLAY, ID_PROGRESS, ID_STATUS, ID_SERVER, ID_TITLE, ID_GAMEDIR, ID_GAMEDIR_LABEL,
    ID_USER_LABEL, ID_PASS_LABEL, ID_NEWS_LABEL, ID_REGION_LABEL,
};

enum : UINT {
    WM_STATUS = WM_APP + 1, WM_NEWS, WM_LOGIN, WM_PATCH_PROGRESS, WM_PATCH_DONE, WM_LAUNCHED,
};
const UINT_PTR kPollTimer = 1;
const UINT kPollMs = 30000;
const UINT_PTR kGameTimer = 2;

struct LoginResult { bool ok; api::Session session; std::string error; };
struct PatchProgress { std::string file; unsigned long long done, total; };
struct PatchDone { bool ok; bool restart; std::string version; std::string error; };
struct Launched { bool ok; api::Session session; std::wstring error; unsigned long pid; };
struct NewsResult { bool ok; std::vector<api::NewsItem> items; std::string error; };

struct App {
    Config* cfg = nullptr;
    HWND wnd = nullptr;
    HFONT font = nullptr, title_font = nullptr;
    int dpi = 96;
    api::Session session;
    api::Status status;
    bool status_busy = false, logging_in = false, patching = false, patched = false, launching = false;
    std::wstring game_dir;
    std::string client_version;
    unsigned long game_pid = 0;

    HWND item(int id) const { return GetDlgItem(wnd, id); }
    int S(int v) const { return MulDiv(v, dpi, 96); }
} app;

template <class T> void post(UINT msg, T* payload) {
    if (!app.wnd) { delete payload; return; }
    if (!PostMessageW(app.wnd, msg, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void set_text(int id, const std::wstring& s) { SetWindowTextW(app.item(id), s.c_str()); }
void set_status(const std::wstring& s) { set_text(ID_STATUS, s); }

std::wstring get_text(int id) {
    HWND h = app.item(id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, app.S(x), app.S(y), app.S(w), app.S(h),
                             app.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return c;
}

std::string selected_region() {
    HWND lv = app.item(ID_REGIONS);
    int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (i < 0) return "";
    LVITEMW it = {};
    it.mask = LVIF_PARAM;
    it.iItem = i;
    ListView_GetItem(lv, &it);
    size_t idx = static_cast<size_t>(it.lParam);
    return idx < app.status.regions.size() ? app.status.regions[idx].id : "";
}

const api::Region* region_by_id(const std::string& id) {
    for (auto& r : app.status.regions) if (r.id == id) return &r;
    return nullptr;
}

void refresh_controls() {
    bool logged_in = !app.session.access_token.empty() || !app.session.refresh_token.empty();
    for (int id : {ID_USER, ID_PASS, ID_REMEMBER, ID_LOGIN, ID_REGISTER, ID_USER_LABEL, ID_PASS_LABEL})
        ShowWindow(app.item(id), logged_in ? SW_HIDE : SW_SHOW);
    for (int id : {ID_ACCOUNT, ID_LOGOUT}) ShowWindow(app.item(id), logged_in ? SW_SHOW : SW_HIDE);
    for (int id : {ID_LOGIN, ID_REGISTER, ID_USER, ID_PASS}) EnableWindow(app.item(id), !app.logging_in);
    if (logged_in) set_text(ID_ACCOUNT, L"Logged in as " + widen(app.session.account));

    const api::Region* r = region_by_id(selected_region());
    bool running = app.game_pid != 0;
    bool can_play = logged_in && app.patched && !app.patching && !app.launching && !running &&
                    !app.game_dir.empty() && r && r->joinable();
    EnableWindow(app.item(ID_PLAY), can_play);
    set_text(ID_PLAY, running ? L"Playing" : app.launching ? L"Starting..." : app.patching ? L"Updating..." : L"Play");
    set_text(ID_GAMEDIR_LABEL, app.game_dir.empty() ? L"Fable: The Lost Chapters not found. Pick its folder:"
                                                    : L"Game: " + app.game_dir);
}

// ---- workers ---------------------------------------------------------------

void start_status() {
    if (app.status_busy) return;
    app.status_busy = true;
    Config c = *app.cfg;
    std::thread([c] { post(WM_STATUS, new api::Status(api::status(c))); }).detach();
}

void start_news() {
    Config c = *app.cfg;
    std::thread([c] {
        auto* r = new NewsResult{};
        r->ok = api::news(c, r->items, r->error);
        post(WM_NEWS, r);
    }).detach();
}

void start_patch() {
    if (app.patching) return;
    app.patching = true;
    set_status(L"Checking for updates...");
    refresh_controls();
    Config c = *app.cfg;
    std::thread([c] {
        auto* done = new PatchDone{};
        updater::Manifest m;
        if (!updater::fetch(c, m, done->error)) { done->error = "Can't check for updates: " + done->error; post(WM_PATCH_DONE, done); return; }
        auto need = updater::outdated(m);
        updater::Result r = updater::apply(need, [](const std::string& f, unsigned long long d, unsigned long long t) {
            // Throttle: a progress message per 256 KiB is plenty.
            static unsigned long long last = 0;
            if (d < last || d - last >= (256u << 10) || d == t) { last = d; post(WM_PATCH_PROGRESS, new PatchProgress{f, d, t}); }
        });
        done->ok = r.ok;
        done->restart = r.launcher_replaced;
        done->version = m.version;
        if (!r.ok) done->error = "Update failed: " + r.error;
        post(WM_PATCH_DONE, done);
    }).detach();
}

void start_login(bool create) {
    std::wstring user = get_text(ID_USER), pass = get_text(ID_PASS);
    if (user.empty() || pass.empty()) { set_status(L"Enter a username and password."); return; }
    app.logging_in = true;
    set_status(create ? L"Creating account..." : L"Logging in...");
    refresh_controls();
    Config c = *app.cfg;
    std::string u = narrow(user), p = narrow(pass);
    SecureZeroMemory(&pass[0], pass.size() * sizeof(wchar_t));
    SetWindowTextW(app.item(ID_PASS), L"");
    std::thread([c, u, p, create]() mutable {
        auto* r = new LoginResult{};
        r->ok = api::login(c, u, p, create, r->session, r->error);
        SecureZeroMemory(&p[0], p.size());
        post(WM_LOGIN, r);
    }).detach();
}

void start_refresh_login() {
    app.logging_in = true;
    set_status(L"Logging in...");
    Config c = *app.cfg;
    api::Session s = app.session;
    std::thread([c, s]() mutable {
        auto* r = new LoginResult{};
        r->ok = api::refresh(c, s, r->error);
        r->session = s;
        post(WM_LOGIN, r);
    }).detach();
}

void start_play() {
    std::string region = selected_region();
    if (region.empty()) return;
    app.launching = true;
    app.cfg->region = region;
    app.cfg->save();
    set_status(L"Joining " + widen(region) + L"...");
    refresh_controls();
    Config c = *app.cfg;
    api::Session s = app.session;
    std::wstring game = app.game_dir;
    std::string version = app.client_version;
    std::thread([c, s, game, region, version]() mutable {
        auto* r = new Launched{};
        api::Ticket t;
        std::string err;
        if (!api::join(c, s, region, version, t, err)) {
            r->error = widen(err);
        } else {
            LaunchResult lr = launch_fable(game, self_dir() + L"\\fable_online.dll", {
                {L"FABLE_ONLINE_MODE", L"client"},
                {L"FABLE_ONLINE_PROBE", L"0"},
                {L"FABLE_ONLINE_GATEWAY", widen(c.gateway)},
                {L"FABLE_ONLINE_REGION", widen(region)},
                {L"FABLE_ONLINE_ACCOUNT", widen(s.account)},
                {L"FABLE_ONLINE_TICKET", widen(t.ticket)},
                {L"FABLE_ONLINE_HOST", widen(t.host)},
                {L"FABLE_ONLINE_PORT", std::to_wstring(t.port)},
            });
            r->ok = lr.ok;
            r->pid = lr.pid;
            r->error = lr.error;
        }
        r->session = s;  // join may have rotated the refresh token
        post(WM_LAUNCHED, r);
    }).detach();
}

void pick_game_dir() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = app.wnd;
    bi.lpszTitle = L"Pick the Fable The Lost Chapters folder (the one with Fable.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!file_exists(std::wstring(path) + L"\\Fable.exe")) {
        MessageBoxW(app.wnd, L"That folder has no Fable.exe in it.", L"Chapters of the Lost", MB_ICONWARNING);
        return;
    }
    app.game_dir = path;
    app.cfg->game_dir = path;
    app.cfg->save();
    refresh_controls();
}

// ---- message handlers --------------------------------------------------------

void on_status(std::unique_ptr<api::Status> st) {
    app.status_busy = false;
    std::string keep = selected_region();
    if (keep.empty()) keep = app.cfg->region;
    app.status = *st;
    HWND lv = app.item(ID_REGIONS);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < app.status.regions.size(); ++i) {
        const api::Region& r = app.status.regions[i];
        std::wstring name = widen(r.name), state = widen(r.status);
        std::wstring pop = std::to_wstring(r.players) + L" / " + std::to_wstring(r.capacity);
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = int(i);
        it.pszText = &name[0];
        it.lParam = LPARAM(i);
        int row = ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, row, 1, &state[0]);
        ListView_SetItemText(lv, row, 2, &pop[0]);
        if (r.id == keep) ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (!st->reachable) set_text(ID_SERVER, L"Servers: unreachable (" + widen(st->error) + L")");
    else if (!st->online) set_text(ID_SERVER, L"Servers: down for maintenance");
    else set_text(ID_SERVER, L"Servers: online" + (st->motd.empty() ? L"" : L"   " + widen(st->motd)));
    // Patch once the gateway answers, and try again on later polls if it failed.
    if (st->reachable && !app.patched) start_patch();
    refresh_controls();
}

void on_news(std::unique_ptr<NewsResult> n) {
    std::wstring text;
    if (!n->ok) text = L"News is unavailable right now.\r\n";
    for (auto& i : n->items) {
        text += widen(i.title) + (i.date.empty() ? L"" : L"   (" + widen(i.date) + L")") + L"\r\n";
        std::wstring body = widen(i.body);
        for (size_t p = 0; (p = body.find(L'\n', p)) != std::wstring::npos; p += 2)
            if (p == 0 || body[p - 1] != L'\r') body.insert(p, L"\r");
        text += body + L"\r\n";
        if (!i.url.empty()) text += widen(i.url) + L"\r\n";
        text += L"\r\n";
    }
    if (text.empty()) text = L"No news yet.";
    set_text(ID_NEWS, text);
}

void on_login(std::unique_ptr<LoginResult> r) {
    app.logging_in = false;
    if (r->ok) {
        app.session = r->session;
        app.cfg->username = r->session.account;
        bool remember = SendMessageW(app.item(ID_REMEMBER), BM_GETCHECK, 0, 0) == BST_CHECKED;
        // A saved token (from an earlier "remember me") stays remembered.
        if (remember || !app.cfg->refresh_token.empty()) app.cfg->refresh_token = r->session.refresh_token;
        app.cfg->save();
        set_status(L"Logged in.");
    } else {
        app.session = api::Session{};
        if (!app.cfg->refresh_token.empty()) { app.cfg->refresh_token.clear(); app.cfg->save(); }
        set_status(widen(r->error));
    }
    refresh_controls();
}

void on_logout() {
    Config c = *app.cfg;
    api::Session s = app.session;
    std::thread([c, s] { api::logout(c, s); }).detach();
    app.session = api::Session{};
    app.cfg->refresh_token.clear();
    app.cfg->save();
    set_status(L"Logged out.");
    refresh_controls();
}

void on_patch_progress(std::unique_ptr<PatchProgress> p) {
    int pct = p->total ? int(p->done * 100 / p->total) : 0;
    SendMessageW(app.item(ID_PROGRESS), PBM_SETPOS, pct, 0);
    set_status(L"Downloading " + widen(p->file) + L"  " + std::to_wstring(pct) + L"%");
}

void on_patch_done(std::unique_ptr<PatchDone> d) {
    app.patching = false;
    SendMessageW(app.item(ID_PROGRESS), PBM_SETPOS, d->ok ? 100 : 0, 0);
    if (d->ok) {
        app.patched = true;
        app.client_version = d->version;
        app.cfg->client_version = d->version;
        app.cfg->save();
        if (d->restart) {
            set_status(L"Launcher updated. Restarting...");
            if (updater::restart_self()) { DestroyWindow(app.wnd); return; }
            set_status(L"Launcher updated. Close and reopen it to finish.");
        } else {
            set_status(L"Up to date (version " + widen(d->version) + L").");
        }
    } else {
        set_status(widen(d->error));
    }
    refresh_controls();
}

void on_launched(std::unique_ptr<Launched> r) {
    app.launching = false;
    app.session = r->session;
    if (!app.cfg->refresh_token.empty() && app.cfg->refresh_token != r->session.refresh_token) {
        app.cfg->refresh_token = r->session.refresh_token;
        app.cfg->save();
    }
    if (r->ok) {
        app.game_pid = r->pid;
        set_status(L"Fable is running. Have fun, hero.");
        ShowWindow(app.wnd, SW_MINIMIZE);
    } else {
        set_status(r->error);
        MessageBoxW(app.wnd, r->error.c_str(), L"Couldn't start Fable", MB_ICONERROR);
    }
    refresh_controls();
}

void build() {
    // 880 x 560 logical pixels.
    HWND t = add(L"STATIC", L"CHAPTERS OF THE LOST", SS_LEFT, ID_TITLE, 16, 12, 400, 36);
    SendMessageW(t, WM_SETFONT, reinterpret_cast<WPARAM>(app.title_font), TRUE);
    add(L"STATIC", L"Servers: checking...", SS_LEFT | SS_ENDELLIPSIS, ID_SERVER, 16, 50, 848, 20);

    add(L"STATIC", L"News", SS_LEFT, ID_NEWS_LABEL, 16, 78, 200, 18);
    add(L"EDIT", L"Loading news...", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_NEWS,
        16, 98, 500, 336, WS_EX_CLIENTEDGE);

    add(L"STATIC", L"Username", SS_LEFT, ID_USER_LABEL, 536, 78, 120, 18);
    add(L"EDIT", widen(app.cfg->username).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ID_USER, 536, 98, 328, 24, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"Password", SS_LEFT, ID_PASS_LABEL, 536, 128, 120, 18);
    add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, ID_PASS, 536, 148, 328, 24, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Remember me", BS_AUTOCHECKBOX | WS_TABSTOP, ID_REMEMBER, 536, 178, 140, 20);
    add(L"BUTTON", L"Log in", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_LOGIN, 536, 204, 160, 30);
    add(L"BUTTON", L"Create account", BS_PUSHBUTTON | WS_TABSTOP, ID_REGISTER, 704, 204, 160, 30);
    add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, ID_ACCOUNT, 536, 104, 328, 20);
    add(L"BUTTON", L"Log out", BS_PUSHBUTTON | WS_TABSTOP, ID_LOGOUT, 536, 130, 120, 28);
    if (!app.cfg->refresh_token.empty()) SendMessageW(app.item(ID_REMEMBER), BM_SETCHECK, BST_CHECKED, 0);

    add(L"STATIC", L"Region", SS_LEFT, ID_REGION_LABEL, 536, 244, 200, 18);
    HWND lv = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                  ID_REGIONS, 536, 264, 328, 170, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    struct { const wchar_t* name; int w; } cols[] = {{L"Region", 150}, {L"Status", 80}, {L"Players", 90}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i].name);
        c.cx = app.S(cols[i].w);
        ListView_InsertColumn(lv, i, &c);
    }

    add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, ID_GAMEDIR_LABEL, 16, 446, 640, 20);
    add(L"BUTTON", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, ID_GAMEDIR, 664, 442, 80, 26);
    add(L"STATIC", L"Starting...", SS_LEFT | SS_ENDELLIPSIS, ID_STATUS, 16, 480, 680, 20);
    add(PROGRESS_CLASSW, L"", 0, ID_PROGRESS, 16, 504, 680, 22);
    HWND play = add(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, ID_PLAY, 712, 476, 152, 52);
    SendMessageW(play, WM_SETFONT, reinterpret_cast<WPARAM>(app.title_font), TRUE);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            app.wnd = h;
            build();
            refresh_controls();
            start_status();
            start_news();
            if (!app.cfg->refresh_token.empty()) { app.session.refresh_token = app.cfg->refresh_token; app.session.account = app.cfg->username; start_refresh_login(); }
            SetTimer(h, kPollTimer, kPollMs, nullptr);
            SetTimer(h, kGameTimer, 2000, nullptr);
            return 0;
        case WM_TIMER:
            if (w == kPollTimer) start_status();
            if (w == kGameTimer && app.game_pid && running_fable() != app.game_pid) {
                app.game_pid = 0;
                set_status(L"Fable closed.");
                refresh_controls();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_LOGIN: start_login(false); break;
                case ID_REGISTER: start_login(true); break;
                case ID_LOGOUT: on_logout(); break;
                case ID_PLAY: start_play(); break;
                case ID_GAMEDIR: pick_game_dir(); break;
                case IDOK: if (IsWindowVisible(app.item(ID_LOGIN))) start_login(false); break;  // Enter in the password box
            }
            return 0;
        case WM_NOTIFY: {
            auto* n = reinterpret_cast<NMHDR*>(l);
            if (n->idFrom == ID_REGIONS && n->code == LVN_ITEMCHANGED) refresh_controls();
            if (n->idFrom == ID_REGIONS && n->code == NM_DBLCLK && IsWindowEnabled(app.item(ID_PLAY))) start_play();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(l) == app.item(ID_TITLE)) SetTextColor(dc, RGB(150, 90, 20));
            if (reinterpret_cast<HWND>(l) == app.item(ID_NEWS)) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_STATUS: on_status(std::unique_ptr<api::Status>(reinterpret_cast<api::Status*>(l))); return 0;
        case WM_NEWS: on_news(std::unique_ptr<NewsResult>(reinterpret_cast<NewsResult*>(l))); return 0;
        case WM_LOGIN: on_login(std::unique_ptr<LoginResult>(reinterpret_cast<LoginResult*>(l))); return 0;
        case WM_PATCH_PROGRESS: on_patch_progress(std::unique_ptr<PatchProgress>(reinterpret_cast<PatchProgress*>(l))); return 0;
        case WM_PATCH_DONE: on_patch_done(std::unique_ptr<PatchDone>(reinterpret_cast<PatchDone*>(l))); return 0;
        case WM_LAUNCHED: on_launched(std::unique_ptr<Launched>(reinterpret_cast<Launched*>(l))); return 0;
        case WM_DESTROY:
            app.wnd = nullptr;  // late worker posts fail and free their payloads
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int run_ui(Config& cfg) {
    app.cfg = &cfg;
    app.game_dir = find_game_dir(cfg.game_dir);
    app.client_version = cfg.client_version;
    updater::cleanup_old_launcher();

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HDC screen = GetDC(nullptr);
    app.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    app.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW big = ncm.lfMessageFont;
    big.lfHeight = -app.S(22);
    big.lfWeight = FW_BOLD;
    app.title_font = CreateFontIndirectW(&big);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"FableOnlineLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, app.S(880), app.S(544)};
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Chapters of the Lost Online", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, SW_SHOW);
    SetFocus(app.item(app.cfg->username.empty() ? ID_USER : ID_PASS));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(app.font);
    DeleteObject(app.title_font);
    return 0;
}
