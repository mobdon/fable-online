#include "target_colour.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../config.h"
#include "../log.h"

namespace fo::display::target_colour {
namespace {
// In the highlight update, the chosen kind (a local at [ebp+6Ch]) is compared with the
// cursor's current kind ([esi+44h]); when it differs, the colour and fade are reloaded from
// the table. The six bytes `mov eax,[ebp+6Ch] ; cmp eax,[esi+44h]` become a jump to a block
// that lets the kind be changed before the compare.
constexpr uint32_t kSite = 0x0064C41D, kResume = 0x0064C423;
constexpr uint8_t kOriginal[8] = {0x8B, 0x45, 0x6C, 0x3B, 0x46, 0x44, 0x74, 0x2E};

constexpr int kEnemy = 2;
constexpr uint32_t kGuiDef = 0x013B878C;           // player GUI definition
constexpr uint32_t kStartColours = 0x86C, kEndColours = 0x878;
// Game functions, all __thiscall (called here as __fastcall with an unused edx).
constexpr uint32_t kHandleGet = 0x00A01B50;        // thing handle -> thing, or null
constexpr uint32_t kPlayerOf = 0x00449970;          // player GUI owner -> player
constexpr uint32_t kPlayerMode = 0x00633BE0;        // player -> current mode id
constexpr uint32_t kPlayerHasMode = 0x00634160;     // player, mode id -> mode is on the stack
constexpr int kModeCombat = 2, kModeOverlay = 0x28;
using GetPtr_t = void*(__fastcall*)(void*, void*);
using GetInt_t = int(__fastcall*)(void*, void*);
using HasMode_t = bool(__fastcall*)(void*, void*, int);

char g_status[120] = "red target: Fable's own colours";
std::atomic<int> g_logged{0};
int g_last_kind = -1, g_last_out = -1, g_last_mode = -1;
bool g_table_logged = false;

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uint32_t word(uint32_t addr) { return readable(addr, 4) ? *reinterpret_cast<const uint32_t*>(addr) : 0; }

void log_table() {
    g_table_logged = true;
    const uint32_t def = word(kGuiDef);
    const uint32_t start = def ? word(def + kStartColours) : 0, end = def ? word(def + kEndColours) : 0;
    if (!start || !end || !readable(start, 20) || !readable(end, 20)) {
        fo::log("red target: colour table not readable");
        return;
    }
    char line[200];
    int n = std::snprintf(line, sizeof line, "red target: colours by kind (ARGB start/end):");
    for (int i = 0; i < 5 && n > 0 && n < static_cast<int>(sizeof line); ++i)
        n += std::snprintf(line + n, sizeof line - n, " %d=%08X/%08X", i, word(start + 4 * i), word(end + 4 * i));
    fo::log("%s", line);
}

int __stdcall adjust(int kind, uint8_t* cursor) {
    if (!config().red_target) return kind;   // read live: the mod settings screen can change it
    if (!g_table_logged) log_table();
    int out = kind;
    int mode = -1;
    void* player = reinterpret_cast<GetPtr_t>(kPlayerOf)(*reinterpret_cast<void**>(cursor + 8), nullptr);
    if (player) mode = reinterpret_cast<GetInt_t>(kPlayerMode)(player, nullptr);
    const bool combat = player && (mode == kModeCombat ||
                                   (mode == kModeOverlay && reinterpret_cast<HasMode_t>(kPlayerHasMode)(player, nullptr, kModeCombat)));
    if (combat && kind != kEnemy && kind != 0) {
        // Same creature test the highlight code uses (thing type byte +0x10 == 1).
        auto* thing = static_cast<uint8_t*>(reinterpret_cast<GetPtr_t>(kHandleGet)(cursor + 0x28, nullptr));
        if (thing && thing[0x10] == 1) out = kEnemy;
    }
    if ((kind != g_last_kind || out != g_last_out || mode != g_last_mode) && g_logged < 200) {
        ++g_logged;
        fo::log("red target: kind %d -> %d, hero mode %d%s", kind, out, mode, combat ? " (combat)" : "");
    }
    g_last_kind = kind;
    g_last_out = out;
    g_last_mode = mode;
    return out;
}

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
}  // namespace

void install() {
    if (!readable(kSite, sizeof kOriginal) || std::memcmp(reinterpret_cast<const void*>(kSite), kOriginal, sizeof kOriginal) != 0) {
        std::snprintf(g_status, sizeof g_status, "red target: code not where expected, left as is");
        return;
    }
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) return;
    const uint32_t c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cave));
    const uint32_t fn = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&adjust));
    uint8_t* p = cave;
    const uint8_t head[] = {0x8B, 0x45, 0x6C,   // mov eax, [ebp+6Ch]
                            0x60,               // pushad
                            0x56,               // push esi   (cursor)
                            0x50};              // push eax   (kind)
    std::memcpy(p, head, sizeof head);
    p += sizeof head;
    *p++ = 0xE8;                                // call adjust (stdcall)
    put32(p, fn - (c + static_cast<uint32_t>(p - cave) + 4));
    p += 4;
    const uint8_t tail[] = {0x89, 0x44, 0x24, 0x1C,   // mov [esp+1Ch], eax  (pushad's eax)
                            0x61,                     // popad
                            0x3B, 0x46, 0x44};        // cmp eax, [esi+44h]
    std::memcpy(p, tail, sizeof tail);
    p += sizeof tail;
    *p++ = 0xE9;                                // jmp back to the je
    put32(p, kResume - (c + static_cast<uint32_t>(p - cave) + 4));
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), cave, static_cast<SIZE_T>(p - cave));

    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    put32(jump + 1, c - (kSite + 5));
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(kSite);
    if (!VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, jump, sizeof jump);
    VirtualProtect(at, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
    std::snprintf(g_status, sizeof g_status, config().red_target ? "red target: creatures targeted in combat mode shown as enemies" : "red target: ready, off in settings");
}

const char* status() { return g_status; }
}  // namespace fo::display::target_colour
