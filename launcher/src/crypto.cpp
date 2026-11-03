#include "crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <vector>

namespace {
const char* kHex = "0123456789abcdef";

std::string to_hex(const unsigned char* p, size_t n) {
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += kHex[p[i] >> 4]; s += kHex[p[i] & 15]; }
    return s;
}

bool from_hex(const std::string& s, std::vector<unsigned char>& out) {
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<unsigned char>(a << 4 | b));
    }
    return true;
}
}  // namespace

std::string sha256_file(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(64 * 1024);
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got)
            if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0) out = to_hex(digest, 32);
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

std::string protect(const std::string& plain) {
    if (plain.empty()) return "";
    DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"Chapters of the Lost", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return "";
    std::string s = to_hex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

std::string unprotect(const std::string& hex) {
    std::vector<unsigned char> raw;
    if (hex.empty() || !from_hex(hex, raw)) return "";
    DATA_BLOB in{DWORD(raw.size()), raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return "";
    std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}
