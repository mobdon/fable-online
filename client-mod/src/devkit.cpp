// Test-only tools, run from the Developer page of Mod settings (and F9 / F10 when [dev] hotkeys=1):
// test gear, gold, a full heal, god mode and a health line in the log.
//
// Everything goes through Fable's own script interface, the object its cutscene scripts use for
// GiveHero, GiveGold and HeroWear. The calls run inside the game window's message handler, so
// they are on the game's main thread, between frames.
#include "devkit.h"

#include <windows.h>

#include "config.h"
#include "game/hook.h"
#include "log.h"

namespace {

// Fable.exe has no ASLR, so these are fixed. See tools/re-notes/04-script-interface.md.
constexpr uintptr_t kScriptInterface = 0x0143E8F8;  // CGameScriptInterface*
constexpr uintptr_t kStrCtor = 0x0099EBF0;          // CCharString(const char*, int len)
constexpr uintptr_t kStrDtor = 0x0099EAE0;          // ~CCharString()

constexpr unsigned kGiveHeroObject = 0x1E4;  // (const CCharString&, int, bool)
constexpr unsigned kAfterGiveHero = 0x1E8;   // (const CCharString&): the step GiveHero runs next
constexpr unsigned kGiveGold = 0x1F8;        // (int)
constexpr unsigned kHeroWear = 0x2F8;        // (const CCharString&)
constexpr unsigned kHeroHealth = 0x404;      // () -> float
constexpr unsigned kHeroMaxHealth = 0x408;   // () -> float
constexpr unsigned kChangeHeroHealth = 0x41C;  // (float delta, bool, bool)

struct CCharString {
    void* rep = nullptr;
    explicit CCharString(const char* s) {
        reinterpret_cast<void(__thiscall*)(CCharString*, const char*, int)>(kStrCtor)(this, s, -1);
    }
    ~CCharString() { reinterpret_cast<void(__thiscall*)(CCharString*)>(kStrDtor)(this); }
    CCharString(const CCharString&) = delete;
    CCharString& operator=(const CCharString&) = delete;
};

void* iface() { return *reinterpret_cast<void**>(kScriptInterface); }

template <typename Fn>
Fn slot(void* obj, unsigned offset) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[offset / 4]);
}

const char* const kGear[] = {
    "OBJECT_SWORD_OF_AEONS",         "OBJECT_HERO_HELMET_PLATINUM",   "OBJECT_HERO_SHIRT_PLATINUM",
    "OBJECT_HERO_GLOVES_PLATINUM",   "OBJECT_HERO_TROUSERS_PLATINUM", "OBJECT_HERO_BOOTS_PLATINUM",
};
const char* const kWear[] = {
    "OBJECT_HERO_HELMET_PLATINUM", "OBJECT_HERO_SHIRT_PLATINUM",  "OBJECT_HERO_GLOVES_PLATINUM",
    "OBJECT_HERO_TROUSERS_PLATINUM", "OBJECT_HERO_BOOTS_PLATINUM",
};

void log_health_of(void* s) {
    float hp = slot<float(__thiscall*)(void*)>(s, kHeroHealth)(s);
    float max = slot<float(__thiscall*)(void*)>(s, kHeroMaxHealth)(s);
    fo::log("devkit: hero health %.1f / %.1f", hp, max);
}

void give_test_kit() {
    void* s = iface();
    if (!s) { fo::log("devkit: script interface not ready (still on the menu?)"); return; }
    fo::log("devkit: giving the test kit");
    log_health_of(s);

    // No gold: the server's ledger owns money online, and gold added here only inflates
    // Fable's local purse.

    for (const char* name : kGear) {
        fo::log("devkit:   give %s", name);
        CCharString str(name);
        slot<void(__thiscall*)(void*, CCharString*, int, bool)>(s, kGiveHeroObject)(s, &str, 0, false);
        slot<void(__thiscall*)(void*, CCharString*)>(s, kAfterGiveHero)(s, &str);
    }
    for (const char* name : kWear) {
        fo::log("devkit:   wear %s", name);
        CCharString str(name);
        slot<void(__thiscall*)(void*, CCharString*)>(s, kHeroWear)(s, &str);
    }

    float hp = slot<float(__thiscall*)(void*)>(s, kHeroHealth)(s);
    float max = slot<float(__thiscall*)(void*)>(s, kHeroMaxHealth)(s);
    fo::log("devkit:   heal +%.1f", max - hp);
    slot<void(__thiscall*)(void*, float, bool, bool)>(s, kChangeHeroHealth)(s, max - hp, true, false);
    log_health_of(s);
    fo::log("devkit: test kit done");
}

void heal_full_of(void* s) {
    float hp = slot<float(__thiscall*)(void*)>(s, kHeroHealth)(s);
    float max = slot<float(__thiscall*)(void*)>(s, kHeroMaxHealth)(s);
    if (max > 0 && hp < max) slot<void(__thiscall*)(void*, float, bool, bool)>(s, kChangeHeroHealth)(s, max - hp, true, false);
}

void god_tick() {
    if (!fo::config().god_mode) return;
    if (void* s = iface()) heal_full_of(s);
}

}  // namespace

namespace fo::devkit {

void install() {
    // Everything runs on the game thread, which is where engine calls are safe.
    if (!fo::game::hook::install()) return;
    fo::game::hook::on_key([](int vk) {
        if (!fo::config().dev_hotkeys) return;
        if (vk == VK_F9) give_test_kit();
        else if (vk == VK_F10 && iface()) log_health_of(iface());
    });
    fo::game::hook::on_tick(god_tick, 100);
    fo::log("devkit: on (Mod settings > Developer)%s", fo::config().dev_hotkeys ? "; F9 = test gear, F10 = log health" : "");
}

// Game thread only (Mod settings queues these through game::hook::on_tick).
void test_kit() { give_test_kit(); }

void give_gold(int amount) {
    void* s = iface();
    if (!s) { fo::log("devkit: script interface not ready (still on the menu?)"); return; }
    fo::log("devkit: gold +%d", amount);
    slot<void(__thiscall*)(void*, int)>(s, kGiveGold)(s, amount);
}

void heal_full() {
    void* s = iface();
    if (!s) { fo::log("devkit: script interface not ready (still on the menu?)"); return; }
    heal_full_of(s);
    log_health_of(s);
}

void log_health() {
    void* s = iface();
    if (!s) { fo::log("devkit: script interface not ready (still on the menu?)"); return; }
    log_health_of(s);
}

}  // namespace fo::devkit
