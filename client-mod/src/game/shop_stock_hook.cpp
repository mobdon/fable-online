#include "shop_stock_hook.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <cwctype>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../log.h"
#include "defs_edit.h"
#include "menu_row.h"
#include "shop_stock.h"

namespace fo::game::shop_stock {
namespace {

// Bump when the filter's rules change, so an old cached copy is rebuilt.
constexpr int kRulesVersion = 2;   // 2: adds the Mod Settings row to the in-game menu

decltype(&CreateFileA) next_CreateFileA = nullptr;  // whatever the import slot held (kernel32, or another hook)
decltype(&CreateFileW) next_CreateFileW = nullptr;
decltype(&CreateFileW) raw_CreateFileW = nullptr;   // kernel32 itself, for our own reads and writes

HMODULE g_self = nullptr;
std::mutex g_mu;
bool g_tried = false;
std::wstring g_filtered;  // path of the filtered copy; empty = serve the original
bool g_load_patched = false;
std::set<int> g_banned;   // game.bin entry indexes of banned item defs
std::mutex g_banned_mu;
std::atomic<bool> g_menu_row{false};  // the served game.bin lists the Mod Settings row

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

    if (config().shop_filter) {
        std::vector<uint8_t> scratch;
        Report rb = filter(game, names, scratch);
        std::lock_guard<std::mutex> lock(g_banned_mu);
        g_banned.clear();
        for (uint32_t i : rb.banned) g_banned.insert(static_cast<int>(i));
        fo::log("shops: %zu banned item def indexes known", g_banned.size());
    }

    std::vector<uint8_t> old_stamp, cached;
    if (read_all(stamp_path, old_stamp) && std::string(old_stamp.begin(), old_stamp.end()) == stamp &&
        GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        fo::log("shops: using the filtered game.bin from cache");
        // The cached copy carries the menu row when this rules version adds it.
        defs::Bin bin;
        std::vector<uint8_t> c;
        g_menu_row = read_all(dst, c) && bin.load(c, names) && menu_row::patch(bin).error == menu_row::kAlreadyListed;
        return dst;
    }

    DWORD t0 = GetTickCount();
    std::vector<uint8_t> out;
    bool changed = false;
    if (config().shop_filter) {
        Report r = filter(game, names, out);
        if (!r.ok) {
            fo::log("shops: game.bin not filtered (%s); Fable's own shop stock is used", r.error.c_str());
            out.clear();
        } else {
            fo::log("shops: %u shop lists, %u banned item defs, %u stock rows emptied, %u chunks rewritten (%lu ms)",
                    r.shops, r.banned_defs, r.rows_cleared, r.chunks_rewritten, GetTickCount() - t0);
            for (const auto& n : r.cleared) fo::log("shops:   no longer sold: %s", n.c_str());
            changed = r.rows_cleared > 0;
        }
    }
    if (out.empty()) out = game;
    {
        defs::Bin bin;
        std::vector<uint8_t> with_row;
        if (!bin.load(out, names)) {
            fo::log("menu: game.bin not read (%s); no Mod Settings row in Fable's menu", bin.error.c_str());
        } else {
            menu_row::Report m = menu_row::patch(bin);
            if (!m.ok) fo::log("menu: Mod Settings row not added (%s)", m.error.c_str());
            else if (!bin.save(with_row)) fo::log("menu: Mod Settings row not added (%s)", bin.error.c_str());
            else {
                out.swap(with_row);
                changed = true;
                g_menu_row = true;
                fo::log("menu: Mod Settings row added to Fable's in-game menu");
            }
        }
    }
    if (!changed) return {};
    if (!write_all(dst, out.data(), out.size()) || !write_all(stamp_path, stamp, std::strlen(stamp))) {
        fo::log("shops: couldn't write %ls (%lu); Fable's own game.bin is used", dst.c_str(), GetLastError());
        g_menu_row = false;
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

// ---- shop stock restored from a save --------------------------------------------------------
//
// A shop's stock is also kept in the save (CTCShop persist at 0x775D..: StockItemDefsKeys,
// StockItemDefsQuantity, StockItemDefsMaxStock). On load, for each saved item, Fable looks the
// item up in the shop's def rows (0x772250, thiscall, 1 arg, returns the row's stock record:
// +0 type, +4 Quantity, +8 PriceMultiplier, +0xC MaxStock) and, if found, puts the saved
// quantity and max stock back on top of it. So a shop saved before the filter keeps selling
// a banned item even though its def row is empty. The check at 0x776024 is redirected here:
// a row the filter emptied (Quantity and MaxStock both 0) counts as "not stocked", which is
// the same branch Fable takes for an item the shop no longer lists.
constexpr uintptr_t kFindStockRow = 0x772250;
using FindStockRow = const uint32_t*(__thiscall*)(void*, int);

// online-19 showed the check above never firing while the shop still sold phials, so every
// call site of 0x772250 that tests the result for null goes through here: 0x776024 (save
// load), 0x774D4D and 0x775A75. A row is "not stocked" when its def is a banned item (game.bin
// entry index, the same index space Fable passes here, if the log confirms it) or when the
// filter emptied it. The first calls at each site are logged so the next log shows the truth.
struct Site { uintptr_t at; const char* name; std::atomic<unsigned> calls{0}; std::atomic<unsigned> dropped{0}; bool patched = false; };
Site g_sites[] = {{0x776024, "save-load"}, {0x774D4D, "site-4d4d"}, {0x775A75, "site-5a75"}};

template <int I>
const uint32_t* __fastcall find_stock_row(void* shop, void*, int def) {
    Site& s = g_sites[I];
    const uint32_t* row = reinterpret_cast<FindStockRow>(kFindStockRow)(shop, def);
    bool banned;
    {
        std::lock_guard<std::mutex> lock(g_banned_mu);
        banned = g_banned.count(def) != 0;
    }
    bool emptied = row && row[1] == 0 && row[3] == 0;
    unsigned n = ++s.calls;
    if (n <= 40) {
        if (row) fo::log("shops: %s lookup def %d%s: %08X %08X %08X %08X %08X %08X %08X", s.name, def, banned ? " (banned)" : "",
                         row[0], row[1], row[2], row[3], row[4], row[5], row[6]);
        else fo::log("shops: %s lookup def %d%s: not in the shop's def", s.name, def, banned ? " (banned)" : "");
    }
    if (row && (banned || emptied)) {
        if (++s.dropped <= 20) fo::log("shops: %s: def %d treated as not stocked", s.name, def);
        return nullptr;
    }
    return row;
}

using Wrapper = const uint32_t*(__fastcall*)(void*, void*, int);
const Wrapper g_wrappers[] = {&find_stock_row<0>, &find_stock_row<1>, &find_stock_row<2>};

bool patch_call(uintptr_t site, Wrapper to_fn) {
    auto at = reinterpret_cast<uint8_t*>(site);
    int32_t rel;
    std::memcpy(&rel, at + 1, 4);
    if (at[0] != 0xE8 || reinterpret_cast<uintptr_t>(at + 5) + rel != kFindStockRow) return false;
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to_fn) - reinterpret_cast<uintptr_t>(at + 5));
    DWORD old;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at + 1, &to, 4);
    VirtualProtect(at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

bool patch_load_check() {
    bool all = true;
    for (int i = 0; i < 3; ++i) all &= (g_sites[i].patched = patch_call(g_sites[i].at, g_wrappers[i]));
    return all;
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
    g_self = self;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    raw_CreateFileW = reinterpret_cast<decltype(raw_CreateFileW)>(GetProcAddress(k32, "CreateFileW"));
    next_CreateFileA = reinterpret_cast<decltype(next_CreateFileA)>(GetProcAddress(k32, "CreateFileA"));
    next_CreateFileW = raw_CreateFileW;
    if (void* p = hook_import("CreateFileA", reinterpret_cast<void*>(hook_CreateFileA))) next_CreateFileA = reinterpret_cast<decltype(next_CreateFileA)>(p);
    if (void* p = hook_import("CreateFileW", reinterpret_cast<void*>(hook_CreateFileW))) next_CreateFileW = reinterpret_cast<decltype(next_CreateFileW)>(p);
    if (config().shop_filter) g_load_patched = patch_load_check();
}

bool menu_row_served() { return g_menu_row.load() && g_tried && !g_filtered.empty(); }

void log_status() {
    fo::log("shops: filter %s; stock lookups %s (save-load %s, 4d4d %s, 5a75 %s); calls so far %u/%u/%u",
            g_tried ? (g_filtered.empty() ? "NOT applied (see above)" : "applied") : "not reached yet (game.bin not opened through CreateFile)",
            g_load_patched ? "patched" : "NOT all patched", g_sites[0].patched ? "yes" : "no", g_sites[1].patched ? "yes" : "no",
            g_sites[2].patched ? "yes" : "no", g_sites[0].calls.load(), g_sites[1].calls.load(), g_sites[2].calls.load());
}

}  // namespace fo::game::shop_stock
