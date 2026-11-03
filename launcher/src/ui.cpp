// The launcher window: news, account, realm list, patching and Play.
// Styled after Fable's own menus: a dark, warm backdrop, gold double-line
// frames with corner studs, serif type and gold buttons. Everything is drawn
// by us (owner-draw buttons, painted labels), so no Windows chrome shows.
// All network work runs on worker threads that post results back as WM_APP
// messages, so the window never freezes.
#include "ui.h"
#include "api.h"
#include "../../shared/art/big.h"
#include "../../shared/art/dxt.h"
#include "game.h"
#include "updater.h"
#include "util.h"
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <thread>

namespace {

enum : int {
    ID_NEWS = 100, ID_USER, ID_PASS, ID_REMEMBER, ID_LOGIN, ID_REGISTER, ID_LOGOUT,
    ID_REGIONS, ID_PLAY, ID_GAMEDIR,
    // Painted text, not windows:
    ID_STATUS, ID_SERVER, ID_MOTD, ID_ACCOUNT, ID_GAMEDIR_LABEL,
};

enum : UINT {
    WM_STATUS = WM_APP + 1, WM_NEWS, WM_LOGIN, WM_PATCH_PROGRESS, WM_PATCH_DONE, WM_LAUNCHED,
};
const UINT_PTR kPollTimer = 1;
const UINT kPollMs = 30000;
const UINT_PTR kGameTimer = 2;

// ---- palette (sampled from Fable's front end, see tools/re-notes/04-frontend-art.md) ----
const COLORREF kText = RGB(0xE9, 0xE4, 0xD6);
const COLORREF kHighlight = RGB(0xFF, 0xF6, 0xDC);
const COLORREF kMuted = RGB(0xC9, 0xC0, 0xA8);
const COLORREF kPewter = RGB(0x93, 0x8D, 0x7E);
const COLORREF kPewterOn = RGB(0xA2, 0x8F, 0x74);
const COLORREF kPewterDim = RGB(84, 80, 70);
const COLORREF kPewterDeep = RGB(46, 44, 38);
const COLORREF kChromeTop = RGB(0xF4, 0xF7, 0xFF);
const COLORREF kChromeMid = RGB(0x9A, 0xA3, 0xF4);
const COLORREF kChromeShadow = RGB(0x3D, 0x4A, 0x9A);
const COLORREF kField = RGB(10, 11, 10);
const COLORREF kPanelInk = RGB(13, 14, 12);   // matches the smoked panels
const COLORREF kSelect = RGB(70, 64, 50);
const COLORREF kGood = RGB(150, 196, 120);
const COLORREF kWarn = RGB(222, 180, 96);
const COLORREF kBad = RGB(214, 110, 90);
// Older names used by the news and message handlers.
const COLORREF kGoldBright = kHighlight, kGold = kPewterOn, kGoldDim = kPewter, kParchment = kText, kParchmentDim = kMuted;

// Pieces of Fable's menu art, loaded from the player's install.
struct Skin {
    bool loaded = false;
    fo::art::Image backdrop;
    fo::art::Image btn_off[3], btn_on[3];   // left cap, middle, right cap
    HBITMAP corner[4] = {};             // TL TR BL BR
    HBITMAP edge[4] = {};               // top, bottom, left, right
};

struct LoginResult { bool ok; api::Session session; std::string error; };
struct PatchProgress { std::string file; unsigned long long done, total; };
struct PatchDone { bool ok; bool restart; std::string version; std::string error; };
struct Launched { bool ok; api::Session session; std::wstring error; unsigned long pid; };
struct NewsResult { bool ok; std::vector<api::NewsItem> items; std::string error; };

struct App {
    Config* cfg = nullptr;
    HWND wnd = nullptr;
    int dpi = 96;
    HFONT title = nullptr, subtitle = nullptr, heading = nullptr, body = nullptr, small = nullptr,
          button = nullptr, play = nullptr, field = nullptr, news = nullptr;
    HBRUSH field_brush = nullptr;
    HBITMAP backdrop = nullptr;   // static art: background, frames, headings
    Skin skin;
    HWND hot = nullptr;           // button under the mouse
    int width = 0, height = 0;    // client size in pixels
    std::map<int, std::wstring> text;
    bool status_error = false;
    int progress = 0;             // 0..100
    bool remember = false;
    api::Session session;
    api::Status status;
    bool status_busy = false, logging_in = false, patching = false, patched = false, launching = false;
    std::wstring game_dir;
    std::string client_version;
    unsigned long game_pid = 0;

    HWND item(int id) const { return GetDlgItem(wnd, id); }
    int S(int v) const { return MulDiv(v, dpi, 96); }
    bool logged_in() const { return !session.access_token.empty() || !session.refresh_token.empty(); }
} app;

// Logical layout (96 dpi), 880 x 600.
const RECT kNewsFrame = {24, 118, 516, 492};
const RECT kHeroFrame = {536, 118, 856, 356};
const RECT kRealmFrame = {536, 364, 856, 492};
const RECT kUserBox = {560, 168, 832, 198};
const RECT kPassBox = {560, 222, 832, 252};

void reskin();
RECT scaled(const RECT& r) { return {app.S(r.left), app.S(r.top), app.S(r.right), app.S(r.bottom)}; }

template <class T> void post(UINT msg, T* payload) {
    if (!app.wnd) { delete payload; return; }
    if (!PostMessageW(app.wnd, msg, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void set_text(int id, const std::wstring& s) {
    if (id >= ID_STATUS) {
        if (app.text[id] == s) return;
        app.text[id] = s;
        InvalidateRect(app.wnd, nullptr, FALSE);
    } else {
        SetWindowTextW(app.item(id), s.c_str());
    }
}
void set_status(const std::wstring& s, bool error = false) {
    app.status_error = error;
    app.text[ID_STATUS].clear();
    set_text(ID_STATUS, s);
}

std::wstring get_text(int id) {
    HWND h = app.item(id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

std::wstring upper(std::wstring s) { CharUpperBuffW(&s[0], DWORD(s.size())); return s; }

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
    bool in = app.logged_in();
    for (int id : {ID_USER, ID_PASS, ID_REMEMBER, ID_LOGIN, ID_REGISTER})
        ShowWindow(app.item(id), in ? SW_HIDE : SW_SHOW);
    ShowWindow(app.item(ID_LOGOUT), in ? SW_SHOW : SW_HIDE);
    for (int id : {ID_LOGIN, ID_REGISTER, ID_USER, ID_PASS}) EnableWindow(app.item(id), !app.logging_in);
    set_text(ID_ACCOUNT, in ? widen(app.session.account) : L"");

    const api::Region* r = region_by_id(selected_region());
    bool running = app.game_pid != 0;
    bool can_play = in && app.patched && !app.patching && !app.launching && !running &&
                    !app.game_dir.empty() && r && r->joinable();
    EnableWindow(app.item(ID_PLAY), can_play);
    set_text(ID_PLAY, running ? L"In Albion" : app.launching ? L"Opening the Gate" : app.patching ? L"Updating" : L"Enter Albion");
    InvalidateRect(app.item(ID_PLAY), nullptr, FALSE);
    set_text(ID_GAMEDIR_LABEL, app.game_dir.empty() ? L"Fable: The Lost Chapters not found. Show me where it is:"
                                                    : L"Fable: " + app.game_dir);
    InvalidateRect(app.wnd, nullptr, FALSE);
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
    if (user.empty() || pass.empty()) { set_status(L"Enter a name and password.", true); return; }
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
    reskin();
    refresh_controls();
}

// ---- message handlers --------------------------------------------------------

void on_status(std::unique_ptr<api::Status> st) {
    app.status_busy = false;
    std::string keep = selected_region();
    if (keep.empty()) keep = app.cfg->region;
    app.status = *st;
    HWND lv = app.item(ID_REGIONS);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < app.status.regions.size(); ++i) {
        const api::Region& r = app.status.regions[i];
        std::wstring name = widen(r.name), state = r.status == "online" ? L"OPEN" : r.status == "maintenance" ? L"CLOSED"
                                                 : r.status == "offline" ? L"DOWN" : upper(widen(r.status));
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
    // One realm: pick it and show it as a card. Several: show the list.
    bool single = app.status.regions.size() == 1;
    if (single) ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowWindow(lv, single || app.status.regions.empty() ? SW_HIDE : SW_SHOW);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    std::wstring realm = st->regions.size() == 1 ? L"REALM" : L"REALMS";
    if (!st->reachable) { set_text(ID_SERVER, L"REALM UNREACHABLE"); set_text(ID_MOTD, widen(st->error)); }
    else if (!st->online) { set_text(ID_SERVER, realm + L" RESTING"); set_text(ID_MOTD, L"Down for maintenance"); }
    else { set_text(ID_SERVER, realm + L" OPEN"); set_text(ID_MOTD, widen(st->motd)); }
    // Patch once the gateway answers, and try again on later polls if it failed.
    if (st->reachable && !app.patched) start_patch();
    refresh_controls();
}

// Appends text to the news scroll in the given style.
void news_append(HWND re, const std::wstring& s, HFONT font, COLORREF color, bool bold) {
    LOGFONTW lf = {};
    GetObjectW(font, sizeof lf, &lf);
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD;
    cf.crTextColor = color;
    cf.dwEffects = bold ? CFE_BOLD : 0;
    lstrcpynW(cf.szFaceName, lf.lfFaceName, LF_FACESIZE);
    HDC dc = GetDC(nullptr);
    cf.yHeight = MulDiv(std::abs(lf.lfHeight), 1440, GetDeviceCaps(dc, LOGPIXELSY));
    ReleaseDC(nullptr, dc);
    int end = GetWindowTextLengthW(re);
    SendMessageW(re, EM_SETSEL, end, end);
    SendMessageW(re, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(re, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(s.c_str()));
}

void on_news(std::unique_ptr<NewsResult> n) {
    HWND re = app.item(ID_NEWS);
    SetWindowTextW(re, L"");
    if (!n->ok) news_append(re, L"The town crier is silent. News will return when the realms answer.\n", app.news, kParchmentDim, false);
    if (n->ok && n->items.empty()) news_append(re, L"No news from Albion yet.\n", app.news, kParchmentDim, false);
    for (auto& i : n->items) {
        news_append(re, widen(i.title) + L"\n", app.heading, kGoldBright, true);
        if (!i.date.empty()) news_append(re, widen(i.date) + L"\n", app.small, kGoldDim, false);
        news_append(re, widen(i.body) + L"\n", app.news, kParchment, false);
        if (!i.url.empty()) news_append(re, widen(i.url) + L"\n", app.small, kGold, false);
        news_append(re, L"\n", app.small, kParchment, false);
    }
    SendMessageW(re, EM_SETSEL, 0, 0);
    SendMessageW(re, WM_VSCROLL, SB_TOP, 0);
}

void on_login(std::unique_ptr<LoginResult> r) {
    app.logging_in = false;
    if (r->ok) {
        app.session = r->session;
        app.cfg->username = r->session.account;
        // A saved token (from an earlier "remember me") stays remembered.
        if (app.remember || !app.cfg->refresh_token.empty()) app.cfg->refresh_token = r->session.refresh_token;
        app.cfg->save();
        set_status(L"Welcome back, " + widen(r->session.account) + L".");
    } else {
        app.session = api::Session{};
        if (!app.cfg->refresh_token.empty()) { app.cfg->refresh_token.clear(); app.cfg->save(); }
        set_status(widen(r->error), true);
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
    app.progress = p->total ? int(p->done * 100 / p->total) : 0;
    set_status(L"Downloading " + widen(p->file) + L"   " + std::to_wstring(app.progress) + L"%");
}

void on_patch_done(std::unique_ptr<PatchDone> d) {
    app.patching = false;
    app.progress = d->ok ? 100 : 0;
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
            set_status(L"Ready. Version " + widen(d->version) + L".");
        }
    } else {
        set_status(widen(d->error), true);
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
        set_status(L"Fable is running. Good luck, hero.");
        ShowWindow(app.wnd, SW_MINIMIZE);
    } else {
        set_status(r->error, true);
        MessageBoxW(app.wnd, r->error.c_str(), L"Couldn't start Fable", MB_ICONERROR);
    }
    refresh_controls();
}

// ---- drawing -------------------------------------------------------------------

int CALLBACK font_found(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) {
    *reinterpret_cast<bool*>(p) = true;
    return 0;
}

HFONT make_font(const wchar_t* const* faces, int px, int weight) {
    // First installed face wins. Cinzel and Cormorant Garamond ship inside the exe.
    HDC dc = GetDC(nullptr);
    const wchar_t* pick = faces[0];
    for (const wchar_t* const* f = faces; *f; ++f) {
        LOGFONTW lf = {};
        lstrcpynW(lf.lfFaceName, *f, LF_FACESIZE);
        lf.lfCharSet = DEFAULT_CHARSET;
        bool found = false;
        EnumFontFamiliesExW(dc, &lf, font_found, reinterpret_cast<LPARAM>(&found), 0);
        if (found) { pick = *f; break; }
    }
    ReleaseDC(nullptr, dc);
    return CreateFontW(-app.S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, pick);
}

const wchar_t* kSerif[] = {L"Cormorant Garamond", L"Palatino Linotype", L"Book Antiqua", L"Georgia", L"DejaVu Serif", nullptr};
const wchar_t* kDisplay[] = {L"Cinzel", L"Trajan Pro", L"Palatino Linotype", L"Georgia", L"DejaVu Serif", nullptr};

// Loads the fonts embedded as RCDATA 201.. (see launcher.rc).
void load_embedded_fonts() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    for (int id = 201; id <= 204; ++id) {
        HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
        if (!r) continue;
        HGLOBAL g = LoadResource(inst, r);
        DWORD n = 0;
        if (g) AddFontMemResourceEx(LockResource(g), SizeofResource(inst, r), nullptr, &n);
    }
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int w = 1) {
    HPEN pen = CreatePen(PS_SOLID, w, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void outline(HDC dc, RECT r, COLORREF c) {
    line(dc, r.left, r.top, r.right - 1, r.top, c);
    line(dc, r.right - 1, r.top, r.right - 1, r.bottom - 1, c);
    line(dc, r.right - 1, r.bottom - 1, r.left, r.bottom - 1, c);
    line(dc, r.left, r.bottom - 1, r.left, r.top, c);
}

void stud(HDC dc, int cx, int cy, int r, COLORREF c, COLORREF edge) {
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    SetDCBrushColor(dc, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

void vgradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom) {
    TRIVERTEX v[2] = {
        {r.left, r.top, COLOR16(GetRValue(top) << 8), COLOR16(GetGValue(top) << 8), COLOR16(GetBValue(top) << 8), 0},
        {r.right, r.bottom, COLOR16(GetRValue(bottom) << 8), COLOR16(GetGValue(bottom) << 8), COLOR16(GetBValue(bottom) << 8), 0}};
    GRADIENT_RECT g = {0, 1};
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
}

// Text with letter spacing, centred on cx.
SIZE spaced(HDC dc, const std::wstring& s, int cx, int y, HFONT font, COLORREF color, int extra, bool shadow = true) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextCharacterExtra(dc, extra);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &sz);
    if (shadow) {
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutW(dc, cx - sz.cx / 2 + 1, y + 1, s.c_str(), int(s.size()));
    }
    SetTextColor(dc, color);
    TextOutW(dc, cx - sz.cx / 2, y, s.c_str(), int(s.size()));
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, old);
    return sz;
}

// Text with a 1px dark shadow, for legibility over the forest.
void shadow_text(HDC dc, const std::wstring& s, RECT r, UINT fmt, COLORREF c) {
    RECT sh = r;
    OffsetRect(&sh, 1, 1);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, s.c_str(), -1, &sh, fmt);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, fmt);
}

// fo::art::Image -> premultiplied 32-bit DIB for AlphaBlend.
HBITMAP to_bitmap(const fo::art::Image& img) {
    if (img.empty()) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!b) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    for (size_t i = 0; i < img.px.size(); ++i) {
        uint32_t c = img.px[i], a = c >> 24;
        px[i] = a << 24 | (((c >> 16) & 255) * a / 255) << 16 | (((c >> 8) & 255) * a / 255) << 8 | ((c & 255) * a / 255);
    }
    return b;
}

void blend(HDC dc, HBITMAP bmp, int x, int y, int w, int h) {
    if (!bmp) return;
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(src, bmp);
    BITMAP bm;
    GetObjectW(bmp, sizeof bm, &bm);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, w, h, src, 0, 0, bm.bmWidth, bm.bmHeight, bf);
    SelectObject(src, old);
    DeleteDC(src);
}

bool get_image(fo::art::Bank& bank, const std::string& name, fo::art::Image& out) {
    fo::art::Texture t;
    return bank.load(name, t) && fo::art::decode(t, out);
}

// Fable's menu art, read from the player's own install (nothing is shipped).
void load_skin() {
    Skin& k = app.skin;
    k = Skin{};
    if (app.game_dir.empty()) return;
    fo::art::Bank bank;
    if (!bank.open(app.game_dir + L"\\Data\\graphics\\pc\\frontend.big")) return;
    // Title backdrop: six tiles (256, 256, 128 per row, two rows), plus a sunbeam layer added on top.
    fo::art::Image bg;
    bg.w = 640;
    bg.h = 512;
    bg.px.assign(size_t(bg.w) * bg.h, 0xFF000000u);
    const int xs[3] = {0, 256, 512};
    for (int i = 0; i < 6; ++i) {
        fo::art::Image tile, beam;
        if (!get_image(bank, "FRONTEND_BG_0" + std::to_string(i + 1) + "_SPRITE", tile)) return;
        bool has_beam = get_image(bank, "FORREST_SUNBEAM_1_" + std::to_string(i + 1), beam) && beam.w == tile.w && beam.h == tile.h;
        for (int y = 0; y < tile.h && 256 * (i / 3) + y < bg.h; ++y)
            for (int x = 0; x < tile.w && xs[i % 3] + x < bg.w; ++x) {
                uint32_t c = tile.px[size_t(y) * tile.w + x];
                if (has_beam) {
                    uint32_t b = beam.px[size_t(y) * beam.w + x], a = b >> 24;
                    auto add = [&](int sh) {
                        uint32_t v = ((c >> sh) & 255) + ((b >> sh) & 255) * a / 255 * 7 / 10;
                        return (v > 255 ? 255u : v) << sh;
                    };
                    c = add(16) | add(8) | add(0);
                }
                bg.px[size_t(256 * (i / 3) + y) * bg.w + xs[i % 3] + x] = c | 0xFF000000u;
            }
    }
    k.backdrop = std::move(bg);
    const char* parts[3] = {"_L", "_M", "_R"};
    for (int i = 0; i < 3; ++i) {
        get_image(bank, std::string("FE_BUTTON_OFF") + parts[i], k.btn_off[i]);
        get_image(bank, std::string("FE_BUTTON_ON") + parts[i], k.btn_on[i]);
    }
    const char* corners[4] = {"UI_TABLE_TEST_TL_FE", "UI_TABLE_TEST_TR_FE", "UI_TABLE_TEST_BL_FE", "UI_TABLE_TEST_BR_FE"};
    for (int i = 0; i < 4; ++i) {
        fo::art::Image c;
        if (get_image(bank, corners[i], c)) k.corner[i] = to_bitmap(fo::art::resize(c, app.S(c.w), app.S(c.h)));
    }
    const char* edges[4] = {"UI_TABLE_TEST_H_T_FE", "UI_TABLE_TEST_H_B_FE", "UI_TABLE_TEST_V_L_FE", "UI_TABLE_TEST_V_R_FE"};
    for (int i = 0; i < 4; ++i) {
        fo::art::Image e;
        if (get_image(bank, edges[i], e)) k.edge[i] = to_bitmap(fo::art::resize(e, app.S(e.w), app.S(e.h)));
    }
    k.loaded = !k.btn_off[0].empty() && !k.btn_on[0].empty();
}

// A Fable-style panel: smoked glass over the backdrop, the game's studded
// pewter frame (or a drawn stand-in), and a heading set over the top edge.
void frame(HDC dc, const RECT& logical, const wchar_t* heading) {
    RECT r = scaled(logical);
    const Skin& k = app.skin;
    if (k.corner[0] && k.edge[0]) {
        BITMAP cb;
        GetObjectW(k.corner[0], sizeof cb, &cb);
        int c = cb.bmWidth, e = app.S(8);
        // Edges are 8x8 tiles; the frame line sits in the middle of the corner art.
        int off = 0;
        blend(dc, k.edge[0], r.left + c, r.top + off, r.right - r.left - 2 * c, e);
        blend(dc, k.edge[1], r.left + c, r.bottom - e - off, r.right - r.left - 2 * c, e);
        blend(dc, k.edge[2], r.left + off, r.top + c, e, r.bottom - r.top - 2 * c);
        blend(dc, k.edge[3], r.right - e - off, r.top + c, e, r.bottom - r.top - 2 * c);
        blend(dc, k.corner[0], r.left, r.top, c, c);
        blend(dc, k.corner[1], r.right - c, r.top, c, c);
        blend(dc, k.corner[2], r.left, r.bottom - c, c, c);
        blend(dc, k.corner[3], r.right - c, r.bottom - c, c, c);
    } else {
        RECT in = r;
        InflateRect(&in, -app.S(3), -app.S(3));
        outline(dc, r, kPewterDeep);
        outline(dc, in, kPewter);
        int s = app.S(4);
        for (POINT p : {POINT{in.left, in.top}, POINT{in.right - 1, in.top}, POINT{in.left, in.bottom - 1}, POINT{in.right - 1, in.bottom - 1}})
            stud(dc, p.x, p.y, s, kMuted, kPewterDeep);
    }
    if (heading) spaced(dc, heading, (r.left + r.right) / 2, r.top + app.S(12), app.heading, kHighlight, app.S(4));
}

// Background art, built once per size/skin: the forest backdrop (or a dark
// olive vignette without the game's art), smoked panels, frames and the title.
void build_backdrop() {
    int w = app.width, h = app.height;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    if (app.backdrop) DeleteObject(app.backdrop);
    app.backdrop = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto* px = static_cast<uint32_t*>(bits);

    const fo::art::Image& src = app.skin.backdrop;
    if (!src.empty()) {
        // Cover the window, keeping the art's aspect, centred.
        float scale = std::max(float(w) / src.w, float(h) / src.h);
        int sw = int(src.w * scale + 0.5f), sh = int(src.h * scale + 0.5f);
        fo::art::Image big = fo::art::resize(src, sw, sh);
        int ox = (sw - w) / 2, oy = (sh - h) / 3;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) px[size_t(y) * w + x] = big.px[size_t(y + oy) * sw + x + ox];
    } else {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float dx = (x - w * 0.5f) / (w * 0.7f), dy = (y - h * 0.4f) / (h * 0.8f);
                float v = 1.0f - 1.1f * (dx * dx + dy * dy);
                if (v < 0.08f) v = 0.08f;
                unsigned hsh = (unsigned(x) * 73856093u) ^ (unsigned(y) * 19349663u);
                hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                float g = float(hsh & 7) - 3.5f;
                auto c = [](float f) { return uint32_t(f < 0 ? 0 : f > 255 ? 255 : f); };
                px[size_t(y) * w + x] = c((42 + g) * v) << 16 | c((45 + g) * v) << 8 | c((35 + g) * v);
            }
    }
    // Darken the top for the title, the bottom for the footer, and the panels.
    auto shade = [&](RECT r, float keep, uint32_t tint) {
        for (int y = std::max(0, int(r.top)); y < std::min(h, int(r.bottom)); ++y)
            for (int x = std::max(0, int(r.left)); x < std::min(w, int(r.right)); ++x) {
                uint32_t& p = px[size_t(y) * w + x];
                auto ch = [&](int s) { return (uint32_t(((p >> s) & 255) * keep) + ((tint >> s) & 255)) << s; };
                p = ch(16) | ch(8) | ch(0);
            }
    };
    for (int y = 0; y < h; ++y) {
        float t = y < app.S(110) ? 0.45f + 0.55f * y / app.S(110) : y > h - app.S(110) ? 0.35f + 0.65f * (h - y) / app.S(110) : 1.0f;
        for (int x = 0; x < w; ++x) {
            uint32_t& p = px[size_t(y) * w + x];
            p = uint32_t(((p >> 16) & 255) * t) << 16 | uint32_t(((p >> 8) & 255) * t) << 8 | uint32_t((p & 255) * t);
        }
    }
    for (const RECT* f : {&kNewsFrame, &kHeroFrame, &kRealmFrame}) shade(scaled(*f), 0.22f, 0x0A0B0Au);

    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, app.backdrop);
    SetBkMode(dc, TRANSPARENT);

    // Title in Fable's chrome blue: a gradient clipped to the lettering.
    const wchar_t* t = L"Chapters of the Lost";
    int tx = app.S(36), ty = app.S(14);
    SelectObject(dc, app.title);
    SIZE tsz;
    GetTextExtentPoint32W(dc, t, lstrlenW(t), &tsz);
    SetTextColor(dc, RGB(0, 0, 0));
    for (int d = 1; d <= 3; ++d) TextOutW(dc, tx + d, ty + d, t, lstrlenW(t));  // drop shadow
    BeginPath(dc);
    TextOutW(dc, tx, ty, t, lstrlenW(t));
    EndPath(dc);
    SelectClipPath(dc, RGN_COPY);
    RECT top = {tx, ty, tx + tsz.cx, ty + tsz.cy / 2}, bot = {tx, ty + tsz.cy / 2, tx + tsz.cx, ty + tsz.cy};
    vgradient(dc, top, kChromeTop, kChromeMid);
    vgradient(dc, bot, kChromeShadow, kChromeMid);
    SelectClipRgn(dc, nullptr);
    BeginPath(dc);
    TextOutW(dc, tx, ty, t, lstrlenW(t));
    EndPath(dc);
    HPEN rim = CreatePen(PS_SOLID, 1, RGB(20, 24, 52));
    HGDIOBJ op = SelectObject(dc, rim);
    StrokePath(dc);
    SelectObject(dc, op);
    DeleteObject(rim);
    int sub_y = ty + tsz.cy - app.S(2);
    SIZE ssz = spaced(dc, L"ONLINE", tx + tsz.cx / 2, sub_y, app.subtitle, kText, app.S(12));
    int ly = sub_y + ssz.cy / 2;
    line(dc, tx + app.S(6), ly, tx + tsz.cx / 2 - ssz.cx / 2 - app.S(12), ly, kPewter);
    line(dc, tx + tsz.cx / 2 + ssz.cx / 2 + app.S(6), ly, tx + tsz.cx - app.S(6), ly, kPewter);
    stud(dc, tx + app.S(6), ly, app.S(2), kMuted, kPewterDeep);
    stud(dc, tx + tsz.cx - app.S(6), ly, app.S(2), kMuted, kPewterDeep);

    frame(dc, kNewsFrame, L"TIDINGS");
    frame(dc, kHeroFrame, L"HERO");
    frame(dc, kRealmFrame, nullptr);  // heading depends on how many realms there are; see paint()

    SelectObject(dc, old);
    DeleteDC(dc);
}

void reskin() {
    load_skin();
    build_backdrop();
    InvalidateRect(app.wnd, nullptr, TRUE);
    for (HWND c = GetWindow(app.wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) InvalidateRect(c, nullptr, TRUE);
}

void field_box(HDC dc, const RECT& logical) {
    RECT r = scaled(logical);
    fill(dc, r, kField);
    outline(dc, r, kPewterDim);
    line(dc, r.left + 1, r.top + 1, r.right - 1, r.top + 1, RGB(0, 0, 0));
}

void paint(HDC target) {
    HDC dc = CreateCompatibleDC(target);
    HBITMAP buf = CreateCompatibleBitmap(target, app.width, app.height);
    HGDIOBJ oldbuf = SelectObject(dc, buf);
    HDC art_dc = CreateCompatibleDC(target);
    HGDIOBJ oldart = SelectObject(art_dc, app.backdrop);
    BitBlt(dc, 0, 0, app.width, app.height, art_dc, 0, 0, SRCCOPY);
    SelectObject(art_dc, oldart);
    DeleteDC(art_dc);
    SetBkMode(dc, TRANSPARENT);

    // Realm status, top right.
    const std::wstring& srv = app.text[ID_SERVER];
    COLORREF dot = srv.size() > 5 && srv.compare(srv.size() - 5, 5, L" OPEN") == 0 ? kGood : srv.empty() ? kMuted : kBad;
    std::wstring srv_text = srv.empty() ? L"SEEKING THE REALM" : srv;
    RECT sr = {app.width - app.S(380), app.S(26), app.width - app.S(36), app.S(48)};
    SelectObject(dc, app.heading);
    SetTextCharacterExtra(dc, app.S(3));
    SIZE ssz;
    GetTextExtentPoint32W(dc, srv_text.c_str(), int(srv_text.size()), &ssz);
    shadow_text(dc, srv_text, sr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, kText);
    SetTextCharacterExtra(dc, 0);
    stud(dc, sr.right - ssz.cx - app.S(14), (sr.top + sr.bottom) / 2, app.S(4), dot, RGB(0, 0, 0));
    RECT mr = {app.width - app.S(420), app.S(50), app.width - app.S(36), app.S(72)};
    SelectObject(dc, app.body);
    shadow_text(dc, app.text[ID_MOTD], mr, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS, kMuted);

    // Realm panel: a card for a single realm, column heads over the list for several.
    RECT rf = scaled(kRealmFrame);
    int rcx = (rf.left + rf.right) / 2;
    if (app.status.regions.size() == 1) {
        const api::Region& g = app.status.regions[0];
        spaced(dc, L"REALM", rcx, rf.top + app.S(12), app.heading, kHighlight, app.S(4));
        RECT nr = {rf.left + app.S(16), rf.top + app.S(34), rf.right - app.S(16), rf.top + app.S(66)};
        SelectObject(dc, app.subtitle);
        shadow_text(dc, widen(g.name), nr, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, kHighlight);
        COLORREF sc = g.status == "online" ? kGood : g.status == "busy" ? kWarn : g.status == "full" ? kBad : kPewter;
        std::wstring state = g.status == "online" ? L"OPEN" : g.status == "maintenance" ? L"CLOSED"
                           : g.status == "offline" ? L"DOWN" : upper(widen(g.status));
        spaced(dc, state, rcx, rf.top + app.S(70), app.small, sc, app.S(3));
        // Population as a pewter gauge.
        RECT gauge = {rf.left + app.S(40), rf.top + app.S(94), rf.right - app.S(40), rf.top + app.S(100)};
        fill(dc, gauge, kField);
        outline(dc, gauge, kPewterDeep);
        if (g.capacity > 0 && g.players > 0) {
            RECT f = gauge;
            InflateRect(&f, -1, -1);
            f.right = f.left + int((f.right - f.left) * std::min(1.0, double(g.players) / g.capacity));
            vgradient(dc, f, kHighlight, kPewterOn);
        }
        RECT pr = {rf.left + app.S(16), rf.top + app.S(102), rf.right - app.S(16), rf.top + app.S(122)};
        SelectObject(dc, app.small);
        shadow_text(dc, std::to_wstring(g.players) + L" of " + std::to_wstring(g.capacity) + L" heroes in Albion", pr,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER, kMuted);
    } else {
        spaced(dc, app.status.regions.empty() ? L"REALM" : L"REALMS", rcx, rf.top + app.S(12), app.heading, kHighlight, app.S(4));
        if (!app.status.regions.empty()) {
            SelectObject(dc, app.small);
            SetTextColor(dc, kPewter);
            SetTextCharacterExtra(dc, app.S(2));
            TextOutW(dc, app.S(560), app.S(388), L"REALM", 5);
            TextOutW(dc, app.S(684), app.S(388), L"STATE", 5);
            TextOutW(dc, app.S(756), app.S(388), L"HEROES", 6);
            SetTextCharacterExtra(dc, 0);
            line(dc, app.S(556), app.S(406), app.S(836), app.S(406), kPewterDeep);
        } else {
            RECT er = {rf.left + app.S(16), rf.top + app.S(40), rf.right - app.S(16), rf.bottom - app.S(16)};
            SelectObject(dc, app.body);
            shadow_text(dc, L"Seeking the realm...", er, DT_CENTER | DT_SINGLELINE | DT_VCENTER, kMuted);
        }
    }

    // Hero panel.
    if (!app.logged_in()) {
        SelectObject(dc, app.small);
        SetTextColor(dc, kMuted);
        SetTextCharacterExtra(dc, app.S(2));
        TextOutW(dc, app.S(560), app.S(150), L"NAME", 4);
        TextOutW(dc, app.S(560), app.S(204), L"PASSWORD", 8);
        SetTextCharacterExtra(dc, 0);
        field_box(dc, kUserBox);
        field_box(dc, kPassBox);
    } else {
        spaced(dc, L"SIGNED IN AS", app.S(696), app.S(176), app.small, kMuted, app.S(3));
        RECT nr = {app.S(556), app.S(198), app.S(836), app.S(244)};
        SelectObject(dc, app.subtitle);
        shadow_text(dc, app.text[ID_ACCOUNT], nr, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, kHighlight);
    }

    // Footer: game folder, status line and progress.
    RECT gr = {app.S(28), app.S(512), app.S(470), app.S(530)};
    SelectObject(dc, app.small);
    shadow_text(dc, app.text[ID_GAMEDIR_LABEL], gr, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS, app.game_dir.empty() ? kWarn : kPewter);
    RECT st = {app.S(28), app.S(532), app.S(560), app.S(554)};
    SelectObject(dc, app.body);
    shadow_text(dc, app.text[ID_STATUS], st, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, app.status_error ? kBad : kText);
    RECT bar = {app.S(28), app.S(560), app.S(560), app.S(568)};
    fill(dc, bar, kField);
    outline(dc, bar, kPewterDeep);
    if (app.progress > 0) {
        RECT f = bar;
        InflateRect(&f, -1, -1);
        f.right = f.left + (f.right - f.left) * app.progress / 100;
        vgradient(dc, f, kHighlight, kPewterOn);
    }

    BitBlt(target, 0, 0, app.width, app.height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbuf);
    DeleteObject(buf);
    DeleteDC(dc);
}

// Fable's pewter scroll bar, 9-sliced to the button's size.
void draw_scroll_button(HDC dc, const RECT& r, bool on) {
    const fo::art::Image* parts = on ? app.skin.btn_on : app.skin.btn_off;
    int h = r.bottom - r.top, w = r.right - r.left;
    int cap = parts[0].w * h / std::max(1, parts[0].h);
    if (cap * 2 > w) cap = w / 2;
    struct { const fo::art::Image* img; int x, cw; } pieces[3] = {{&parts[0], r.left, cap}, {&parts[1], r.left + cap, w - 2 * cap}, {&parts[2], r.right - cap, cap}};
    for (auto& p : pieces) {
        if (p.cw <= 0) continue;
        HBITMAP b = to_bitmap(fo::art::resize(*p.img, p.cw, h));
        blend(dc, b, p.x, r.top, p.cw, h);
        if (b) DeleteObject(b);
    }
}

void draw_button(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool pressed = d->itemState & ODS_SELECTED;
    bool disabled = d->itemState & ODS_DISABLED;
    bool hot = d->hwndItem == app.hot;
    wchar_t label[64] = {};
    GetWindowTextW(d->hwndItem, label, 64);

    // Paint the backdrop behind the control first, so art with alpha sits on the forest.
    RECT wr;
    GetWindowRect(d->hwndItem, &wr);
    MapWindowPoints(nullptr, app.wnd, reinterpret_cast<POINT*>(&wr), 2);
    HDC art_dc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(art_dc, app.backdrop);
    BitBlt(dc, 0, 0, r.right, r.bottom, art_dc, wr.left, wr.top, SRCCOPY);
    SelectObject(art_dc, old);
    DeleteDC(art_dc);
    SetBkMode(dc, TRANSPARENT);

    if (d->CtlID == ID_REMEMBER) {
        int b = app.S(14), y = (r.top + r.bottom - b) / 2;
        RECT box = {r.left, y, r.left + b, y + b};
        fill(dc, box, kField);
        outline(dc, box, hot ? kMuted : kPewterDim);
        if (app.remember) stud(dc, box.left + b / 2, box.top + b / 2, b / 2 - app.S(4), kHighlight, kPewter);
        RECT tr = {r.left + b + app.S(8), r.top, r.right, r.bottom};
        SelectObject(dc, app.body);
        shadow_text(dc, label, tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER, hot ? kText : kMuted);
        return;
    }
    if (d->CtlID == ID_GAMEDIR || d->CtlID == ID_REGISTER) {
        SelectObject(dc, d->CtlID == ID_GAMEDIR ? app.small : app.body);
        shadow_text(dc, label, r, (d->CtlID == ID_GAMEDIR ? DT_RIGHT : DT_CENTER) | DT_SINGLELINE | DT_VCENTER,
                    pressed || hot ? kHighlight : kMuted);
        return;
    }

    bool big = d->CtlID == ID_PLAY;
    bool lit = !disabled && (hot || pressed || big);
    if (app.skin.loaded) {
        draw_scroll_button(dc, r, lit);
    } else {
        RECT bar = r;
        InflateRect(&bar, -app.S(10), -(r.bottom - r.top) / 5);
        vgradient(dc, bar, lit ? RGB(196, 180, 150) : RGB(164, 158, 142), lit ? RGB(112, 96, 72) : RGB(92, 88, 78));
        outline(dc, bar, RGB(30, 28, 24));
        for (int x : {int(bar.left), int(bar.right) - 1}) stud(dc, x, (bar.top + bar.bottom) / 2, app.S(5), kMuted, RGB(30, 28, 24));
    }
    if (disabled) {
        // Dim the art: Fable greys out what you can't pick.
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 150, 0};
        HDC black = CreateCompatibleDC(dc);
        HBITMAP one = CreateCompatibleBitmap(dc, 1, 1);
        HGDIOBJ ob = SelectObject(black, one);
        SetPixel(black, 0, 0, RGB(0, 0, 0));
        AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, black, 0, 0, 1, 1, bf);
        SelectObject(black, ob);
        DeleteObject(one);
        DeleteDC(black);
    }
    RECT tr = r;
    if (pressed) OffsetRect(&tr, 0, 1);
    std::wstring t = upper(label);
    SelectObject(dc, big ? app.play : app.button);
    SetTextCharacterExtra(dc, app.S(big ? 3 : 2));
    // Dark halo so cream text reads on the light pewter.
    SetTextColor(dc, RGB(20, 18, 14));
    for (POINT o : {POINT{-1, 0}, POINT{1, 0}, POINT{0, -1}, POINT{0, 1}, POINT{1, 1}}) {
        RECT s = tr;
        OffsetRect(&s, o.x, o.y);
        DrawTextW(dc, t.c_str(), -1, &s, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(dc, disabled ? kPewter : lit ? kHighlight : kText);
    DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(dc, 0);
}

LRESULT realm_customdraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            HWND lv = cd->nmcd.hdr.hwndFrom;
            int row = int(cd->nmcd.dwItemSpec);
            bool sel = ListView_GetItemState(lv, row, LVIS_SELECTED) & LVIS_SELECTED;
            cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
            cd->clrTextBk = sel ? kSelect : kField;
            size_t idx = static_cast<size_t>(cd->nmcd.lItemlParam);
            const api::Region* r = idx < app.status.regions.size() ? &app.status.regions[idx] : nullptr;
            COLORREF c = sel ? kHighlight : kText;
            if (cd->iSubItem == 1 && r)
                c = r->status == "online" ? kGood : r->status == "busy" ? kWarn : r->status == "full" ? kBad : kPewter;
            if (cd->iSubItem == 2) c = sel ? kHighlight : kMuted;
            if (r && !r->joinable() && cd->iSubItem != 1) c = kPewterDim;
            cd->clrText = c;
            return CDRF_DODEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

// Hover highlight for the owner-drawn buttons.
LRESULT CALLBACK hover_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE && app.hot != h) {
        HWND prev = app.hot;
        app.hot = h;
        if (prev) InvalidateRect(prev, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
    } else if (m == WM_MOUSELEAVE && app.hot == h) {
        app.hot = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefSubclassProc(h, m, w, l);
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, HFONT font, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, app.S(x), app.S(y), app.S(w), app.S(h),
                             app.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (style & BS_OWNERDRAW) SetWindowSubclass(c, hover_proc, 1, 0);
    return c;
}

void build() {
    // News: a rich edit, so titles can stand out. Transparent, so the smoked panel shows.
    HWND news = add(MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, ID_NEWS,
                    44, 146, 456, 330, app.news);
    SendMessageW(news, EM_SETBKGNDCOLOR, 0, kPanelInk);
    SendMessageW(news, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(app.S(4), app.S(4)));
    SetWindowTheme(news, L"DarkMode_Explorer", nullptr);
    SetWindowTextW(news, L"");
    news_append(news, L"Listening for the town crier...", app.news, kMuted, false);

    add(L"EDIT", widen(app.cfg->username).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ID_USER,
        kUserBox.left + 8, kUserBox.top + 5, kUserBox.right - kUserBox.left - 16, 20, app.field);
    HWND p = add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, ID_PASS,
                 kPassBox.left + 8, kPassBox.top + 5, kPassBox.right - kPassBox.left - 16, 20, app.field);
    SendMessageW(p, EM_SETPASSWORDCHAR, 0x2022, 0);
    add(L"BUTTON", L"Remember me", BS_OWNERDRAW | WS_TABSTOP, ID_REMEMBER, 560, 256, 160, 20, app.body);
    add(L"BUTTON", L"Log in", BS_OWNERDRAW | WS_TABSTOP, ID_LOGIN, 548, 280, 296, 44, app.button);
    add(L"BUTTON", L"No hero yet? Create an account", BS_OWNERDRAW | WS_TABSTOP, ID_REGISTER, 560, 324, 272, 20, app.body);
    add(L"BUTTON", L"Log out", BS_OWNERDRAW | WS_TABSTOP, ID_LOGOUT, 548, 280, 296, 44, app.button);
    app.remember = !app.cfg->refresh_token.empty();

    HWND lv = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | WS_TABSTOP,
                  ID_REGIONS, 552, 410, 292, 76, app.body);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(lv, kField);
    ListView_SetTextBkColor(lv, kField);
    ListView_SetTextColor(lv, kText);
    SetWindowTheme(lv, L"DarkMode_Explorer", nullptr);
    SendMessageW(lv, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    // Taller rows: a list view takes its row height from its image list.
    HIMAGELIST rows = ImageList_Create(1, app.S(24), ILC_COLOR, 1, 0);
    ListView_SetImageList(lv, rows, LVSIL_SMALL);
    struct { const wchar_t* name; int w; } cols[] = {{L"Realm", 128}, {L"State", 72}, {L"Heroes", 88}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i].name);
        c.cx = app.S(cols[i].w);
        ListView_InsertColumn(lv, i, &c);
    }

    add(L"BUTTON", L"Change folder", BS_OWNERDRAW | WS_TABSTOP, ID_GAMEDIR, 474, 511, 86, 20, app.small);
    add(L"BUTTON", L"Enter Albion", BS_OWNERDRAW | WS_TABSTOP, ID_PLAY, 576, 506, 288, 68, app.play);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            app.wnd = h;
            RECT rc;
            GetClientRect(h, &rc);
            app.width = rc.right;
            app.height = rc.bottom;
            load_skin();
            build_backdrop();
            build();
            refresh_controls();
            start_status();
            start_news();
            if (!app.cfg->refresh_token.empty()) { app.session.refresh_token = app.cfg->refresh_token; app.session.account = app.cfg->username; start_refresh_login(); }
            SetTimer(h, kPollTimer, kPollMs, nullptr);
            SetTimer(h, kGameTimer, 2000, nullptr);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
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
                case ID_REMEMBER: app.remember = !app.remember; InvalidateRect(app.item(ID_REMEMBER), nullptr, FALSE); break;
                case IDOK: if (IsWindowVisible(app.item(ID_LOGIN))) start_login(false); break;  // Enter in a text box
            }
            return 0;
        case WM_DRAWITEM:
            draw_button(reinterpret_cast<DRAWITEMSTRUCT*>(l));
            return TRUE;
        case WM_NOTIFY: {
            auto* n = reinterpret_cast<NMHDR*>(l);
            if (n->idFrom == ID_REGIONS && n->code == NM_CUSTOMDRAW) return realm_customdraw(reinterpret_cast<NMLVCUSTOMDRAW*>(l));
            if (n->idFrom == ID_REGIONS && n->code == LVN_ITEMCHANGED) refresh_controls();
            if (n->idFrom == ID_REGIONS && n->code == NM_DBLCLK && IsWindowEnabled(app.item(ID_PLAY))) start_play();
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, kParchment);
            SetBkColor(dc, kField);
            return reinterpret_cast<LRESULT>(app.field_brush);
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

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HDC screen = GetDC(nullptr);
    app.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    load_embedded_fonts();
    app.title = make_font(kDisplay, 42, FW_BOLD);
    app.subtitle = make_font(kDisplay, 20, FW_NORMAL);
    app.heading = make_font(kDisplay, 14, FW_BOLD);
    app.button = make_font(kDisplay, 15, FW_BOLD);
    app.play = make_font(kDisplay, 19, FW_BOLD);
    app.body = make_font(kSerif, 17, FW_MEDIUM);
    app.news = make_font(kSerif, 17, FW_MEDIUM);
    app.small = make_font(kSerif, 14, FW_MEDIUM);
    app.field = make_font(kSerif, 18, FW_MEDIUM);
    app.field_brush = CreateSolidBrush(kField);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ChaptersOfTheLostLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, app.S(880), app.S(600)};
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Chapters of the Lost Online", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    // Dark title bar (Windows 10 20H1+) and a matching caption colour (Windows 11).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    COLORREF caption = RGB(10, 11, 10);
    DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof caption);
    ShowWindow(h, SW_SHOW);
    SetFocus(app.item(app.cfg->username.empty() ? ID_USER : ID_PASS));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HFONT f : {app.title, app.subtitle, app.heading, app.button, app.play, app.body, app.news, app.small, app.field})
        DeleteObject(f);
    DeleteObject(app.field_brush);
    DeleteObject(app.backdrop);
    return 0;
}
