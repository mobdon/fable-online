#include "patch.h"
#include <cstring>

namespace fo::patch {
namespace {
bool write_ptr(void** where, void* value) {
    DWORD old;
    if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *where = value;
    VirtualProtect(where, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, sizeof(void*));
    return true;
}
}  // namespace

void* iat(HMODULE module, const char* dll, const char* func, void* hook) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(byname->Name), func) != 0) continue;
            void** slot = reinterpret_cast<void**>(&slots->u1.Function);
            void* orig = *slot;
            if (orig == hook) return nullptr;
            return write_ptr(slot, hook) ? orig : nullptr;
        }
    }
    return nullptr;
}

void* vtable(void* obj, int index, void* hook) {
    void** vt = *reinterpret_cast<void***>(obj);
    void* orig = vt[index];
    if (orig == hook) return nullptr;
    return write_ptr(&vt[index], hook) ? orig : nullptr;
}
}  // namespace fo::patch
