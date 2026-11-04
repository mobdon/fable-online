#include "shop_stock_hook.h"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../log.h"
#include "shop_stock.h"

namespace fo::game::shop_stock {
namespace {

// Bump when the filter's rules change, so an old cached copy is rebuilt.
constexpr int kRulesVersion = 1;

decltype(&CreateFileA) next_CreateFileA = nullptr;  // whatever the import slot held (kernel32, or another hook)
decltype(&CreateFileW) next_CreateFileW = nullptr;
decltype(&CreateFileW) raw_CreateFileW = nullptr;   // kernel32 itself, for our own reads and writes

HMODULE g_self = nullptr;
std::mutex g_mu;
bool g_tried = false;
std::wstring g_filtered;  // path of the filtered copy; empty = serve the original

bool is_game_bin(std::wstring p) {
    for (auto& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    const std::wstring tail = L"compileddefs\\game.bin";
    return p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0 &&
           (p.size() == tail.size() || p[p.size() - tail.size() - 1] == L'\\');
}

bool read_all(const std::wstring& path, std::vector<uint8_t>& out, FILETIME* mtime = nullptr) {
    HANDLE f = raw_CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (512ll << 20);
    if (ok && mtime) ok = GetFileTime(f, nullptr, nullptr, mtime) != 0;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

bool write_all(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = raw_CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, static_cast<DWORD>(n), &put, nullptr) && put == n;
    CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Builds (or reuses) the filtered copy of the game.bin at `original`. Game thread, once.
std::wstring build(const std::wstring& original) {
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(original.c_str(), MAX_PATH * 2, full, nullptr)) return {};
    std::wstring src(full);
    std::wstring defs = src.substr(0, src.find_last_of(L'\\'));
    std::wstring dir = module_dir(g_self) + L"\\cache";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring dst = dir + L"\\game.bin", stamp_path = dir + L"\\game.bin.stamp";

    std::vector<uint8_t> game, names;
    FILETIME mt{};
    if (!read_all(src, game, &mt) || !read_all(defs + L"\\names.bin", names)) {
        fo::log("shops: couldn't read game.bin / names.bin; Fable's own shop stock is used");
        return {};
    }
    char stamp[96];
    std::snprintf(stamp, sizeof(stamp), "v%d %zu %08lX%08lX\n", kRulesVersion, game.size(), mt.dwHighDateTime, mt.dwLowDateTime);

    std::vector<uint8_t> old_stamp, cached;
    if (read_all(stamp_path, old_stamp) && std::string(old_stamp.begin(), old_stamp.end()) == stamp &&
        GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        fo::log("shops: using the filtered game.bin from cache");
        return dst;
    }

    DWORD t0 = GetTickCount();
    std::vector<uint8_t> out;
    Report r = filter(game, names, out);
    if (!r.ok) {
        fo::log("shops: game.bin not filtered (%s); Fable's own shop stock is used", r.error.c_str());
        return {};
    }
    fo::log("shops: %u shop lists, %u banned item defs, %u stock rows emptied, %u chunks rewritten (%lu ms)",
            r.shops, r.banned_defs, r.rows_cleared, r.chunks_rewritten, GetTickCount() - t0);
    for (const auto& n : r.cleared) fo::log("shops:   no longer sold: %s", n.c_str());
    if (!r.rows_cleared) return {};
    if (!write_all(dst, out.data(), out.size()) || !write_all(stamp_path, stamp, std::strlen(stamp))) {
        fo::log("shops: couldn't write %ls (%lu); Fable's own shop stock is used", dst.c_str(), GetLastError());
        return {};
    }
    return dst;
}

std::wstring redirect(const std::wstring& path, DWORD access, DWORD disp) {
    if ((access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) || disp != OPEN_EXISTING || !is_game_bin(path)) return {};
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_tried) {
        g_tried = true;
        g_filtered = build(path);
    }
    if (!g_filtered.empty()) fo::log("shops: game opened game.bin; serving the filtered copy");
    return g_filtered;
}

HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        wchar_t w[MAX_PATH * 2] = {};
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH * 2)) {
            std::wstring to = redirect(w, access, disp);
            if (!to.empty()) return next_CreateFileW(to.c_str(), access, share, sa, disp, flags, tmpl);
        }
    }
    return next_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name) {
        std::wstring to = redirect(name, access, disp);
        if (!to.empty()) return next_CreateFileW(to.c_str(), access, share, sa, disp, flags, tmpl);
    }
    return next_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

// Swaps one kernel32 import slot of Fable.exe and returns what it held, so hooks chain.
void* hook_import(const char* fn, void* hook) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), "kernel32.dll") != 0 || !imp->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(by_name->Name), fn) != 0) continue;
            void* prev = reinterpret_cast<void*>(slots->u1.Function);
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

}  // namespace

void install(HMODULE self) {
    if (!config().shop_filter) return;
    g_self = self;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    raw_CreateFileW = reinterpret_cast<decltype(raw_CreateFileW)>(GetProcAddress(k32, "CreateFileW"));
    next_CreateFileA = reinterpret_cast<decltype(next_CreateFileA)>(GetProcAddress(k32, "CreateFileA"));
    next_CreateFileW = raw_CreateFileW;
    if (void* p = hook_import("CreateFileA", reinterpret_cast<void*>(hook_CreateFileA))) next_CreateFileA = reinterpret_cast<decltype(next_CreateFileA)>(p);
    if (void* p = hook_import("CreateFileW", reinterpret_cast<void*>(hook_CreateFileW))) next_CreateFileW = reinterpret_cast<decltype(next_CreateFileW)>(p);
}

}  // namespace fo::game::shop_stock
