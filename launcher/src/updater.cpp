#include "updater.h"
#include "crypto.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include <windows.h>

namespace updater {
namespace {
const unsigned long long kMaxFile = 1ull << 30;  // 1 GiB

std::wstring local_path(const std::string& rel) {
    std::wstring w = widen(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return self_dir() + L"\\" + w;
}

unsigned long long file_size(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return ~0ull;
    return (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool same_file(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}  // namespace

bool safe_path(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/') return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string seg = p.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') return false;
        for (char ch : seg) {
            bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                      ch == '.' || ch == '_' || ch == '-' || ch == ' ';
            if (!ok) return false;
        }
        start = end + 1;
    }
    return true;
}

bool fetch(const Config& c, Manifest& out, std::string& error) {
    http::Response r = http::request("GET", c.api("/manifest?channel=" + c.channel));
    if (r.status != 200) {
        error = r.status ? "manifest: HTTP " + std::to_string(r.status) : r.error;
        return false;
    }
    json::Value v;
    if (!json::parse(r.body, v)) { error = "manifest is not valid JSON"; return false; }
    Manifest m;
    m.version = v["version"].str();
    m.protocol = int(v["protocol"].num());
    std::string base = v["base_url"].str();
    if (!base.empty() && base.back() != '/') base += '/';
    const json::Value& files = v["files"];
    for (size_t i = 0; i < files.size(); ++i) {
        File f;
        f.path = files[i]["path"].str();
        f.size = static_cast<unsigned long long>(files[i]["size"].num(-1));
        f.sha256 = files[i]["sha256"].str();
        for (auto& ch : f.sha256) if (ch >= 'A' && ch <= 'F') ch += 32;
        f.url = files[i]["url"].str(base + f.path);
        if (!safe_path(f.path)) { error = "manifest has an unsafe path: " + f.path; return false; }
        if (f.sha256.size() != 64 || f.size > kMaxFile) { error = "manifest entry is malformed: " + f.path; return false; }
        if (!http::url_allowed(f.url)) { error = "manifest URL must be https: " + f.url; return false; }
        m.files.push_back(f);
    }
    if (m.version.empty()) { error = "manifest has no version"; return false; }
    out = m;
    return true;
}

std::vector<File> outdated(const Manifest& m) {
    std::vector<File> need;
    for (auto& f : m.files) {
        std::wstring p = local_path(f.path);
        if (file_size(p) != f.size || sha256_file(p) != f.sha256) need.push_back(f);
    }
    return need;
}

Result apply(const std::vector<File>& files, const Progress& progress) {
    Result r;
    std::wstring self = self_path();
    for (auto& f : files) {
        std::wstring dest = local_path(f.path);
        std::wstring part = dest + L".part";
        make_dirs(dir_of(dest));
        http::Response d = http::download(f.url, part, [&](unsigned long long done, unsigned long long total) {
            if (progress) progress(f.path, done, total ? total : f.size);
            return done <= f.size;  // stop a download that runs past the size we expect
        });
        if (!d.error.empty() || d.status != 200) {
            r.error = f.path + ": " + (d.error.empty() ? "HTTP " + std::to_string(d.status) : d.error);
            return r;
        }
        if (file_size(part) != f.size || sha256_file(part) != f.sha256) {
            DeleteFileW(part.c_str());
            r.error = f.path + ": downloaded file failed its checksum";
            return r;
        }
        if (same_file(dest, self)) {
            // Windows won't overwrite a running exe, but it will rename one.
            std::wstring old = dir_of(self) + L"\\fable_online_launcher.old";
            DeleteFileW(old.c_str());
            if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                r.error = "couldn't move the old launcher aside (error " + std::to_string(GetLastError()) + ")";
                return r;
            }
            if (!MoveFileExW(part.c_str(), self.c_str(), MOVEFILE_WRITE_THROUGH)) {
                MoveFileExW(old.c_str(), self.c_str(), 0);  // put the old one back
                r.error = "couldn't install the new launcher (error " + std::to_string(GetLastError()) + ")";
                return r;
            }
            r.launcher_replaced = true;
        } else if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD e = GetLastError();
            DeleteFileW(part.c_str());
            r.error = f.path + ": couldn't replace the file (error " + std::to_string(e) +
                      (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? ", is Fable still running?)" : ")");
            return r;
        }
    }
    r.ok = true;
    return r;
}

void cleanup_old_launcher() {
    std::wstring old = self_dir() + L"\\fable_online_launcher.old";
    // The previous launcher may still be exiting right after a self-update.
    for (int i = 0; i < 20 && file_exists(old) && !DeleteFileW(old.c_str()); ++i) Sleep(100);
}

bool restart_self() {
    std::wstring exe = self_path();
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace updater
