// Test-only armour recolour.
//
// With FABLE_ONLINE_ARMOUR_LOOK=1 (or green) set, the hero's plate armour textures
// (TEXTURE_HERO_PLATEARMOUR_* and TEXTURE_HERO_PLATINUM_*) are recoloured as the game loads them: bare steel becomes a
// green lacquer and dark steel a deep blue, while leather and brass stay as painted
// (fo::art::Tint). FABLE_ONLINE_ARMOUR_LOOK=black gives a near-black finish instead, for the
// dev and moderator set. The pixels come from the player's own data/graphics/pc/textures.big and
// never leave their machine.
//
// How: at start-up this file reads each plate texture's top mip, recolours it and keeps it in
// memory. It then rewrites those textures' entries in textures.big's entry tables as the game
// reads them, so each entry points at a region past the end of the file, stored uncompressed
// with one mip. Reads of that region are answered from memory. GetFileSize(Ex) reports the
// file as long enough to hold it.
//
// Before/after previews go to <Fable folder>\fable_online_armour_preview\ as .bmp files.
//
// Hooks: CreateFileA/W, ReadFile, ReadFileEx, GetFileSize, GetFileSizeEx and CloseHandle in
// Fable.exe's import table, chained after any hook already in a slot (herolook). Only handles
// to textures.big are touched.
#include "armourlook.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../../shared/art/big.h"
#include "../../shared/art/dxt.h"
#include "../../shared/art/tint.h"
#include "log.h"

namespace {

// Plate armour, and the platinum set the dev kit (F9) hands out, so a test hero sees it at once.
constexpr const char* kPrefixes[] = {"TEXTURE_HERO_PLATEARMOUR_", "TEXTURE_HERO_PLATINUM_"};

bool wanted(const std::string& name) {
    for (const char* p : kPrefixes)
        if (name.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

struct Table {
    uint64_t offset = 0;
    std::vector<uint8_t> data;  // the bank's entry table, with our entries rewritten
};
std::vector<Table> g_tables;
const char* g_finish = "green";

uint64_t g_file_size = 0;    // the real textures.big size
uint64_t g_virt_base = 0;    // where our region starts (past the end of the file)
std::vector<uint8_t> g_virt; // the recoloured mips, back to back

std::mutex g_mu;
std::set<HANDLE> g_handles;
struct Pending { LPOVERLAPPED_COMPLETION_ROUTINE done; uint8_t* buf; uint64_t pos; };
std::map<LPOVERLAPPED, Pending> g_pending;
struct Apc { LPOVERLAPPED_COMPLETION_ROUTINE done; DWORD n; LPOVERLAPPED ov; };
bool g_logged_table = false, g_logged_virt = false;

decltype(&CreateFileA) real_CreateFileA = nullptr;
decltype(&CreateFileW) real_CreateFileW = nullptr;
decltype(&ReadFile) real_ReadFile = nullptr;
decltype(&ReadFileEx) real_ReadFileEx = nullptr;
decltype(&GetFileSize) real_GetFileSize = nullptr;
decltype(&GetFileSizeEx) real_GetFileSizeEx = nullptr;
decltype(&CloseHandle) real_CloseHandle = nullptr;

uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- previews -----------------------------------------------------------------------------

void write_bmp(const std::wstring& path, const fo::art::Image& im) {
    if (im.empty()) return;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    uint32_t row = static_cast<uint32_t>(im.w) * 4, data = row * static_cast<uint32_t>(im.h);
    uint8_t h[54] = {'B', 'M'};
    put32(h + 2, 54 + data); put32(h + 10, 54); put32(h + 14, 40);
    put32(h + 18, static_cast<uint32_t>(im.w)); put32(h + 22, static_cast<uint32_t>(-im.h));  // top-down
    h[26] = 1; h[28] = 32; put32(h + 34, data);
    std::fwrite(h, 1, sizeof h, f);
    std::fwrite(im.px.data(), 4, im.px.size(), f);  // 0xAARRGGBB little-endian is BGRA
    std::fclose(f);
}

void preview(const std::wstring& dir, const fo::art::Texture& t, const char* suffix) {
    fo::art::Image im;
    if (fo::art::decode(t, im)) write_bmp(dir + L"\\" + widen(t.name) + widen(suffix) + L".bmp", im);
}

// ---- building the replacement -------------------------------------------------------------

struct Replacement { uint32_t offset, size; };

bool build(const std::wstring& big_path) {
    fo::art::Bank bank;
    if (!bank.open(big_path)) { fo::log("armourlook: can't read textures.big (%s)", bank.error().c_str()); return false; }

    std::wstring dir = exe_dir() + L"\\fable_online_armour_preview";
    CreateDirectoryW(dir.c_str(), nullptr);

    HANDLE f = real_CreateFileW(big_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { fo::log("armourlook: can't open textures.big (%lu)", GetLastError()); return false; }
    LARGE_INTEGER sz{};
    real_GetFileSizeEx(f, &sz);
    g_file_size = static_cast<uint64_t>(sz.QuadPart);
    g_virt_base = (g_file_size + 0x10000) & ~uint64_t(0xFFFF);
    if (g_virt_base > 0xF0000000ull) { real_CloseHandle(f); fo::log("armourlook: textures.big too large"); return false; }

    fo::art::Tint tint = fo::art::tint_preset(g_finish);
    std::map<std::string, Replacement> repl;
    for (const auto& name : bank.names()) {
        if (!wanted(name)) continue;
        fo::art::Texture t;
        if (!bank.load(name, t)) { fo::log("armourlook: %s: %s", name.c_str(), bank.error().c_str()); continue; }
        preview(dir, t, "_before");
        switch (t.pixels) {
        case fo::art::PixelFormat::DXT1: fo::art::tint_dxt(1, t.mip0.data(), t.mip0.size(), tint); break;
        case fo::art::PixelFormat::DXT3: fo::art::tint_dxt(3, t.mip0.data(), t.mip0.size(), tint); break;
        case fo::art::PixelFormat::DXT5: fo::art::tint_dxt(5, t.mip0.data(), t.mip0.size(), tint); break;
        case fo::art::PixelFormat::BGRA8: fo::art::tint_bgra(t.mip0.data(), t.mip0.size(), tint); break;
        }
        preview(dir, t, "_after");
        while (g_virt.size() % 2048) g_virt.push_back(0);
        Replacement r{static_cast<uint32_t>(g_virt_base + g_virt.size()), static_cast<uint32_t>(t.mip0.size())};
        g_virt.insert(g_virt.end(), t.mip0.begin(), t.mip0.end());
        repl[name] = r;
        fo::log("armourlook: recoloured %s (%ux%u, format %u, %u bytes)", name.c_str(), t.width, t.height,
                t.format, r.size);
    }
    if (repl.empty()) { real_CloseHandle(f); fo::log("armourlook: no plate armour textures found"); return false; }

    // Bank index: u32 count, then per bank: name\0, u32 id, count, table offset, table size, u32.
    auto read_at = [&](uint64_t pos, void* out, DWORD n) {
        LONG hi = static_cast<LONG>(pos >> 32);
        SetFilePointer(f, static_cast<LONG>(pos & 0xFFFFFFFF), &hi, FILE_BEGIN);
        DWORD got = 0;
        return real_ReadFile(f, out, n, &got, nullptr) && got == n;
    };
    uint8_t hdr[16] = {};
    std::vector<uint8_t> idx;
    if (read_at(0, hdr, sizeof hdr) && std::memcmp(hdr, "BIGB", 4) == 0) {
        uint64_t bi = u32(hdr + 8);
        idx.resize(static_cast<size_t>(std::min<uint64_t>(8192, g_file_size - bi)));
        if (!read_at(bi, idx.data(), static_cast<DWORD>(idx.size()))) idx.clear();
    }
    size_t patched = 0, p = 4;
    uint32_t banks = idx.size() >= 4 ? u32(idx.data()) : 0;
    for (uint32_t b = 0; b < banks && b < 64; ++b) {
        while (p < idx.size() && idx[p]) ++p;
        ++p;
        if (p + 20 > idx.size()) break;
        uint32_t count = u32(&idx[p + 4]), toff = u32(&idx[p + 8]), tsize = u32(&idx[p + 12]);
        p += 20;
        if (!tsize || toff + uint64_t(tsize) > g_file_size || tsize > (64u << 20)) continue;
        Table tab;
        tab.offset = toff;
        tab.data.resize(tsize);
        if (!read_at(toff, tab.data.data(), tsize)) continue;
        auto& d = tab.data;
        size_t q = 0;
        auto need = [&](size_t n) { return q + n <= d.size(); };
        if (!need(4)) continue;
        q = 4 + size_t(u32(&d[0])) * 8;
        bool changed = false;
        for (uint32_t i = 0; i < count && need(28); ++i) {
            size_t e = q;
            uint32_t nlen = u32(&d[q + 24]);
            q += 28;
            if (nlen > 256 || !need(nlen + 8)) break;
            std::string name(reinterpret_cast<const char*>(&d[q]), nlen);
            q += nlen;
            uint32_t nsrc = u32(&d[q + 4]);
            q += 8;
            for (uint32_t s = 0; s < nsrc && need(4); ++s) q += 4 + u32(&d[q]);
            if (!need(4)) break;
            uint32_t sl = u32(&d[q]);
            q += 4;
            if (!need(sl)) break;
            auto it = repl.find(name);
            if (it != repl.end() && sl >= 28) {
                // Entry: size and offset. Subheader: mip count (byte 17) and mip0's stored size
                // (bytes 24..27; equal to mip0's size means stored uncompressed).
                put32(&d[e + 12], it->second.size);
                put32(&d[e + 16], it->second.offset);
                d[q + 17] = 1;
                put32(&d[q + 24], it->second.size);
                changed = true;
                ++patched;
            }
            q += sl;
        }
        if (changed) g_tables.push_back(std::move(tab));
    }
    real_CloseHandle(f);
    fo::log("armourlook: %u textures recoloured, %u table entries rewritten, %u bytes kept in memory",
            static_cast<unsigned>(repl.size()), static_cast<unsigned>(patched), static_cast<unsigned>(g_virt.size()));
    return patched > 0;
}

// ---- serving reads ------------------------------------------------------------------------

// Fills buf for [pos, pos+n): table bytes are already in buf from the real read; bytes past the
// real file come from our region (zeros in the gap). Returns how many bytes the read now covers.
DWORD fill(uint8_t* buf, uint64_t pos, DWORD n, DWORD real_got) {
    uint64_t a = pos, b = pos + n;
    for (const auto& t : g_tables) {
        uint64_t ta = t.offset, tb = t.offset + t.data.size();
        if (b <= ta || a >= tb) continue;
        uint64_t lo = a > ta ? a : ta, hi = b < tb ? b : tb;
        if (lo - a >= real_got) continue;
        if (hi - a > real_got) hi = a + real_got;
        std::memcpy(buf + (lo - a), t.data.data() + (lo - ta), static_cast<size_t>(hi - lo));
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_logged_table) { g_logged_table = true; fo::log("armourlook: patched an entry table as the game read it"); }
    }
    uint64_t vend = g_virt_base + g_virt.size();
    if (b <= g_file_size || a >= vend) return real_got;
    uint64_t start = a > g_file_size ? a : g_file_size;
    if (start - a > real_got) return real_got;  // a short read before the end of the file: leave it
    if (b > vend) b = vend;
    for (uint64_t x = start; x < b; ++x)
        buf[x - a] = x >= g_virt_base ? g_virt[static_cast<size_t>(x - g_virt_base)] : 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_logged_virt) { g_logged_virt = true; fo::log("armourlook: served recoloured texture data"); }
    }
    return static_cast<DWORD>(b - a);
}

bool tracked(HANDLE h) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_handles.count(h) != 0;
}

bool is_textures_big(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = static_cast<wchar_t>(towlower(c));
    const std::wstring tail = L"\\pc\\textures.big", tail2 = L"/pc/textures.big";
    auto ends = [&](const std::wstring& t) { return p.size() >= t.size() && p.compare(p.size() - t.size(), t.size(), t) == 0; };
    return ends(tail) || ends(tail2);
}

void track(HANDLE h, const std::wstring& path) {
    if (h == INVALID_HANDLE_VALUE || !is_textures_big(path)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_handles.insert(h);
}

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
    auto* out = static_cast<uint8_t*>(buf);
    if (pos >= g_file_size) {
        // Wholly in our region: answer it without touching the file.
        DWORD done = fill(out, pos, n, 0);
        if (got) *got = done;
        if (ov) {
            ov->Internal = 0;
            ov->InternalHigh = done;
            if (ov->hEvent) SetEvent(ov->hEvent);
        } else {
            LONG hi = static_cast<LONG>((pos + done) >> 32);
            SetFilePointer(h, static_cast<LONG>((pos + done) & 0xFFFFFFFF), &hi, FILE_BEGIN);
        }
        return TRUE;
    }
    DWORD local = 0;
    if (!got && !ov) got = &local;
    BOOL r = real_ReadFile(h, buf, n, got, ov);
    if (r) {
        DWORD real_got = got ? *got : n;
        DWORD done = fill(out, pos, n, real_got);
        if (done != real_got) {
            if (got) *got = done;
            if (!ov) {
                LONG hi = static_cast<LONG>((pos + done) >> 32);
                SetFilePointer(h, static_cast<LONG>((pos + done) & 0xFFFFFFFF), &hi, FILE_BEGIN);
            }
        }
    } else if (GetLastError() == ERROR_IO_PENDING) {
        for (const auto& t : g_tables)
            if (pos < t.offset + t.data.size() && pos + n > t.offset)
                fo::log("armourlook: an entry table was read asynchronously through ReadFile; not patched");
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
    if (err == 0) n = fill(p.buf, p.pos, n, n);
    if (p.done) p.done(err, n, ov);
}

VOID CALLBACK run_apc(ULONG_PTR arg) {
    auto* a = reinterpret_cast<Apc*>(arg);
    a->done(0, a->n, a->ov);
    delete a;
}

BOOL WINAPI hook_ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE done) {
    if (!tracked(h) || !ov) return real_ReadFileEx(h, buf, n, ov, done);
    uint64_t pos = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    if (pos >= g_file_size) {
        DWORD got = fill(static_cast<uint8_t*>(buf), pos, n, 0);
        ov->Internal = 0;
        ov->InternalHigh = got;
        if (done) QueueUserAPC(run_apc, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(new Apc{done, got, ov}));
        return TRUE;
    }
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

uint64_t reported_size() { return g_virt_base + g_virt.size(); }

DWORD WINAPI hook_GetFileSize(HANDLE h, LPDWORD high) {
    if (!tracked(h)) return real_GetFileSize(h, high);
    uint64_t s = reported_size();
    if (high) *high = static_cast<DWORD>(s >> 32);
    return static_cast<DWORD>(s & 0xFFFFFFFF);
}

BOOL WINAPI hook_GetFileSizeEx(HANDLE h, PLARGE_INTEGER out) {
    if (!tracked(h)) return real_GetFileSizeEx(h, out);
    if (out) out->QuadPart = static_cast<LONGLONG>(reported_size());
    return TRUE;
}

BOOL WINAPI hook_CloseHandle(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_handles.erase(h);
    }
    return real_CloseHandle(h);
}

// Swaps Fable.exe's kernel32 import slots. Each slot's current value (possibly another of our
// hooks) is what the matching real_ pointer calls, so hooks chain.
int patch_imports(const std::map<std::string, std::pair<void*, void**>>& hooks) {
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
            *it->second.second = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<uintptr_t>(it->second.first);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            ++done;
        }
    }
    return done;
}

}  // namespace

namespace fo::armourlook {

void install() {
    // 1 or green: the green finish. black: the dark dev-only finish.
    char want[16] = {};
    DWORD n = GetEnvironmentVariableA("FABLE_ONLINE_ARMOUR_LOOK", want, sizeof(want));
    if (n == 0 || n >= sizeof(want) || want[0] == '0') return;
    g_finish = (std::strcmp(want, "black") == 0) ? "black" : "green";
    fo::log_open();

    // For build(); replaced by the import-slot values below once the hooks go in.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    real_CreateFileA = reinterpret_cast<decltype(real_CreateFileA)>(GetProcAddress(k32, "CreateFileA"));
    real_CreateFileW = reinterpret_cast<decltype(real_CreateFileW)>(GetProcAddress(k32, "CreateFileW"));
    real_ReadFile = reinterpret_cast<decltype(real_ReadFile)>(GetProcAddress(k32, "ReadFile"));
    real_ReadFileEx = reinterpret_cast<decltype(real_ReadFileEx)>(GetProcAddress(k32, "ReadFileEx"));
    real_GetFileSize = reinterpret_cast<decltype(real_GetFileSize)>(GetProcAddress(k32, "GetFileSize"));
    real_GetFileSizeEx = reinterpret_cast<decltype(real_GetFileSizeEx)>(GetProcAddress(k32, "GetFileSizeEx"));
    real_CloseHandle = reinterpret_cast<decltype(real_CloseHandle)>(GetProcAddress(k32, "CloseHandle"));

    fo::log("armourlook: recolouring the plate and platinum armour textures (%s finish)", g_finish);
    if (!build(exe_dir() + L"\\data\\graphics\\pc\\textures.big")) { fo::log("armourlook: off"); return; }

    int hooked = patch_imports({
        {"CreateFileA", {reinterpret_cast<void*>(hook_CreateFileA), reinterpret_cast<void**>(&real_CreateFileA)}},
        {"CreateFileW", {reinterpret_cast<void*>(hook_CreateFileW), reinterpret_cast<void**>(&real_CreateFileW)}},
        {"ReadFile", {reinterpret_cast<void*>(hook_ReadFile), reinterpret_cast<void**>(&real_ReadFile)}},
        {"ReadFileEx", {reinterpret_cast<void*>(hook_ReadFileEx), reinterpret_cast<void**>(&real_ReadFileEx)}},
        {"GetFileSize", {reinterpret_cast<void*>(hook_GetFileSize), reinterpret_cast<void**>(&real_GetFileSize)}},
        {"GetFileSizeEx", {reinterpret_cast<void*>(hook_GetFileSizeEx), reinterpret_cast<void**>(&real_GetFileSizeEx)}},
        {"CloseHandle", {reinterpret_cast<void*>(hook_CloseHandle), reinterpret_cast<void**>(&real_CloseHandle)}},
    });
    fo::log("armourlook: on (%d imports hooked)", hooked);
}

}  // namespace fo::armourlook
