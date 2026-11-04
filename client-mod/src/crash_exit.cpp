#include "crash_exit.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "log.h"

namespace fo::crash_exit {
namespace {

/** No game tick for this long after a fault: the game is not coming back. */
constexpr uint32_t kDeadAfterMs = 8000;
/** A fault older than this is one the game handled; it no longer counts. */
constexpr uint32_t kFaultWindowMs = 20000;

std::atomic<uint32_t> g_lastTick{0};
std::atomic<uint32_t> g_faultAt{0};
std::atomic<bool> g_started{false};

bool fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
        return true;
    default:
        return false;
    }
}

[[noreturn]] void end(const char* why) {
    fo::log("crash: %s, closing Fable", why);
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    for (;;) Sleep(1000);
}

// First chance: the game may still handle it, so only note the time.
LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    if (fatal(ep->ExceptionRecord->ExceptionCode)) {
        uint32_t now = GetTickCount();
        g_faultAt.store(now ? now : 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Nothing handled it: there is nothing to save.
LONG WINAPI on_unhandled(EXCEPTION_POINTERS*) { end("unhandled fault"); }

void watch() {
    for (;;) {
        Sleep(500);
        uint32_t fault = g_faultAt.load();
        uint32_t tick = g_lastTick.load();
        if (!fault || !tick) continue;
        uint32_t now = GetTickCount();
        if (now - fault > kFaultWindowMs) {
            g_faultAt.store(0);
            continue;
        }
        // The game thread stopped right after a fault and has not ticked since.
        if (static_cast<int32_t>(fault - tick) >= -1000 && now - tick > kDeadAfterMs) end("game stopped after a fault");
    }
}

}  // namespace

void install() {
    if (g_started.exchange(true)) return;
    // No Windows error dialog: a crashed full-screen game behind one is what froze the PC.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    AddVectoredExceptionHandler(0, on_fault);
    SetUnhandledExceptionFilter(on_unhandled);
    std::thread(watch).detach();
    fo::log("crash: Fable closes itself after a crash");
}

void game_alive(uint32_t nowMs) { g_lastTick.store(nowMs ? nowMs : 1); }

}  // namespace fo::crash_exit
