// Fable.exe still has Lionhead's full scoreboard and clan client (login, clan list, clan
// logos, member list, score upload and download). It connects to the host held in the
// CCharString at [0x013B8AF0], set at 0x00562F8B from the literal "fableclangw.lionhead.com",
// on the port saved in the player profile (+0x120, read at 0x0055F830).
//
// We point both at our server:
//  - 0x00562F8B "push imm32" gets our own host string, so the game sets our host itself.
//  - 0x0055F830 "mov eax, [eax+0x120]" becomes "mov eax, <port>; nop".
// Settings: [scoreboard] host= and port= in fable_online.ini next to the DLL, or the
// FABLE_ONLINE_SCOREBOARD=host:port environment variable. Default 127.0.0.1:7777.
// Protocol notes: tools/re-notes/07-scoreboard-protocol.md.
#include "scoreboard.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "log.h"

namespace fo {
namespace {
constexpr uintptr_t kHostPush = 0x00562F8B;   // 68 14 DB 24 01  push "fableclangw.lionhead.com"
constexpr uintptr_t kPortLoad = 0x0055F830;   // 8B 80 20 01 00 00  mov eax, [eax+0x120]
const unsigned char kHostOrig[5] = {0x68, 0x14, 0xDB, 0x24, 0x01};
const unsigned char kPortOrig[6] = {0x8B, 0x80, 0x20, 0x01, 0x00, 0x00};

char g_host[128] = "127.0.0.1";  // must live as long as the process: the game reads it later
unsigned g_port = 7777;
const char* g_status = "not run";

void read_settings() {
    char env[160] = {};
    if (GetEnvironmentVariableA("FABLE_ONLINE_SCOREBOARD", env, sizeof(env)) > 0) {
        char* colon = strrchr(env, ':');
        if (colon) {
            *colon = 0;
            unsigned p = static_cast<unsigned>(strtoul(colon + 1, nullptr, 10));
            if (p > 0 && p < 65536) g_port = p;
        }
        if (env[0]) lstrcpynA(g_host, env, sizeof(g_host));
        return;
    }
    // fable_online.ini next to this DLL.
    char ini[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&read_settings), &self);
    DWORD n = GetModuleFileNameA(self, ini, MAX_PATH);
    char* slash = n ? strrchr(ini, '\\') : nullptr;
    if (!slash) return;
    lstrcpynA(slash + 1, "fable_online.ini", static_cast<int>(MAX_PATH - (slash + 1 - ini)));
    GetPrivateProfileStringA("scoreboard", "host", g_host, g_host, sizeof(g_host), ini);
    unsigned p = GetPrivateProfileIntA("scoreboard", "port", g_port, ini);
    if (p > 0 && p < 65536) g_port = p;
}

bool write_code(uintptr_t at, const unsigned char* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    return true;
}
}  // namespace

bool scoreboard_patch() {
    read_settings();
    if (memcmp(reinterpret_cast<void*>(kHostPush), kHostOrig, sizeof(kHostOrig)) != 0 ||
        memcmp(reinterpret_cast<void*>(kPortLoad), kPortOrig, sizeof(kPortOrig)) != 0) {
        g_status = "skipped: Fable.exe bytes differ (not the Steam build?)";
        return false;
    }
    unsigned char push[5] = {0x68};
    uintptr_t host = reinterpret_cast<uintptr_t>(g_host);
    memcpy(push + 1, &host, 4);
    unsigned char mov[6] = {0xB8, 0, 0, 0, 0, 0x90};
    memcpy(mov + 1, &g_port, 4);
    if (!write_code(kHostPush, push, sizeof(push)) || !write_code(kPortLoad, mov, sizeof(mov))) {
        g_status = "failed: VirtualProtect";
        return false;
    }
    g_status = "patched";
    return true;
}

void scoreboard_report() {
    log("scoreboard: %s (server %s:%u)", g_status, g_host, g_port);
}
}  // namespace fo
