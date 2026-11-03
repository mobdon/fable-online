#pragma once
// Low-level hooking helpers: import table and COM vtable patches. The only change to
// Fable.exe's code bytes is the camera jump in display/widescreen.cpp.
#include <windows.h>

namespace fo::patch {
// Replaces module's import of dll!func with hook. Returns the original, or nullptr if not imported.
void* iat(HMODULE module, const char* dll, const char* func, void* hook);
// Replaces slot `index` of obj's vtable. Returns the old pointer (or the saved original if
// this slot already points at hook). All objects sharing that vtable are affected.
void* vtable(void* obj, int index, void* hook);
}  // namespace fo::patch
