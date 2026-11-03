// Fable Online launcher, phase 1.
//
// 1. Finds Fable: The Lost Chapters (argument, or Steam's default library).
// 2. Starts Fable.exe suspended, with SteamAppId set so it doesn't relaunch via Steam.
// 3. Injects fable_online.dll (from the launcher's own folder) with LoadLibraryW.
// 4. Resumes the game.
//
// Usage: fable_online_launcher.exe ["C:\path\to\Fable The Lost Chapters"] [--server] [--probe]
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
const wchar_t* kSteamAppId = L"204030";

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring dir_of(const std::wstring& p) {
    auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring self_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return dir_of(buf);
}

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

std::wstring find_game_dir(const std::wstring& given) {
    std::vector<std::wstring> candidates;
    if (!given.empty()) candidates.push_back(given);
    candidates.push_back(steam_root() + L"\\steamapps\\common\\Fable The Lost Chapters");
    candidates.push_back(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fable The Lost Chapters");
    for (auto& d : candidates)
        if (file_exists(d + L"\\Fable.exe")) return d;
    return L"";
}

// Fable only allows one copy: a second one quietly exits. Catch that up front.
DWORD running_fable() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof(e)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"Fable.exe") == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

void fail(const wchar_t* what) {
    DWORD e = GetLastError();
    std::fwprintf(stderr, L"error: %ls (Windows error %lu)\n", what, e);
}

bool inject(HANDLE process, const std::wstring& dll) {
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fail(L"VirtualAllocEx"); return false; }
    if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
        fail(L"WriteProcessMemory");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    // kernel32 sits at the same address in every process of the same bitness,
    // and the launcher is built 32-bit like Fable.exe.
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
    if (!t) { fail(L"CreateRemoteThread"); VirtualFreeEx(process, remote, 0, MEM_RELEASE); return false; }
    WaitForSingleObject(t, 15000);
    DWORD module = 0;
    GetExitCodeThread(t, &module);  // low 32 bits of the HMODULE; 0 = LoadLibrary failed
    CloseHandle(t);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (module == 0) { std::fwprintf(stderr, L"error: LoadLibraryW failed inside Fable.exe\n"); return false; }
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring given;
    bool server = false;
    bool probe = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--server") server = true;
        else if (a == L"--probe") probe = true;
        else given = a;
    }

    if (DWORD pid = running_fable()) {
        std::fwprintf(stderr, L"error: Fable is already running (pid %lu). Close it first (Task Manager > Fable.exe > End task).\n", pid);
        return 1;
    }
    std::wstring game = find_game_dir(given);
    if (game.empty()) {
        std::fwprintf(stderr, L"error: Fable.exe not found. Pass the game folder as the first argument.\n");
        return 1;
    }
    std::wstring exe = game + L"\\Fable.exe";
    std::wstring dll = self_dir() + L"\\fable_online.dll";
    if (!file_exists(dll)) {
        std::fwprintf(stderr, L"error: %ls is missing. Put it next to the launcher.\n", dll.c_str());
        return 1;
    }
    std::wprintf(L"game: %ls\nmod:  %ls\nmode: %ls\nprobe: %ls\n", exe.c_str(), dll.c_str(), server ? L"server" : L"client", probe ? L"on" : L"off");

    // Inherited by Fable.exe: stops a Steam relaunch and tells the mod its mode.
    SetEnvironmentVariableW(L"SteamAppId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamGameId", kSteamAppId);
    SetEnvironmentVariableW(L"FABLE_ONLINE_MODE", server ? L"server" : L"client");
    SetEnvironmentVariableW(L"FABLE_ONLINE_PROBE", probe ? L"1" : L"0");

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, game.c_str(), &si, &pi)) {
        fail(L"CreateProcessW");
        return 1;
    }
    if (!inject(pi.hProcess, dll)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    ResumeThread(pi.hThread);
    std::wprintf(L"Fable started with Fable Online loaded (pid %lu).\n", pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
