#include "game.h"
#include "util.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

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
    const DWORD waited = WaitForSingleObject(t, 15000);
    DWORD module = 0;
    GetExitCodeThread(t, &module);  // low 32 bits of the HMODULE; 0 = LoadLibrary failed
    CloseHandle(t);
    // A load that hasn't finished still holds the loader lock: resuming the game would leave
    // it frozen, so the caller ends it instead. The path string stays allocated, as the
    // loader may still read it.
    if (waited != WAIT_OBJECT_0) return L"the mod took too long to load inside Fable.exe";
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (module == 0) return L"LoadLibraryW failed inside Fable.exe";
    return L"";
}

// How Fable.exe is laid out on disk. The Steam build carries its code in .text like any
// program. The retail discs' build is the same code (same link time, same addresses once
// running) wrapped by a loader: .text has no bytes in the file and the wrapper, in the last
// section, decrypts it after the process starts (about ten seconds). Injecting the mod while
// that is still going patches code the wrapper then overwrites, and the wrapper dies on the
// mod's hooks, so such a build gets the mod only once its own code is running.
struct ExeLayout {
    bool packed = false;
    uintptr_t textStart = 0, textEnd = 0;        // where .text sits in memory
    uintptr_t wrapperStart = 0, wrapperEnd = 0;  // the section holding the entry point: the wrapper
};

ExeLayout read_layout(const std::wstring& exe) {
    ExeLayout l;
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return l;
    std::vector<unsigned char> head(4096);
    DWORD got = 0;
    ReadFile(f, head.data(), static_cast<DWORD>(head.size()), &got, nullptr);
    CloseHandle(f);
    if (got < sizeof(IMAGE_DOS_HEADER)) return l;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > got) return l;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(head.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return l;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const DWORD entry = nt->OptionalHeader.AddressOfEntryPoint;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (reinterpret_cast<const unsigned char*>(sec + i + 1) > head.data() + got) break;
        const uintptr_t start = nt->OptionalHeader.ImageBase + sec[i].VirtualAddress;
        const uintptr_t end = start + sec[i].Misc.VirtualSize;
        if (entry >= sec[i].VirtualAddress && entry < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
            l.wrapperStart = start;
            l.wrapperEnd = end;
        }
        if (std::memcmp(sec[i].Name, ".text\0\0\0", 8) != 0) continue;
        l.textStart = start;
        l.textEnd = end;
        l.packed = sec[i].SizeOfRawData == 0;
    }
    if (!l.packed) l.wrapperStart = l.wrapperEnd = 0;
    return l;
}

// True once the process owns a window. The wrapper makes none; the game makes its main
// window early in its own start-up, so a window means the game's code is in charge.
BOOL CALLBACK find_window_of(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != reinterpret_cast<DWORD*>(out)[0]) return TRUE;
    reinterpret_cast<DWORD*>(out)[1] = 1;
    return FALSE;
}

bool has_window(DWORD pid) {
    DWORD data[2] = {pid, 0};
    EnumWindows(find_window_of, reinterpret_cast<LPARAM>(data));
    return data[1] != 0;
}

// Waits until the wrapper has unpacked the game and handed over, then returns at once: the
// game makes its Direct3D device within a fraction of a second of its window, and the mod
// has to be in before that. Two signs, either will do:
//  - .text has been filled in: the wrapper decrypts it whole, then spends a while resolving
//    imports before it jumps into the game. The mod goes in during that while, so its hooks
//    are up before the game makes its window and its Direct3D device, which it does within
//    milliseconds of starting. (The exe has no relocations, so nothing the wrapper still
//    does touches the code the mod patches.) Three sightings a few milliseconds apart;
//  - failing that, the main thread is running code in .text: the wrapper has handed over;
//  - the game owns a window and the main thread is out of the wrapper's section, as it is
//    while the player reads one of the game's own dialogs (the hardware warning): the thread
//    then sits in a system DLL's message loop, not in .text.
// The player may sit at a dialog for a while, so the wait is long, and the game is left alone
// if it runs out.
std::wstring wait_for_unpack(HANDLE process, HANDLE thread, DWORD pid, const ExeLayout& l, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    unsigned inText = 0, outOfWrapper = 0, filled = 0;
    // Three spots across .text; all zero until the wrapper has written the code.
    const uintptr_t textSize = l.textEnd - l.textStart;
    const uintptr_t probes[3] = {l.textStart + 0x1000, l.textStart + textSize / 2, l.textEnd - 0x1000};
    while (GetTickCount64() - start < timeoutMs) {
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)
            return L"Fable.exe exited while unpacking itself (does it start on its own?)";
        unsigned nonZero = 0;
        for (uintptr_t at : probes) {
            unsigned char bytes[16] = {};
            SIZE_T got = 0;
            if (!ReadProcessMemory(process, reinterpret_cast<const void*>(at), bytes, sizeof bytes, &got) || got != sizeof bytes) break;
            bool any = false;
            for (unsigned char b : bytes) any = any || b != 0;
            if (any) ++nonZero;
        }
        if (nonZero == 3) {
            if (++filled >= 3) return L"";
        } else {
            filled = 0;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool got = GetThreadContext(thread, &ctx) != 0;
            ResumeThread(thread);
            if (got) {
                if (ctx.Eip >= l.textStart && ctx.Eip < l.textEnd) {
                    if (++inText >= 3) return L"";
                }
                const bool inWrapper = ctx.Eip >= l.wrapperStart && ctx.Eip < l.wrapperEnd;
                if (!inWrapper && has_window(pid) && ++outOfWrapper >= 3) return L"";
            }
        }
        Sleep(5);
    }
    return L"Fable.exe is running but never handed over from its wrapper; the mod is not loaded";
}

// ---------------------------------------------------------------- online boot

bool g_online_boot = true;
std::wstring g_holy_site = L"GuildHSP";

// Where Windows keeps a legacy program's writes under Program Files: Fable.exe (no manifest)
// reads this copy first, so a file put here is the one the game sees.
std::wstring virtual_store_path(const std::wstring& path) {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    if (path.size() < 3 || path[1] != L':') return L"";
    return std::wstring(local) + L"\\VirtualStore" + path.substr(2);
}

bool read_text(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool write_text(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = text.empty() || (WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size());
    CloseHandle(f);
    return ok;
}

// Rewrites the lines of a start-up script that begin with each `prefix` to `line`, keeping
// everything else. A prefix with `append` set is added at the end if no line had it.
struct Setting { const char* prefix; std::string line; bool append; };

std::string with_settings(const std::string& text, const std::vector<Setting>& settings) {
    std::string out;
    std::vector<bool> seen(settings.size(), false);
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        for (size_t i = 0; i < settings.size(); ++i) {
            if (start == std::string::npos || line.compare(start, std::strlen(settings[i].prefix), settings[i].prefix) != 0) continue;
            line = settings[i].line;
            seen[i] = true;
            break;
        }
        out += line;
        if (nl == std::string::npos) break;
        out += cr ? "\r\n" : "\n";
        at = nl + 1;
    }
    const bool crlf = text.find("\r\n") != std::string::npos;
    for (size_t i = 0; i < settings.size(); ++i) {
        if (seen[i] || !settings[i].append) continue;
        if (!out.empty() && out.back() != '\n') out += crlf ? "\r\n" : "\n";
        out += settings[i].line + (crlf ? "\r\n" : "\n");
    }
    return out;
}

// Applies the settings to one of Fable's start-up files. The copy the game reads wins: the
// VirtualStore one if there is one, else the real file; a real file that can't be written
// (Program Files without admin rights) gets its edited copy in the VirtualStore instead,
// which is exactly where the game looks first. The original is kept once as *.fable-original.
std::wstring apply_settings(const std::wstring& game, const wchar_t* name, const std::vector<Setting>& settings) {
    const std::wstring real = game + L"\\" + name;
    const std::wstring shadow = virtual_store_path(real);
    std::wstring source = (!shadow.empty() && file_exists(shadow)) ? shadow : real;
    std::string text;
    if (!read_text(source, text)) return std::wstring(name) + L": not found";
    const std::string edited = with_settings(text, settings);
    if (edited == text) return L"";
    std::wstring target = real;
    if (!write_text(target, edited)) {
        if (shadow.empty()) return std::wstring(name) + L": can't write";
        make_dirs(dir_of(shadow));
        target = shadow;
        if (!write_text(target, edited)) return std::wstring(name) + L": can't write";
    }
    const std::wstring backup = target + L".fable-original";
    if (!file_exists(backup)) write_text(backup, text);
    return std::wstring(name) + (target == shadow ? L" (VirtualStore copy)" : L"");
}

std::wstring apply_online_boot(const std::wstring& game) {
    const std::string site = narrow(g_holy_site);
    std::vector<Setting> start = {
        // Only commands the retail engine knows: its string table has SetStartingHolySite but
        // neither SetSkipFrontend nor SetPlayIntro, which are dead leftovers in the shipped file.
        {"SetStartingHolySite(", "SetStartingHolySite(\"" + site + "\");", true},
    };
    std::vector<Setting> story = {
        {"ActivateQuest(\"Gameflow\")", "// ActivateQuest(\"Gameflow\");  // Chapters of the Lost Online: the story's gameflow is off", false},
    };
    std::wstring note = apply_settings(game, L"userst.ini", start);
    std::wstring second = apply_settings(game, L"user.ini", story);
    if (!second.empty()) note += (note.empty() ? L"" : L", ") + second;
    return note;
}
}  // namespace

void set_online_boot(bool on, const std::wstring& holySite) {
    g_online_boot = on;
    if (!holySite.empty()) g_holy_site = holySite;
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
    const ExeLayout layout = read_layout(exe);
    if (g_online_boot) r.boot = apply_online_boot(game);

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
    std::wstring err;
    if (layout.packed) {
        // Let the wrapper unpack the game first; the mod goes in once game code is running.
        ResumeThread(pi.hThread);
        err = wait_for_unpack(pi.hProcess, pi.hThread, pi.dwProcessId, layout, 10 * 60 * 1000);
        if (err.empty()) err = inject(pi.hProcess, dll);
        r.unpacked_first = true;
    } else {
        err = inject(pi.hProcess, dll);
    }
    if (!err.empty()) {
        // A game that is up and showing a window is the player's to close; only a start that
        // never got that far is ended here.
        if (!has_window(pi.dwProcessId)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        r.error = err;
    } else {
        if (!layout.packed) ResumeThread(pi.hThread);
        r.ok = true;
        r.pid = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}
