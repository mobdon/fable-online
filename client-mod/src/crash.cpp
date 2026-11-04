#include "crash.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace fo::crash {
namespace {
constexpr unsigned kMaxReports = 12;
std::atomic<unsigned> g_reports{0};
std::atomic<uintptr_t> g_seen[kMaxReports];

bool fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return true;
    default:
        return false;
    }
}

/** "Fable.exe+0x123" style name for a code address, or false when it is not in a loaded module. */
bool where(uintptr_t addr, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) ||
        !mod)
        return false;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, sizeof(path));
    const char* base = std::strrchr(path, '\\');
    base = base ? base + 1 : path;
    std::snprintf(out, n, "%s+0x%X", base, static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(mod)));
    return true;
}

bool readable(uintptr_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool executable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (!fatal(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t eip = static_cast<uintptr_t>(c->Eip);
    // One report per faulting address, so a fault the game handles over and over can't
    // use up the reports before a real crash.
    for (unsigned i = 0; i < kMaxReports && i < g_reports.load(); ++i)
        if (g_seen[i].load() == eip) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned n = g_reports.fetch_add(1);
    if (n >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
    g_seen[n] = eip;
    char at[128] = "?";
    where(eip, at, sizeof(at));
    // Logged at first chance: the game may still handle it, so this is a lead, not proof.
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fo::log("crash: access violation (%s 0x%08X) at %08X %s, thread %lu",
                er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                static_cast<unsigned>(er->ExceptionInformation[1]), static_cast<unsigned>(eip), at, GetCurrentThreadId());
    else
        fo::log("crash: exception %08lX at %08X %s, thread %lu", er->ExceptionCode, static_cast<unsigned>(eip), at,
                GetCurrentThreadId());
    fo::log("crash:   eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx,
            c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Return addresses left on the stack: the likely chain of callers.
    unsigned found = 0;
    for (uintptr_t sp = c->Esp; found < 16 && sp < c->Esp + 0x800; sp += 4) {
        if (!readable(sp, 4)) break;
        const uintptr_t v = *reinterpret_cast<const uint32_t*>(sp);
        if (v < 0x10000 || !executable(v)) continue;
        char m[128];
        if (!where(v, m, sizeof(m))) continue;
        fo::log("crash:   [esp+%03X] %08X %s", static_cast<unsigned>(sp - c->Esp), static_cast<unsigned>(v), m);
        ++found;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void install() {
    if (AddVectoredExceptionHandler(1, on_exception)) fo::log("crash: fault reporting on");
}
}  // namespace fo::crash
