#include "config.h"
#include "crypto.h"
#include "util.h"
#include <windows.h>
#include <shlobj.h>

namespace {
const wchar_t* kSection = L"launcher";

std::wstring get(const std::wstring& file, const wchar_t* key) {
    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(kSection, key, L"", buf, 4096, file.c_str());
    return buf;
}
void put(const std::wstring& file, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(kSection, key, v.c_str(), file.c_str());
}
}  // namespace

std::wstring Config::path() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return self_dir() + L"\\launcher.ini";
    return std::wstring(buf) + L"\\FableOnline\\launcher.ini";
}

void Config::load() {
    std::wstring f = path();
    if (!file_exists(f)) return;
    auto s = [&](const wchar_t* k, std::string& out) { std::wstring v = get(f, k); if (!v.empty()) out = narrow(v); };
    s(L"gateway", gateway);
    s(L"channel", channel);
    s(L"username", username);
    s(L"region", region);
    s(L"client_version", client_version);
    game_dir = get(f, L"game_dir");
    refresh_token = unprotect(narrow(get(f, L"refresh_token")));
    while (!gateway.empty() && gateway.back() == '/') gateway.pop_back();
}

void Config::save() const {
    std::wstring f = path();
    make_dirs(dir_of(f));
    put(f, L"gateway", widen(gateway));
    put(f, L"channel", widen(channel));
    put(f, L"username", widen(username));
    put(f, L"region", widen(region));
    put(f, L"client_version", widen(client_version));
    put(f, L"game_dir", game_dir);
    put(f, L"refresh_token", widen(protect(refresh_token)));
}
