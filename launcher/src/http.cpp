#include "http.h"
#include "util.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>

namespace http {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

std::string win_error(const char* where) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (error %lu)", where, GetLastError());
    return buf;
}

struct Url {
    bool https = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
};

bool crack(const std::string& url, Url& out) {
    std::wstring w = widen(url);
    URL_COMPONENTS c = {sizeof(c)};
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    c.lpszHostName = host; c.dwHostNameLength = 256;
    c.lpszUrlPath = path; c.dwUrlPathLength = 2048;
    c.lpszExtraInfo = extra; c.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c)) return false;
    if (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS) return false;
    out.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    out.host = host;
    out.path = std::wstring(path) + extra;
    if (out.path.empty()) out.path = L"/";
    out.port = c.nPort;
    return true;
}

bool loopback(const std::wstring& host) {
    return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
}

// Opens a request and sends it; the caller reads the response.
Response open(const std::string& method, const std::string& url, const std::string& body,
              const std::string& bearer, Handle& session, Handle& conn, Handle& req) {
    Response r;
    Url u;
    if (!crack(url, u)) { r.error = "bad URL: " + url; return r; }
    if (!u.https && !loopback(u.host)) { r.error = "refusing plain http to " + narrow(u.host); return r; }
    session.h = WinHttpOpen(L"ChaptersOfTheLostLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = win_error("WinHttpOpen"); return r; }
    WinHttpSetTimeouts(session, 5000, 10000, 15000, 30000);
    // Prefer modern TLS where the OS supports the flag; older Windows ignores it.
    DWORD protocols = 0x00000800 /*TLS1_2*/ | 0x00002000 /*TLS1_3*/;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    conn.h = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (!conn) { r.error = win_error("WinHttpConnect"); return r; }
    req.h = WinHttpOpenRequest(conn, widen(method).c_str(), u.path.c_str(), nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { r.error = win_error("WinHttpOpenRequest"); return r; }
    std::wstring headers = L"Accept: application/json\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
    if (!WinHttpSendRequest(req, headers.c_str(), DWORD(-1),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            DWORD(body.size()), DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_CANNOT_CONNECT ? "can't reach the server"
                : e == ERROR_WINHTTP_TIMEOUT ? "the server timed out"
                : e == ERROR_WINHTTP_SECURE_FAILURE ? "the server's certificate is not valid"
                : win_error("request");
        return r;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = int(status);
    return r;
}

}  // namespace

bool url_allowed(const std::string& url) {
    Url u;
    return crack(url, u) && (u.https || loopback(u.host));
}

Response request(const std::string& method, const std::string& url,
                 const std::string& body, const std::string& bearer) {
    Handle s, c, q;
    Response r = open(method, url, body, bearer, s, c, q);
    if (!r.status) return r;
    char buf[8192];
    DWORD got = 0;
    while (WinHttpReadData(q, buf, sizeof buf, &got) && got) {
        r.body.append(buf, got);
        if (r.body.size() > (8u << 20)) { r.status = 0; r.error = "response too large"; break; }
    }
    return r;
}

Response download(const std::string& url, const std::wstring& path,
                  const std::function<bool(unsigned long long, unsigned long long)>& progress) {
    Handle s, c, q;
    Response r = open("GET", url, "", "", s, c, q);
    if (!r.status) return r;
    if (r.status != 200) { r.error = "download got HTTP " + std::to_string(r.status); return r; }
    wchar_t len[32] = {};
    DWORD lsize = sizeof(len);
    unsigned long long total = 0;
    if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            len, &lsize, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(len, nullptr, 10);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { r.status = 0; r.error = win_error("CreateFile"); return r; }
    char buf[64 * 1024];
    DWORD got = 0;
    unsigned long long done = 0;
    bool ok = true;
    for (;;) {
        if (!WinHttpReadData(q, buf, sizeof buf, &got)) { ok = false; r.error = win_error("WinHttpReadData"); break; }
        if (!got) break;
        DWORD wrote = 0;
        if (!WriteFile(f, buf, got, &wrote, nullptr) || wrote != got) { ok = false; r.error = win_error("WriteFile"); break; }
        done += got;
        if (progress && !progress(done, total)) { ok = false; r.error = "cancelled"; break; }
    }
    CloseHandle(f);
    if (!ok) { DeleteFileW(path.c_str()); r.status = 0; }
    return r;
}

}  // namespace http
