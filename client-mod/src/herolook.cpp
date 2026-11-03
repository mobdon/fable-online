// Test-only hero model swap.
//
// The hero is drawn with MESH_HERO from data/graphics/graphics.big (bank MBANK_ALLMESHES). The
// bank's entry table maps each mesh id to an offset, size and sub-header inside the file. When
// FABLE_ONLINE_HERO_MESH names another mesh (e.g. MESH_RIVAL_HERO_WHISPER), this file rewrites
// MESH_HERO's entry in that table as the game reads it, so MESH_HERO's id and name point at the
// other mesh's data. Nothing is copied or shipped: the data comes from the player's own file.
//
// Hooks: CreateFileA/W, ReadFile, ReadFileEx and CloseHandle in Fable.exe's import table. Only
// handles to graphics.big are touched, and only bytes inside the entry table are changed.
//
// It applies when the game starts (the mesh loads with the hero), not as a live toggle.
#include "herolook.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "log.h"

namespace {

constexpr const char* kHeroMesh = "MESH_HERO";
constexpr const char* kMeshBank = "MBANK_ALLMESHES";

struct Table {
    uint64_t offset = 0;        // where the entry table sits in graphics.big
    std::vector<uint8_t> data;  // the table with MESH_HERO's entry rewritten
};
Table g_table;

std::mutex g_mu;
std::set<HANDLE> g_handles;  // open handles to graphics.big
struct Pending { LPOVERLAPPED_COMPLETION_ROUTINE done; uint8_t* buf; uint64_t pos; };
std::map<LPOVERLAPPED, Pending> g_pending;
bool g_logged_patch = false;

decltype(&CreateFileA) real_CreateFileA = nullptr;
decltype(&CreateFileW) real_CreateFileW = nullptr;
decltype(&ReadFile) real_ReadFile = nullptr;
decltype(&ReadFileEx) real_ReadFileEx = nullptr;
decltype(&CloseHandle) real_CloseHandle = nullptr;

// ---- graphics.big entry table -------------------------------------------------------------

struct Reader {
    const std::vector<uint8_t>& d;
    size_t p;
    bool ok = true;
    uint32_t u32() {
        if (p + 4 > d.size()) { ok = false; return 0; }
        uint32_t v; std::memcpy(&v, &d[p], 4); p += 4; return v;
    }
    void skip(size_t n) { if (p + n > d.size()) ok = false; else p += n; }
};

struct Entry {
    size_t begin = 0, end = 0;  // byte range of the whole entry in the table
    uint32_t fields[7] = {};    // magic, id, type, size, offset, crc, name length
    std::string name;
    uint32_t crc2 = 0;
    size_t sources_begin = 0, sources_end = 0;
    size_t sub_begin = 0, sub_end = 0;  // sub-header bytes, without its length word
};

bool read_at(HANDLE f, uint64_t pos, void* out, DWORD n) {
    LONG hi = static_cast<LONG>(pos >> 32);
    if (SetFilePointer(f, static_cast<LONG>(pos & 0xFFFFFFFF), &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR) return false;
    DWORD got = 0;
    return real_ReadFile(f, out, n, &got, nullptr) && got == n;
}

std::wstring graphics_big_path() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"." : p.substr(0, slash)) + L"\\data\\graphics\\graphics.big";
}

bool build_table(const char* wanted) {
    HANDLE f = real_CreateFileW(graphics_big_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { fo::log("herolook: can't open graphics.big (%lu)", GetLastError()); return false; }

    bool ok = false;
    uint32_t hdr[4] = {};
    if (read_at(f, 0, hdr, sizeof(hdr)) && std::memcmp(hdr, "BIGB", 4) == 0) {
        // Bank index: u32 count, then per bank: name\0, id, entry count, table offset, table size, u32.
        std::vector<uint8_t> idx(4096);
        if (read_at(f, hdr[2], idx.data(), 4) ) {
            DWORD idx_size = GetFileSize(f, nullptr) - hdr[2];
            if (idx_size > idx.size()) idx_size = static_cast<DWORD>(idx.size());
            idx.resize(idx_size);
            read_at(f, hdr[2], idx.data(), idx_size);
            Reader r{idx, 0};
            uint32_t banks = r.u32();
            uint32_t count = 0, toff = 0, tsize = 0;
            for (uint32_t b = 0; b < banks && r.ok; ++b) {
                std::string name;
                while (r.p < idx.size() && idx[r.p]) name += static_cast<char>(idx[r.p++]);
                r.skip(1);
                r.u32(); uint32_t c = r.u32(), o = r.u32(), s = r.u32(); r.u32();
                if (name == kMeshBank) { count = c; toff = o; tsize = s; }
            }
            if (count && tsize) {
                g_table.offset = toff;
                g_table.data.resize(tsize);
                ok = read_at(f, toff, g_table.data.data(), tsize);
            }
            if (!ok) fo::log("herolook: %s not found in graphics.big", kMeshBank);
            else {
                // Entry table: u32 n, n * 8 bytes, then the entries.
                const auto& d = g_table.data;
                Reader t{d, 0};
                uint32_t n = t.u32(); t.skip(n * 8);
                Entry hero, src; bool have_hero = false, have_src = false;
                for (uint32_t i = 0; i < count && t.ok; ++i) {
                    Entry e; e.begin = t.p;
                    for (auto& v : e.fields) v = t.u32();
                    if (!t.ok || e.fields[6] > 256) { t.ok = false; break; }
                    e.name.assign(reinterpret_cast<const char*>(&d[t.p]), e.fields[6]); t.skip(e.fields[6]);
                    e.crc2 = t.u32();
                    e.sources_begin = t.p;
                    uint32_t ns = t.u32();
                    for (uint32_t k = 0; k < ns && t.ok; ++k) t.skip(t.u32());
                    e.sources_end = t.p;
                    uint32_t sl = t.u32(); e.sub_begin = t.p; t.skip(sl); e.sub_end = t.p;
                    e.end = t.p;
                    if (e.name == kHeroMesh) { hero = e; have_hero = true; }
                    if (e.name == wanted) { src = e; have_src = true; }
                }
                ok = false;
                if (!t.ok) fo::log("herolook: couldn't read the mesh table");
                else if (!have_hero) fo::log("herolook: %s not in the mesh table", kHeroMesh);
                else if (!have_src) fo::log("herolook: %s not in the mesh table", wanted);
                else {
                    // New MESH_HERO entry: its own magic, id and name; everything else from the source.
                    std::vector<uint8_t> e;
                    auto put = [&](uint32_t v) { const uint8_t* b = reinterpret_cast<uint8_t*>(&v); e.insert(e.end(), b, b + 4); };
                    put(hero.fields[0]); put(hero.fields[1]);
                    for (int k = 2; k < 6; ++k) put(src.fields[k]);
                    put(hero.fields[6]);
                    e.insert(e.end(), hero.name.begin(), hero.name.end());
                    put(src.crc2);
                    e.insert(e.end(), d.begin() + hero.sources_begin, d.begin() + hero.sources_end);
                    put(static_cast<uint32_t>(src.sub_end - src.sub_begin));
                    e.insert(e.end(), d.begin() + src.sub_begin, d.begin() + src.sub_end);

                    size_t old_len = hero.end - hero.begin;
                    if (e.size() > old_len) {
                        fo::log("herolook: %s's entry is %u bytes longer than %s's; pick another mesh",
                                wanted, static_cast<unsigned>(e.size() - old_len), kHeroMesh);
                    } else {
                        std::vector<uint8_t> out(d.begin(), d.begin() + hero.begin);
                        out.insert(out.end(), e.begin(), e.end());
                        out.insert(out.end(), d.begin() + hero.end, d.end());
                        out.resize(d.size(), 0);  // same size as the original table; the tail is padding
                        g_table.data.swap(out);
                        fo::log("herolook: %s (id %u) now uses %s's data: offset 0x%08X, %u bytes",
                                kHeroMesh, hero.fields[1], wanted, src.fields[4], src.fields[3]);
                        ok = true;
                    }
                }
            }
        }
    } else {
        fo::log("herolook: graphics.big has no BIGB header");
    }
    real_CloseHandle(f);
    if (!ok) g_table.data.clear();
    return ok;
}

// Copies our table over whatever part of it a read covers.
void patch(uint8_t* buf, uint64_t pos, DWORD n) {
    uint64_t a = pos, b = pos + n;
    uint64_t ta = g_table.offset, tb = g_table.offset + g_table.data.size();
    if (b <= ta || a >= tb) return;
    uint64_t lo = a > ta ? a : ta, hi = b < tb ? b : tb;
    std::memcpy(buf + (lo - a), g_table.data.data() + (lo - ta), static_cast<size_t>(hi - lo));
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_logged_patch) {
        g_logged_patch = true;
        fo::log("herolook: patched the mesh table as the game read it (%u bytes at 0x%08X)",
                static_cast<unsigned>(hi - lo), static_cast<unsigned>(lo));
    }
}

bool tracked(HANDLE h) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_handles.count(h) != 0;
}

bool is_graphics_big(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = static_cast<wchar_t>(towlower(c));
    const std::wstring tail = L"graphics.big";
    return p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0 &&
           (p.size() == tail.size() || p[p.size() - tail.size() - 1] == L'\\' || p[p.size() - tail.size() - 1] == L'/');
}

void track(HANDLE h, const std::wstring& path) {
    if (h == INVALID_HANDLE_VALUE || !is_graphics_big(path)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_handles.insert(h);
}

// ---- hooks --------------------------------------------------------------------------------

HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl) {
    HANDLE h = real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (name) {
        wchar_t w[MAX_PATH * 2] = {};
        MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH * 2);
        track(h, w);
    }
    return h;
}

HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl) {
    HANDLE h = real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (name) track(h, name);
    return h;
}

BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (!tracked(h)) return real_ReadFile(h, buf, n, got, ov);
    uint64_t pos;
    if (ov) {
        pos = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    } else {
        LONG hi = 0;
        DWORD lo = SetFilePointer(h, 0, &hi, FILE_CURRENT);
        pos = (static_cast<uint64_t>(static_cast<DWORD>(hi)) << 32) | lo;
    }
    BOOL r = real_ReadFile(h, buf, n, got, ov);
    if (r) {
        DWORD done = got ? *got : n;
        patch(static_cast<uint8_t*>(buf), pos, done);
    } else if (GetLastError() == ERROR_IO_PENDING && pos < g_table.offset + g_table.data.size() &&
               pos + n > g_table.offset) {
        fo::log("herolook: the mesh table was read asynchronously through ReadFile; not patched");
    }
    return r;
}

VOID CALLBACK on_read_done(DWORD err, DWORD n, LPOVERLAPPED ov) {
    Pending p{};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_pending.find(ov);
        if (it == g_pending.end()) return;
        p = it->second;
        g_pending.erase(it);
    }
    if (err == 0) patch(p.buf, p.pos, n);
    if (p.done) p.done(err, n, ov);
}

BOOL WINAPI hook_ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE done) {
    if (!tracked(h) || !ov) return real_ReadFileEx(h, buf, n, ov, done);
    uint64_t pos = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    uint64_t ta = g_table.offset, tb = g_table.offset + g_table.data.size();
    if (pos + n <= ta || pos >= tb) return real_ReadFileEx(h, buf, n, ov, done);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending[ov] = Pending{done, static_cast<uint8_t*>(buf), pos};
    }
    BOOL r = real_ReadFileEx(h, buf, n, ov, on_read_done);
    if (!r) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending.erase(ov);
    }
    return r;
}

BOOL WINAPI hook_CloseHandle(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_handles.erase(h);
    }
    return real_CloseHandle(h);
}

// Swaps Fable.exe's import-table slots for kernel32 functions, matched by import name.
int patch_imports(const std::map<std::string, void*>& hooks) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int done = 0;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), "kernel32.dll") != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            auto it = hooks.find(reinterpret_cast<char*>(by_name->Name));
            if (it == hooks.end()) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(it->second);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            ++done;
        }
    }
    return done;
}

}  // namespace

namespace fo::herolook {

void install() {
    char wanted[128] = {};
    DWORD n = GetEnvironmentVariableA("FABLE_ONLINE_HERO_MESH", wanted, sizeof(wanted));
    if (n == 0 || n >= sizeof(wanted)) return;
    fo::log_open();

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    real_CreateFileA = reinterpret_cast<decltype(real_CreateFileA)>(GetProcAddress(k32, "CreateFileA"));
    real_CreateFileW = reinterpret_cast<decltype(real_CreateFileW)>(GetProcAddress(k32, "CreateFileW"));
    real_ReadFile = reinterpret_cast<decltype(real_ReadFile)>(GetProcAddress(k32, "ReadFile"));
    real_ReadFileEx = reinterpret_cast<decltype(real_ReadFileEx)>(GetProcAddress(k32, "ReadFileEx"));
    real_CloseHandle = reinterpret_cast<decltype(real_CloseHandle)>(GetProcAddress(k32, "CloseHandle"));

    fo::log("herolook: hero mesh -> %s", wanted);
    if (!build_table(wanted)) { fo::log("herolook: off"); return; }

    int hooked = patch_imports({
        {"CreateFileA", reinterpret_cast<void*>(hook_CreateFileA)},
        {"CreateFileW", reinterpret_cast<void*>(hook_CreateFileW)},
        {"ReadFile", reinterpret_cast<void*>(hook_ReadFile)},
        {"ReadFileEx", reinterpret_cast<void*>(hook_ReadFileEx)},
        {"CloseHandle", reinterpret_cast<void*>(hook_CloseHandle)},
    });
    fo::log("herolook: on (%d imports hooked)", hooked);
}

}  // namespace fo::herolook
