#include "modsettings.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../devkit.h"
#include "../display/frame_probe.h"
#include "../game/hook.h"
#include "../game/pause_menu.h"
#include "../game/player_modes.h"
#include "../display/mouse_look.h"
#include "../game/spawns.h"
#include "../log.h"
#include "../overlay/renderer.h"
#include "../sync.h"
#include "settings_model.h"

namespace fo::modsettings {
namespace {

using overlay::Face;
using overlay::Renderer;

// Palette sampled from Fable's front end (tools/re-notes/04-frontend-art.md).
constexpr uint32_t kText = 0xFFE9E4D6, kTextLit = 0xFFFFF6DC, kMuted = 0xFFC9C0A8, kLabel = 0xFFB3AA92, kGold = 0xFFE8C877;

enum Id {
    // Extras
    kShoulder = 1, kSmooth, kAim, kRedTarget, kCrosshair, kNameTags, kHudScale, kHudBottom,
    // Developer: tools
    kEnemyType = 90, kSpawn1, kSpawn3, kDespawnNearest, kDespawnNear,
    kTestKit = 100, kGold10k, kHeal, kGod, kSpawning, kStandIn, kShowPos, kTagSelf,
    // Developer: logs and capture
    kStatusLog, kHealthLog, kCapture, kFrameTools,
    // Developer: camera experiments and keys
    kLatch, kAssist, kAutoCenter, kMouseSmooth, kLowLatency, kHotkeys,
};

const std::vector<std::string> kOnOff = {"Off", "On"};
const float kHudScales[] = {-1, 0, 1.2f, 1.4f, 1.6f, 1.8f, 2.0f};
const float kBottomScales[] = {0, 1.2f, 1.4f, 1.6f, 1.8f};
const int kSmoothFrames[] = {0, -1, 2, 4};
// Server dev spawns: "" lets the server pick from the monsters it already runs.
const char* const kEnemyDefs[] = {"", "CREATURE_BANDIT_GRUNT_LEVEL1", "CREATURE_BANDIT_ARCHER_LEVEL1", "CREATURE_HOBBE_BASIC",
                                  "CREATURE_BALVERINE_EASY", "CREATURE_MINION_WARDOG"};
HWND g_window = nullptr;
constexpr int kInGameMenuMode = 43;
bool g_pause_lit = false, g_pause_mouse_was_down = false;

std::mutex g_mu;  // guards everything below
Model g_model;
bool g_built = false;
std::atomic<bool> g_open{false};
std::atomic<DWORD> g_block_until{0};
std::atomic<int> g_open_request{-1};
float g_px = -1, g_py = -1;
bool g_pointer_seen = false;
DWORD g_last_wm_mouse = 0;
bool g_dinput_button = false;
std::vector<int> g_pending;   // actions for the game thread
std::string g_flash;          // a short note after an action
DWORD g_flash_at = 0;

Row toggle(int id, const char* label, const char* help, bool restart = false) {
    Row r;
    r.id = id;
    r.label = label;
    r.help = help;
    r.choices = kOnOff;
    r.restart = restart;
    return r;
}
Row choice(int id, const char* label, const char* help, std::vector<std::string> choices, bool restart = false) {
    Row r;
    r.id = id;
    r.label = label;
    r.help = help;
    r.choices = std::move(choices);
    r.restart = restart;
    return r;
}
Row action(int id, const char* label, const char* help) {
    Row r;
    r.id = id;
    r.label = label;
    r.help = help;
    return r;
}

template <size_t N, typename T>
int nearest(const T (&table)[N], T v) {
    int best = 0;
    for (size_t i = 1; i < N; ++i)
        if (std::fabs(static_cast<double>(table[i] - v)) < std::fabs(static_cast<double>(table[best] - v))) best = static_cast<int>(i);
    return best;
}

void build() {   // caller holds g_mu
    std::vector<Page> pages;
    Page extras;
    extras.title = "EXTRAS";
    extras.rows = {
        choice(kShoulder, "Camera shoulder", "Which side of the hero the camera sits, so the view centre isn't on your back.",
               {"Left", "Right", "Centre"}),
        toggle(kSmooth, "Camera smoothing", "Glides the camera between Fable's camera steps, so it doesn't jump while you turn."),
        toggle(kAim, "Aim at crosshair", "Melee, spells and target lock pick what you're looking at, not what's in front of the hero."),
        toggle(kRedTarget, "Red target in combat", "With a weapon or spell out, whatever you target glows red, villagers too."),
        choice(kCrosshair, "Crosshair", "A mark at the centre of the view while you play.", {"Off", "Ring", "Dot", "Cross"}),
        toggle(kNameTags, "Name tags", "Names above other heroes."),
        choice(kHudScale, "HUD size", "Health bar, minimap and menus. Automatic sizes them as on a 900-line screen.",
               {"Fable's own", "Automatic", "x1.2", "x1.4", "x1.6", "x1.8", "x2.0"}, true),
        choice(kHudBottom, "Bottom HUD size", "The bar along the bottom edge, drawn at its own size.",
               {"Same as HUD", "x1.2", "x1.4", "x1.6", "x1.8"}),
    };
    pages.push_back(std::move(extras));
    if (config().dev) {
        Page dev;
        dev.title = "DEVELOPER";
        dev.rows = {
            choice(kEnemyType, "Enemy to spawn", "Which monster the spawn buttons ask the server for.",
                   {"Random", "Bandit", "Bandit archer", "Hobbe", "Balverine", "Minion war dog"}),
            action(kSpawn1, "Spawn enemy (server)", "10-20 m ahead of you, facing you, run by the server. Needs a gm or admin account."),
            action(kSpawn3, "Spawn 3 enemies (server)", "Three side by side, 10-20 m ahead of you. Needs a gm or admin account."),
            action(kDespawnNearest, "Remove nearest enemy (server)", "Removes the closest monster within 50 m for everyone. Needs a gm or admin account."),
            action(kDespawnNear, "Remove enemies near me (server)", "Removes every monster within 30 m for everyone. Needs a gm or admin account."),
            action(kTestKit, "Give test gear", "Sword of Aeons and the platinum set (worn), 100,000 gold and a full heal."),
            action(kGold10k, "Give 10,000 gold", "Local game only: the server ledger is not changed."),
            action(kHeal, "Heal to full", "Tops up the hero's health in the local game."),
            toggle(kGod, "God mode", "Keeps the hero's health topped up in the local game."),
            choice(kSpawning, "Local monster spawning", "Fable's own creature generators. Off is what online play uses.", {"Off", "On"}),
            action(kStandIn, "Spawn or remove test stand-in", "A test hero puppet next to you (needs FABLE_ONLINE_STANDIN_TEST=1)."),
            toggle(kShowPos, "Show position", "The hero's position and area at the top of the screen."),
            toggle(kTagSelf, "Name tag on own hero", "To check name tag placement when playing alone."),
            action(kStatusLog, "Write online status to log", "Connection, ping, entities and health go to fable_online.log."),
            action(kHealthLog, "Write hero health to log", "Health and max health go to fable_online.log."),
            action(kCapture, "Capture one frame", "The next frame's Direct3D calls go to fable_online.log (needs Frame capture tools)."),
            toggle(kFrameTools, "Frame capture tools", "Hooks Direct3D for frame captures.", true),
            toggle(kLatch, "Camera latch (experimental)", "Turns the view with the mouse every frame between world updates."),
            toggle(kAssist, "Fable camera assist", "Fable's own camera swing toward targets and the hero's facing.", true),
            toggle(kAutoCenter, "Fable camera auto-centre", "Fable eases the camera pitch back while you walk.", true),
            choice(kMouseSmooth, "Mouse smoothing", "Mouse-look frames averaged.", {"Off", "Fable's own", "2 frames", "4 frames"}, true),
            toggle(kLowLatency, "Low latency", "Keeps the CPU at most one frame ahead of the graphics card."),
            toggle(kHotkeys, "Dev hotkeys", "F6 spawning, F7 status, F8 stand-in, F9 test gear, F10 capture or health."),
        };
        pages.push_back(std::move(dev));
    }
    g_model.set_pages(std::move(pages));
    g_built = true;
}

// Reads every row's value from the live settings (they may have changed elsewhere, such as
// the shoulder key or the F6 hotkey).
void sync_values() {   // caller holds g_mu
    const Config& c = config();
    g_model.set_value(kShoulder, c.camera_shoulder < 0 ? 0 : c.camera_shoulder > 0 ? 1 : 2);
    g_model.set_value(kSmooth, c.camera_smooth);
    g_model.set_value(kAim, c.aim_at_crosshair);
    g_model.set_value(kRedTarget, c.red_target);
    g_model.set_value(kCrosshair, static_cast<int>(c.crosshair));
    g_model.set_value(kNameTags, c.tags_enabled);
    g_model.set_value(kHudScale, nearest(kHudScales, c.hud_scale));
    g_model.set_value(kHudBottom, nearest(kBottomScales, c.hud_bottom_scale));
    g_model.set_value(kGod, c.god_mode);
    g_model.set_value(kSpawning, game::spawns::suppressed() ? 0 : 1);
    g_model.set_value(kShowPos, c.show_position);
    g_model.set_value(kTagSelf, c.tags_self);
    g_model.set_value(kFrameTools, c.frame_probe);
    g_model.set_value(kLatch, c.camera_latch);
    g_model.set_value(kAssist, c.camera_assist);
    g_model.set_value(kAutoCenter, c.camera_auto_center);
    g_model.set_value(kMouseSmooth, nearest(kSmoothFrames, c.mouse_smoothing));
    g_model.set_value(kLowLatency, c.low_latency);
    g_model.set_value(kHotkeys, c.dev_hotkeys);
}

std::wstring num(float v) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%.2f", static_cast<double>(v));
    return buf;
}

void set_bool(bool& field, const wchar_t* sec, const wchar_t* key, int v) {
    field = v != 0;
    config_write(sec, key, field ? L"1" : L"0");
}

void flash(const char* s) {   // caller holds g_mu
    g_flash = s;
    g_flash_at = GetTickCount();
}

void apply(int id, int v) {   // caller holds g_mu
    Config& c = config_edit();
    switch (id) {
    case kShoulder: {
        float mag = std::fabs(c.camera_shoulder);
        if (mag < 0.05f) mag = 0.5f;
        c.camera_shoulder = v == 0 ? -mag : v == 1 ? mag : 0.0f;
        config_write(L"display", L"camera_shoulder", num(c.camera_shoulder));
        break;
    }
    case kSmooth: set_bool(c.camera_smooth, L"display", L"camera_smooth", v); break;
    case kAim: set_bool(c.aim_at_crosshair, L"display", L"aim_at_crosshair", v); break;
    case kRedTarget: set_bool(c.red_target, L"display", L"red_target", v); break;
    case kCrosshair: {
        static const wchar_t* names[] = {L"off", L"ring", L"dot", L"cross"};
        c.crosshair = static_cast<Crosshair>(v);
        config_write(L"overlay", L"crosshair", names[v & 3]);
        break;
    }
    case kNameTags: set_bool(c.tags_enabled, L"tags", L"enabled", v); break;
    case kHudScale: {
        // Read when the game starts: only the ini changes now.
        const float s = kHudScales[v];
        config_write(L"display", L"hud_scale", s < 0 ? L"off" : s == 0 ? L"auto" : num(s));
        break;
    }
    case kHudBottom:
        c.hud_bottom_scale = kBottomScales[v];
        config_write(L"display", L"hud_bottom_scale", num(c.hud_bottom_scale));
        break;
    case kGod: set_bool(c.god_mode, L"dev", L"god_mode", v); break;
    case kSpawning: g_pending.push_back(kSpawning); break;   // the game thread compares and toggles
    case kShowPos: set_bool(c.show_position, L"dev", L"show_position", v); break;
    case kTagSelf: set_bool(c.tags_self, L"tags", L"self", v); break;
    case kFrameTools: config_write(L"display", L"frame_probe", v ? L"1" : L"0"); break;   // at start-up only
    case kLatch: set_bool(c.camera_latch, L"display", L"camera_latch", v); break;
    case kAssist: config_write(L"display", L"camera_assist", v ? L"1" : L"0"); break;
    case kAutoCenter: config_write(L"display", L"camera_auto_center", v ? L"1" : L"0"); break;
    case kMouseSmooth: {
        static const wchar_t* names[] = {L"off", L"game", L"2", L"4"};
        config_write(L"display", L"mouse_smoothing", names[v & 3]);
        break;
    }
    case kLowLatency: set_bool(c.low_latency, L"display", L"low_latency", v); break;
    case kHotkeys: set_bool(c.dev_hotkeys, L"dev", L"hotkeys", v); break;
    default: return;
    }
    const Row* r = g_model.find(id);
    fo::log("modsettings: %s = %s%s", r ? r->label.c_str() : "?", r && !r->choices.empty() ? r->choices[r->value].c_str() : "?",
            r && r->restart ? " (after restart)" : "");
    if (r && r->restart) flash("Saved. Applies the next time the game starts.");
}

void act(int id) {   // caller holds g_mu
    switch (id) {
    case kCapture:
        if (!display::probe::ready()) { flash("Turn on Frame capture tools and restart the game first."); return; }
        g_model.close();   // capture the game's frame, not this screen
        g_open = false;
        g_block_until = GetTickCount() + 250;
        display::probe::request_capture();
        fo::log("modsettings: frame capture requested");
        return;
    case kSpawn1:
    case kSpawn3:
    case kDespawnNearest:
    case kDespawnNear:
        g_pending.push_back(id);
        flash("Asking the server...");
        return;
    default:
        g_pending.push_back(id);
        flash("Done. Details are in fable_online.log.");
        return;
    }
}

// Game thread, from game::hook::on_tick.
void run_pending() {
    std::vector<int> work;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        work.swap(g_pending);
    }
    for (int id : work) {
        switch (id) {
        case kTestKit: devkit::test_kit(); break;
        case kGold10k: devkit::give_gold(10000); break;
        case kHeal: devkit::heal_full(); break;
        case kHealthLog: devkit::log_health(); break;
        case kStatusLog: sync::dev_key(VK_F7); break;
        case kStandIn: sync::dev_key(VK_F8); break;
        case kSpawn1:
        case kSpawn3: {
            int type = 0;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                const Row* r = g_model.find(kEnemyType);
                type = r ? r->value : 0;
            }
            sync::dev_spawn(kEnemyDefs[type >= 0 && type < 6 ? type : 0], id == kSpawn3 ? 3 : 1);
            break;
        }
        case kDespawnNearest: sync::dev_despawn(false); break;
        case kDespawnNear: sync::dev_despawn(true); break;
        case kSpawning: {
            int want = 0;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                const Row* r = g_model.find(kSpawning);
                want = r ? r->value : 0;
            }
            const bool on = !game::spawns::suppressed();
            if ((want == 1) != on) sync::dev_key(VK_F6);
            fo::log("modsettings: local monster spawning %s", game::spawns::suppressed() ? "off" : "on");
            break;
        }
        default: break;
        }
    }
}

void handle(const Event& e) {   // caller holds g_mu
    switch (e.kind) {
    case Event::Changed: apply(e.id, e.value); break;
    case Event::Action: act(e.id); break;
    case Event::Closed:
        g_open = false;
        g_block_until = GetTickCount() + 250;
        fo::log("modsettings: closed");
        break;
    default: break;
    }
}

void open_locked(int page) {   // caller holds g_mu
    if (!g_built) build();
    sync_values();
    if (page == kDeveloper && !config().dev) page = kExtras;
    g_model.open(page);
    g_open = true;
    g_flash.clear();
    g_pointer_seen = false;
    fo::log("modsettings: open (%s)", page == kDeveloper ? "Developer" : "Extras");
}

std::string value_text(const Row& r) {
    return r.choices.empty() ? std::string() : "<  " + r.choices[r.value] + "  >";
}

void text_centred(Renderer& r, float cx, float y, const std::string& s, uint32_t c, Face f) {
    r.text(cx - r.measure(s, f) / 2, y, s, c, f);
}

void mouse_moved_by(LONG dx, LONG dy, bool button) {   // caller holds g_mu
    if (GetTickCount() - g_last_wm_mouse > 500) {   // only when the window gets no mouse messages
        g_px = std::fmin(std::fmax(g_px + dx, 0.0f), g_model.screen_w());
        g_py = std::fmin(std::fmax(g_py + dy, 0.0f), g_model.screen_h());
        if (dx || dy) g_pointer_seen = true;
        g_model.pointer(g_px, g_py);
        if (button && !g_dinput_button) handle(g_model.click(g_px, g_py));
    }
    g_dinput_button = button;
}

}  // namespace

void install() {
    if (!game::hook::install()) return;
    game::hook::on_tick(run_pending, 50);
}

void open(int page) {
    std::lock_guard<std::mutex> lock(g_mu);
    open_locked(page);
}

void close() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_open) return;
    g_model.close();
    handle({Event::Closed, 0, 0});
}

bool is_open() { return g_open.load(); }

void notify(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_flash = text;
    g_flash_at = GetTickCount();
}

// On Fable's pause screen: one of its pewter buttons at the bottom left, clear of Fable's own
// list. Fable draws its own pointer there, so the button follows the Windows cursor and button.
bool fable_menu_up() {
    game::player_modes::poll();
    // Player mode 43 is the in-game menu (Inventory ... Options, Return To Game): seen in the
    // log as "player mode 1 ended; player mode 43 started" each time Esc opened it. Mode 23
    // is the separate pause screen.
    return game::pause_menu::open() || game::player_modes::running(kInGameMenuMode);
}

bool button_wanted() { return !g_open && g_window && fable_menu_up(); }

void draw_pause_button(Renderer& r, bool) {
    const bool pause = game::pause_menu::open();
    const bool menu = game::player_modes::running(kInGameMenuMode);
    const bool paused = fable_menu_up();
    const bool show = !g_open && paused && g_window;
    static bool was_paused = false;
    static int logged = 0;
    if (paused != was_paused && logged < 40) {
        ++logged;
        fo::log("modsettings: Fable menu %s (pause screen %d, in-game menu %d, player modes %08X%08X)%s", paused ? "up" : "gone",
                pause, menu, static_cast<unsigned>(game::player_modes::running_mask() >> 32),
                static_cast<unsigned>(game::player_modes::running_mask()), paused && show ? "; MOD SETTINGS button drawn" : "");
    }
    was_paused = paused;
    if (!show) { g_pause_mouse_was_down = false; return; }
    // An extra entry at the foot of Fable's in-game menu list, in the same place, size and
    // look as Fable's own rows. Fable lays that list out on a 480-line grid scaled to the
    // screen height and anchored left: the first row's top edge at 75.3, 15 per row, the
    // bar from x 4.7 to 116.7, text from x 15.3 (measured at 2560x1440). Seven rows
    // (Inventory .. Return To Game), so ours is the eighth.
    const float u = r.screen_h() / 480.0f;
    const float pitch = 15.0f * u;
    const float x = std::round(4.7f * u), w = std::round(112.0f * u);
    const float y = std::round((75.3f + 7 * 15.0f) * u), h = std::round(pitch - u / 3);
    POINT pt;
    RECT rc;
    bool inside = false;
    if (GetCursorPos(&pt) && ScreenToClient(g_window, &pt) && GetClientRect(g_window, &rc) && rc.right > 0 && rc.bottom > 0) {
        const float px = pt.x * r.screen_w() / rc.right, py = pt.y * r.screen_h() / rc.bottom;
        inside = px >= x && px < x + w && py >= y && py < y + h;
    }
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (inside && down && !g_pause_mouse_was_down && GetForegroundWindow() == g_window) {
        fo::log("modsettings: opened from Fable's menu");
        g_pause_mouse_was_down = down;
        open(kExtras);
        return;
    }
    g_pause_mouse_was_down = down;
    g_pause_lit = inside;
    // Colours sampled from Fable's rows: pewter grey, warm gold when under the pointer.
    if (inside) r.pill(x, y, w, h, 0xFFB49A60, 0xFF8E7444, 0xFF020101);
    else r.pill(x, y, w, h, 0xFF99907F, 0xFF7E766A, 0xFF020101);
    const float target = h * 0.66f;   // Fable's capitals fill about two thirds of the row
    const float scale = target / std::max(1.0f, r.line_height(Face::Title));
    r.text_scaled(std::round(15.3f * u), y + (h - target) / 2, "Mod Settings", 0xFFFFFFFF, Face::Title, scale);
}

bool input_blocked() { return g_open.load() || GetTickCount() < g_block_until.load(); }

void draw(Renderer& r) {
    if (!g_open) return;
    std::lock_guard<std::mutex> lock(g_mu);
    const float sw = r.screen_w(), sh = r.screen_h();
    Metrics m;
    m.line = r.line_height(Face::Chat);
    m.title = r.line_height(Face::Title);
    g_model.resize(sw, sh, m);
    if (g_px < 0) { g_px = sw / 2; g_py = sh / 2; }

    r.rect(0, 0, sw, sh, 0x8C000000);   // dim the game behind, as Fable's pause screen does
    const Box p = g_model.panel();
    r.panel(p.x, p.y, p.w, p.h, 1.0f);
    text_centred(r, p.x + p.w / 2, g_model.title_y(), "MOD SETTINGS", kTextLit, Face::Title);

    const auto& pages = g_model.pages();
    const int page = g_model.page();
    if (pages.size() > 1) {
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            const Box t = g_model.tab(i);
            const bool on = i == page || g_model.hover() == -3 - i;
            r.bar(t.x, t.y, t.w, t.h, 1.0f, on, true);
            text_centred(r, t.x + t.w / 2, t.y + (t.h - m.line) / 2, pages[i].title, on ? kTextLit : kText, Face::Chat);
        }
    }

    const auto& rows = pages.empty() ? std::vector<Row>() : pages[page].rows;
    const int first = g_model.first_visible(), last = std::min(static_cast<int>(rows.size()), first + g_model.visible_rows());
    if (!rows.empty()) {
        const Box top = g_model.row(first);
        r.rule(top.x, top.y - 8, top.w, 0.8f);
    }
    for (int i = first; i < last; ++i) {
        const Row& row = rows[i];
        const Box b = g_model.row(i);
        const bool focus = i == g_model.focus();
        const float ty = b.y + (b.h - m.line) / 2;
        if (row.choices.empty()) {
            // An action: one of Fable's pewter buttons, the width of the label column.
            const Box v = g_model.value_box(i);
            const float bw = b.w - v.w - 12;
            r.bar(b.x, b.y + 1, bw, b.h - 2, 1.0f, focus, true);
            text_centred(r, b.x + bw / 2, ty, row.label, focus ? kTextLit : kText, Face::Chat);
            continue;
        }
        if (focus) r.soft_rect(b.x + 4, b.y + 2, b.w - 8, b.h - 4, 4, 0x38C9C0A8);
        std::string label = row.label + (row.restart ? " *" : "");
        r.text(b.x + 12, ty, label, focus ? kTextLit : kText, Face::Chat);
        const Box v = g_model.value_box(i);
        text_centred(r, v.x + v.w / 2, ty, value_text(row), focus ? kGold : kMuted, Face::Chat);
    }
    if (first > 0) text_centred(r, p.x + p.w - 40, g_model.row(first).y - m.line, "^", kLabel, Face::Small);
    if (last < static_cast<int>(rows.size())) text_centred(r, p.x + p.w - 40, g_model.help_y() - m.line * 0.9f, "more v", kLabel, Face::Small);

    std::string help;
    if (!g_flash.empty() && GetTickCount() - g_flash_at < 4000) help = g_flash;
    else if (g_model.focus() >= 0 && g_model.focus() < static_cast<int>(rows.size())) {
        const Row& row = rows[g_model.focus()];
        help = row.help;
        if (row.restart) help += " Applies the next time the game starts.";
    } else {
        help = "Settings are saved as you change them.";
    }
    const float help_w = p.w - 2 * m.line * 1.4f;
    auto lines = r.wrap(help, help_w, Face::Small);
    float hy = g_model.help_y() - (lines.size() > 1 ? r.line_height(Face::Small) * 0.5f : 0);
    for (size_t i = 0; i < lines.size() && i < 2; ++i, hy += r.line_height(Face::Small))
        text_centred(r, p.x + p.w / 2, hy, lines[i], kMuted, Face::Small);

    const Box back = g_model.back_button();
    const bool back_on = g_model.focus() == -1;
    r.bar(back.x, back.y, back.w, back.h, 1.0f, back_on, true);
    text_centred(r, back.x + back.w / 2, back.y + (back.h - m.line) / 2, "BACK", back_on ? kTextLit : kText, Face::Chat);

    if (g_pointer_seen) r.pointer(g_px, g_py, std::fmax(18.0f, m.line * 1.3f));
}

void draw_hud(Renderer& r, bool hero_valid, float x, float y, float z, unsigned area) {
    const Config& c = config();
    if (!c.dev || !c.show_position || g_open) return;
    char buf[128];
    if (hero_valid) std::snprintf(buf, sizeof buf, "x %.1f   y %.1f   z %.1f   area %08X", x, y, z, area);
    else std::snprintf(buf, sizeof buf, "no hero position yet");
    const std::string s = buf;
    const float w = r.measure(s, Face::Small);
    r.rect(r.screen_w() / 2 - w / 2 - 10, 6, w + 20, r.line_height(Face::Small) + 8, 0x90000000);
    r.text(r.screen_w() / 2 - w / 2, 10, s, kText, Face::Small);
}

bool on_message(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    g_window = w;
    if (!g_open) {
        // A click on the pause screen's MOD SETTINGS button is ours, not Fable's.
        if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) && g_pause_lit && game::pause_menu::open()) return true;
        // Swallow the tail of the key that closed the screen, so Fable doesn't act on it too.
        if ((msg == WM_KEYUP || msg == WM_CHAR) && GetTickCount() < g_block_until.load()) return true;
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    auto to_bb = [&](LPARAM l) {
        RECT rc;
        GetClientRect(w, &rc);
        const float cw = static_cast<float>(rc.right - rc.left), ch = static_cast<float>(rc.bottom - rc.top);
        g_px = static_cast<float>(static_cast<short>(LOWORD(l))) * (cw > 0 ? g_model.screen_w() / cw : 1);
        g_py = static_cast<float>(static_cast<short>(HIWORD(l))) * (ch > 0 ? g_model.screen_h() / ch : 1);
        g_last_wm_mouse = GetTickCount();
        g_pointer_seen = true;
    };
    switch (msg) {
    case WM_MOUSEMOVE:
        to_bb(lp);
        g_model.pointer(g_px, g_py);
        return true;
    case WM_LBUTTONDOWN:
        to_bb(lp);
        handle(g_model.click(g_px, g_py));
        return true;
    case WM_MOUSEWHEEL:
        g_model.scroll(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        return true;
    case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK:
        return true;
    case WM_KEYDOWN: {
        Key k;
        switch (wp) {
        case VK_UP: k = Key::Up; break;
        case VK_DOWN: k = Key::Down; break;
        case VK_LEFT: k = Key::Left; break;
        case VK_RIGHT: k = Key::Right; break;
        case VK_PRIOR: k = Key::PageUp; break;
        case VK_NEXT: k = Key::PageDown; break;
        case VK_TAB: k = (GetKeyState(VK_SHIFT) & 0x8000) ? Key::BackTab : Key::Tab; break;
        case VK_RETURN: case VK_SPACE: k = Key::Enter; break;
        case VK_ESCAPE: case VK_BACK: k = Key::Escape; break;
        default: return true;
        }
        handle(g_model.key(k));
        return true;
    }
    case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN:
        return msg != WM_SYSKEYDOWN || wp != VK_F4;   // keep Alt+F4
    }
    return false;
}

void on_mouse_state(const void* state, DWORD size) {
    if (!g_open || !state || size < 16) return;
    LONG d[3];
    std::memcpy(d, state, sizeof d);
    const bool button = (static_cast<const uint8_t*>(state)[12] & 0x80) != 0;
    std::lock_guard<std::mutex> lock(g_mu);
    mouse_moved_by(d[0], d[1], button);
}

void on_mouse_data(const void* objects, DWORD object_size, DWORD count) {
    if (!g_open || !objects || object_size < 8) return;
    // DIDEVICEOBJECTDATA: DWORD dwOfs, DWORD dwData, ... ; offsets 0 = X, 4 = Y, 12 = button 0.
    LONG dx = 0, dy = 0;
    bool button = false, have_button = false;
    for (DWORD i = 0; i < count; ++i) {
        const uint8_t* o = static_cast<const uint8_t*>(objects) + i * object_size;
        DWORD ofs, data;
        std::memcpy(&ofs, o, 4);
        std::memcpy(&data, o + 4, 4);
        if (ofs == 0) dx += static_cast<LONG>(data);
        else if (ofs == 4) dy += static_cast<LONG>(data);
        else if (ofs == 12) { button = (data & 0x80) != 0; have_button = true; }
    }
    std::lock_guard<std::mutex> lock(g_mu);
    mouse_moved_by(dx, dy, have_button ? button : g_dinput_button);
}

}  // namespace fo::modsettings
