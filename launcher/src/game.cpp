#include "game.h"
#include "util.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>

namespace {
const wchar_t* kSteamAppId = L"204030";

// Steam's install path from the registry, falling back to the default.
std::wstring steam_root() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        std::wstring s(buf);
        for (auto& c : s) if (c == L'/') c = L'\\';
        return s;
    }
    return L"C:\\Program Files (x86)\\Steam";
}

std::wstring last_error(const wchar_t* what) {
    wchar_t buf[160];
    std::swprintf(buf, 160, L"%ls failed (Windows error %lu)", what, GetLastError());
    return buf;
}

std::wstring inject(HANDLE process, const std::wstring& dll) {
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return last_error(L"VirtualAllocEx");
    if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
        std::wstring e = last_error(L"WriteProcessMemory");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return e;
    }
    // kernel32 sits at the same address in every process of the same bitness,
    // and the launcher is built 32-bit like Fable.exe.
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
    if (!t) {
        std::wstring e = last_error(L"CreateRemoteThread");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return e;
    }
    WaitForSingleObject(t, 15000);
    DWORD module = 0;
    GetExitCodeThread(t, &module);  // low 32 bits of the HMODULE; 0 = LoadLibrary failed
    CloseHandle(t);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (module == 0) return L"LoadLibraryW failed inside Fable.exe";
    return L"";
}
}  // namespace

std::wstring find_game_dir(const std::wstring& given) {
    std::vector<std::wstring> candidates;
    if (!given.empty()) candidates.push_back(given);
    candidates.push_back(steam_root() + L"\\steamapps\\common\\Fable The Lost Chapters");
    candidates.push_back(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fable The Lost Chapters");
    for (auto& d : candidates)
        if (file_exists(d + L"\\Fable.exe")) return d;
    return L"";
}

unsigned long running_fable() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof(e)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"Fable.exe") == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

LaunchResult launch_fable(const std::wstring& game, const std::wstring& dll,
                          const std::vector<std::pair<std::wstring, std::wstring>>& env) {
    LaunchResult r;
    if (DWORD pid = running_fable()) {
        wchar_t buf[200];
        std::swprintf(buf, 200, L"Fable is already running (pid %lu). Close it first (Task Manager > Fable.exe > End task).", pid);
        r.error = buf;
        return r;
    }
    if (!file_exists(dll)) { r.error = dll + L" is missing. Put it next to the launcher."; return r; }
    std::wstring exe = game + L"\\Fable.exe";
    if (!file_exists(exe)) { r.error = L"Fable.exe not found in " + game; return r; }

    // Inherited by Fable.exe: stops a Steam relaunch and hands the mod its settings.
    SetEnvironmentVariableW(L"SteamAppId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamGameId", kSteamAppId);
    for (auto& kv : env) SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    BOOL created = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                                  nullptr, game.c_str(), &si, &pi);
    // The ticket is one-time, but don't leave it lying around in our own env either.
    for (auto& kv : env) SetEnvironmentVariableW(kv.first.c_str(), nullptr);
    if (!created) { r.error = last_error(L"CreateProcessW"); return r; }
    std::wstring err = inject(pi.hProcess, dll);
    if (!err.empty()) {
        TerminateProcess(pi.hProcess, 1);
        r.error = err;
    } else {
        ResumeThread(pi.hThread);
        r.ok = true;
        r.pid = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}
