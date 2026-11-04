#include "spawns.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>

#include "../log.h"
#include "../world/spawn_filter.h"
#include "thing.h"

// The level loader's header reader passes its arguments in registers (ecx: parser, edx: the
// header it fills), so the mod's stand-in is a few lines of assembly: call the real reader,
// then hand the filled header to fo_level_header_read.
extern "C" void fo_level_header_read(uintptr_t header);
extern "C" void fo_level_header_thunk();
#if defined(_MSC_VER)
// MSVC has no file-scope asm: the same thunk as a naked function in Intel syntax.
extern "C" __declspec(naked) void fo_level_header_thunk() {
    __asm {
        push edx
        mov eax, 0x004C81F0
        call eax
        pop edx
        push eax
        push edx
        call fo_level_header_read
        add esp, 4
        pop eax
        ret
    }
}
#else
asm(R"(
    .text
    .globl _fo_level_header_thunk
_fo_level_header_thunk:
    push %edx
    mov $0x004C81F0, %eax
    call *%eax
    pop %edx
    push %eax
    push %edx
    call _fo_level_header_read
    add $4, %esp
    pop %eax
    ret
)");
#endif

namespace fo::game::spawns {
namespace {

// Vtables of the two generator components, and the slot that runs their spawning work.
// Both take no arguments and return nothing, so a bare `ret` stands in for either.
constexpr uintptr_t kGeneratorVtable = 0x01268C24;         // CTCCreatureGenerator
constexpr uintptr_t kGeneratorCreatorVtable = 0x0126EB3C;  // CTCCreatureGeneratorCreator
constexpr unsigned kUpdateSlot = 0x24;

struct Patched {
    uintptr_t slot = 0;
    uint32_t original = 0;
};

Patched g_patched[2];
bool g_on = false;
unsigned char* g_stub = nullptr;

// ---- the level loader (all addresses fixed: Fable.exe has no ASLR)

// CThingManager: builds one thing from a level (.tng) or save entry and returns it, or 0
// when it drops the entry. __thiscall (manager, parser, mode), pops 8.
constexpr uintptr_t kLoadThing = 0x0051FD80;
// Every call to it: three level loaders and two save loaders.
constexpr uintptr_t kLoadThingCalls[] = {0x0052062E, 0x00520BA1, 0x00520F9A, 0x00521155, 0x00521521};
// Reads an entry's UID, DefinitionType and Player fields into a header; DefinitionType is a
// CCharString at +8. Called once from kLoadThing (directly or through 0x004C88E0).
constexpr uintptr_t kReadHeader = 0x004C81F0;
constexpr uintptr_t kReadHeaderCalls[] = {0x004C89A8, 0x004C9252, 0x0051FE07};
constexpr uint32_t kHeaderDef = 0x08;
// CThing::Remove(bool), the call the loader itself makes on a thing it cannot keep (and the
// script interface's remove command). __thiscall (thing, bool), pops 4.
constexpr uintptr_t kThingRemove = 0x004C9B80;
constexpr uint32_t kAICreatureVtable = 0x0127293C;  // CThingAICreature, the .tng "AICreature" kind
constexpr uint32_t kThingScriptName = 0x74;         // CCharString, set from the entry's ScriptName

using LoadThingFn = uintptr_t(__fastcall*)(uintptr_t manager, uintptr_t unused, uintptr_t parser, int mode);
using RemoveFn = void(__fastcall*)(uintptr_t thing, uintptr_t unused, int flag);

world::SpawnFilter g_filter;
bool g_levelHook = false;
int g_loadDepth = 0;
bool g_haveDef = false;
char g_def[72] = {};
size_t g_dropped = 0;

bool read_mem(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

/** The text of a CCharString (a pointer to a block whose first field is the char data). */
bool read_char_string(uintptr_t str, char* out, size_t cap) {
    out[0] = '\0';
    uint32_t block = 0, text = 0;
    if (!read_mem(str, &block, 4)) return false;
    if (!block) return true;  // the empty string
    if (!read_mem(block, &text, 4) || !text) return true;
    size_t n = 0;
    for (; n + 1 < cap; ++n) {
        char c = 0;
        if (!read_mem(text + n, &c, 1)) return false;
        if (!c) break;
        out[n] = c;
    }
    out[n] = '\0';
    return true;
}

/** The rel32 of a 5-byte `call` at `site`, if it calls `target`. */
bool calls(uintptr_t site, uintptr_t target) {
    unsigned char b[5];
    if (!read_mem(site, b, 5) || b[0] != 0xE8) return false;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return site + 5 + static_cast<uintptr_t>(rel) == target;
}

bool retarget(uintptr_t site, uintptr_t to) {
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(site + 1), &rel, 4);
    VirtualProtect(reinterpret_cast<LPVOID>(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(site), 5);
    return true;
}

uintptr_t __fastcall load_thing_hook(uintptr_t manager, uintptr_t, uintptr_t parser, int mode) {
    ++g_loadDepth;
    g_haveDef = false;
    g_def[0] = '\0';
    uintptr_t thing = reinterpret_cast<LoadThingFn>(kLoadThing)(manager, 0, parser, mode);
    --g_loadDepth;
    if (!thing || !g_on || !g_filter.active() || !g_haveDef) return thing;

    uint32_t vt = 0;
    if (!read_mem(thing, &vt, 4) || vt != kAICreatureVtable) return thing;
    char script[72];
    if (!read_char_string(thing + kThingScriptName, script, sizeof(script))) return thing;
    if (!g_filter.should_drop(true, g_def, script)) return thing;

    reinterpret_cast<RemoveFn>(kThingRemove)(thing, 0, 1);
    ++g_dropped;
    if (g_dropped <= 20 || g_dropped % 100 == 0)
        fo::log("spawns: left out level monster %s (the server runs it; %u so far)", g_def,
                static_cast<unsigned>(g_dropped));
    return 0;
}

unsigned char* stub() {
    if (g_stub) return g_stub;
    g_stub = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub) g_stub[0] = 0xC3;  // ret
    return g_stub;
}

bool patch(uintptr_t slot, uint32_t value, uint32_t& previous) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(slot), 4, PAGE_READWRITE, &old)) return false;
    previous = *reinterpret_cast<uint32_t*>(slot);
    *reinterpret_cast<uint32_t*>(slot) = value;
    VirtualProtect(reinterpret_cast<LPVOID>(slot), 4, old, &old);
    return true;
}

}  // namespace

bool suppressed() { return g_on; }

bool suppress() {
    if (g_on) return true;
    unsigned char* s = stub();
    if (!s) {
        fo::log("spawns: no memory for the stub; local spawning left on");
        return false;
    }
    const uintptr_t vtables[2] = {kGeneratorVtable, kGeneratorCreatorVtable};
    size_t done = 0;
    for (size_t i = 0; i < 2; ++i) {
        uintptr_t slot = vtables[i] + kUpdateSlot;
        uint32_t previous = 0;
        if (!patch(slot, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)), previous)) continue;
        g_patched[done++] = Patched{slot, previous};
    }
    if (done == 0) {
        fo::log("spawns: could not write the generator slots; local spawning left on");
        return false;
    }
    g_on = true;
    fo::log("spawns: local creature spawning off (%u of 2 slots, %u generators in this region)",
            static_cast<unsigned>(done), static_cast<unsigned>(count_generators()));
    return true;
}

void restore() {
    if (!g_on) return;
    for (Patched& p : g_patched) {
        if (!p.slot) continue;
        uint32_t ignored = 0;
        patch(p.slot, p.original, ignored);
        p = Patched{};
    }
    g_on = false;
    fo::log("spawns: local creature spawning back on");
}

bool install_level_hook() {
    if (g_levelHook) return true;
    // Check every site first: a different Fable.exe gets no patch at all, not half of one.
    for (uintptr_t site : kLoadThingCalls)
        if (!calls(site, kLoadThing)) {
            fo::log("spawns: level loader call at 0x%08X is not what this build expects; level monsters stay local",
                    static_cast<unsigned>(site));
            return false;
        }
    for (uintptr_t site : kReadHeaderCalls)
        if (!calls(site, kReadHeader)) {
            fo::log("spawns: header read at 0x%08X is not what this build expects; level monsters stay local",
                    static_cast<unsigned>(site));
            return false;
        }
    for (uintptr_t site : kReadHeaderCalls) retarget(site, reinterpret_cast<uintptr_t>(&fo_level_header_thunk));
    for (uintptr_t site : kLoadThingCalls) retarget(site, reinterpret_cast<uintptr_t>(&load_thing_hook));
    g_levelHook = true;
    fo::log("spawns: level loader hooked (%u calls)",
            static_cast<unsigned>(std::size(kLoadThingCalls) + std::size(kReadHeaderCalls)));
    return true;
}

void set_server_defs(const std::vector<std::string>& defs) {
    g_filter.set_server_defs(defs);
    if (g_filter.active())
        fo::log("spawns: the server runs %u monster types; the level files' copies of them are left out%s",
                static_cast<unsigned>(g_filter.def_count()), g_levelHook ? "" : " (loader hook not in place, so they stay)");
}

size_t dropped_count() { return g_dropped; }

size_t count_generators() {
    // The generator objects carry the vtable we patched, so counting them is a plain scan.
    return scan_vtable(static_cast<uint32_t>(kGeneratorVtable), 4096).size();
}

}  // namespace fo::game::spawns

extern "C" void fo_level_header_read(uintptr_t header) {
    using namespace fo::game::spawns;
    if (g_loadDepth <= 0 || g_haveDef) return;
    g_haveDef = read_char_string(header + kHeaderDef, g_def, sizeof(g_def)) && g_def[0];
}
