#include "quests.h"

#include <windows.h>

#include <cstring>
#include <mutex>

#include "../log.h"

namespace fo::game::quests {
namespace {

// CQuestManager::SetQuestAsCompleted(const CCharString& quest, bool, bool, bool), __thiscall,
// ret 0x10. Called from the script interface (slot +0x480, at 0x00892F9F) and from 0x0061ACB3.
constexpr uintptr_t kSetQuestCompleted = 0x004B1D30;
constexpr uintptr_t kCallSites[] = {0x0061ACB3, 0x00892F9F};
// CQuestManager: the quest card thing whose CTCQuestCard holds this script name, or null.
constexpr uintptr_t kFindQuestCard = 0x004B0C80;

// CTCQuestCard (vtable 0x01264C44, constructor 0x007026A0). +0x2C and +0x30 are copied from
// the card def's GoldReward and RenownReward when the card is made; +0x28 is the script name.
constexpr uint32_t kQuestCardVtable = 0x01264C44;
constexpr uint32_t kCardGold = 0x02C;
constexpr uint32_t kCardRenown = 0x030;

constexpr uint32_t kComponentsBegin = 0x044;  // thing -> {int type, component*} pairs
constexpr uint32_t kCompOwner = 0x004;        // component -> its thing
constexpr uint32_t kThingDefIndex = 0x08C;    // uint16
// Def index -> name: entry = [[defs]+0xA4][index]; [entry+4] is the name's offset in the
// symbol table, whose buffer is at [0x13CA83C]; the text starts 4 bytes after the offset.
constexpr uint32_t kDefManager = 0x013B879C;
constexpr uint32_t kDefEntries = 0x0A4;
constexpr uint32_t kEntryName = 0x004;
constexpr uint32_t kSymbolsBegin = 0x013CA83C;
constexpr uint32_t kSymbolsEnd = 0x013CA840;

using SetQuestCompletedFn = void(__thiscall*)(void*, const void*, int, int, int);
using FindQuestCardFn = void*(__thiscall*)(void*, const void*);

std::mutex g_mu;
std::vector<Completed> g_done;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool u32(uintptr_t addr, uint32_t& v) { return read(addr, &v, 4); }

/** A CCharString: a pointer to {char* text, int length, ...}. */
std::string char_string(uintptr_t str) {
    uint32_t rep = 0, text = 0;
    int32_t len = 0;
    if (!u32(str, rep) || !u32(rep, text) || !read(rep + 4, &len, 4) || len <= 0 || len > 128) return {};
    std::string out(static_cast<size_t>(len), '\0');
    if (!read(text, out.data(), out.size())) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string def_name(uint16_t index) {
    uint32_t mgr = 0, entries = 0, entriesEnd = 0, entry = 0, sym = 0, symBegin = 0, symEnd = 0;
    if (!u32(kDefManager, mgr) || !u32(mgr + kDefEntries, entries) || !entries) return {};
    if (u32(mgr + kDefEntries + 4, entriesEnd) && entriesEnd > entries && (entriesEnd - entries) % 4 == 0 &&
        index >= (entriesEnd - entries) / 4)
        return {};
    if (!u32(entries + index * 4u, entry) || !u32(entry + kEntryName, sym) || sym == 0xFFFFFFFFu) return {};
    if (!u32(kSymbolsBegin, symBegin) || !u32(kSymbolsEnd, symEnd) || symEnd <= symBegin) return {};
    if (sym + 4u >= symEnd - symBegin) return {};
    char buf[72] = {};
    const uint32_t at = symBegin + sym + 4;
    const size_t n = symEnd - at < sizeof(buf) - 1 ? symEnd - at : sizeof(buf) - 1;
    if (!read(at, buf, n)) return {};
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

uintptr_t quest_card_component(uintptr_t thing) {
    uint32_t range[2] = {};
    if (!read(thing + kComponentsBegin, range, sizeof(range))) return 0;
    if (range[1] < range[0] || range[1] - range[0] > 64 * 8 || (range[1] - range[0]) % 8) return 0;
    uint32_t pairs[64 * 2];
    const size_t n = (range[1] - range[0]) / 8;
    if (!n || !read(range[0], pairs, n * 8)) return 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t comp = pairs[i * 2 + 1], vt = 0, owner = 0;
        if (comp && u32(comp, vt) && vt == kQuestCardVtable && u32(comp + kCompOwner, owner) && owner == thing) return comp;
    }
    return 0;
}

// Runs on the game thread, in place of Fable's own call.
void __fastcall on_quest_completed(void* mgr, void*, const void* quest, int a, int b, int c) {
    Completed q;
    q.script = char_string(reinterpret_cast<uintptr_t>(quest));
    // The card is looked up before Fable's code runs, while it is certainly still there.
    void* card = mgr && quest ? reinterpret_cast<FindQuestCardFn>(kFindQuestCard)(mgr, quest) : nullptr;
    if (card) {
        const uintptr_t thing = reinterpret_cast<uintptr_t>(card);
        uint16_t index = 0;
        if (read(thing + kThingDefIndex, &index, 2) && index) q.card = def_name(index);
        if (uintptr_t comp = quest_card_component(thing)) {
            read(comp + kCardGold, &q.gold, 4);
            read(comp + kCardRenown, &q.renown, 4);
        }
    }
    reinterpret_cast<SetQuestCompletedFn>(kSetQuestCompleted)(mgr, quest, a, b, c);

    fo::log("quests: Fable completed %s (card %s, %d gold, %d renown on the card)", q.script.empty() ? "?" : q.script.c_str(),
            q.card.empty() ? "none" : q.card.c_str(), q.gold, q.renown);
    if (q.script.empty() && q.card.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_done.size() < 32) g_done.push_back(std::move(q));
}

bool patch_call(uintptr_t site) {
    auto at = reinterpret_cast<uint8_t*>(site);
    int32_t rel;
    std::memcpy(&rel, at + 1, 4);
    if (at[0] != 0xE8 || reinterpret_cast<uintptr_t>(at + 5) + rel != kSetQuestCompleted) return false;
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&on_quest_completed) - reinterpret_cast<uintptr_t>(at + 5));
    DWORD old;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at + 1, &to, 4);
    VirtualProtect(at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

}  // namespace

bool install() {
    static bool done = false;
    if (done) return true;
    // Check every site first, so a different Fable.exe is left wholly untouched.
    for (uintptr_t s : kCallSites) {
        auto at = reinterpret_cast<const uint8_t*>(s);
        int32_t rel;
        std::memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || reinterpret_cast<uintptr_t>(at + 5) + rel != kSetQuestCompleted) {
            fo::log("quests: Fable.exe differs at 0x%08X; finished quests are not reported", static_cast<unsigned>(s));
            return false;
        }
    }
    for (uintptr_t s : kCallSites) patch_call(s);
    done = true;
    fo::log("quests: watching for finished quests");
    return true;
}

std::vector<Completed> take() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<Completed> out;
    out.swap(g_done);
    return out;
}

}  // namespace fo::game::quests
