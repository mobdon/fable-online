#include "chat_client.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include "../log.h"

namespace fo::net {
namespace {
std::mutex g_mu;
std::deque<std::string> g_out;
std::vector<Event> g_in;
std::atomic<bool> g_run{false};
std::atomic<bool> g_up{false};
std::thread* g_thread = nullptr;  // never destroyed: a joinable std::thread in a static would abort at unload
std::string g_host, g_hello;
unsigned short g_port = 0;
constexpr size_t kMaxOut = 64;
constexpr size_t kMaxIn = 512;
constexpr size_t kMaxLine = 64 * 1024;

void push_in(Event e) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_in.size() < kMaxIn) g_in.push_back(std::move(e));
}

SOCKET dial(std::string& why) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port[8];
    wsprintfA(port, "%u", g_port);
    if (getaddrinfo(g_host.c_str(), port, &hints, &res) != 0 || !res) { why = "can't find " + g_host; return INVALID_SOCKET; }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0) { closesocket(s); s = INVALID_SOCKET; }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) { why = "no answer from " + g_host + ":" + port; return s; }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

bool send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n > 0) { off += n; continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        timeval tv = {2, 0};
        if (select(0, nullptr, &w, nullptr, &tv) <= 0) return false;
    }
    return true;
}

// One connection's life. Returns why it ended.
std::string session(SOCKET s) {
    if (!send_all(s, g_hello + "\n")) return "send failed";
    std::string buf;
    DWORD last_ping = GetTickCount();
    char chunk[4096];
    while (g_run) {
        // Wait up to 100 ms for data, so queued sends go out promptly too.
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        timeval tv = {0, 100 * 1000};
        int ready = select(0, &r, nullptr, nullptr, &tv);
        if (ready < 0) return "socket error";
        if (ready > 0) {
            int n = recv(s, chunk, sizeof(chunk), 0);
            if (n == 0) return "server closed the connection";
            if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) return "connection lost";
            if (n > 0) {
                buf.append(chunk, n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!line.empty()) push_in({Event::Kind::Line, std::move(line)});
                }
                if (buf.size() > kMaxLine) return "bad data from server";
            }
        }
        std::deque<std::string> out;
        { std::lock_guard<std::mutex> l(g_mu); out.swap(g_out); }
        for (auto& line : out) if (!send_all(s, line + "\n")) return "send failed";
        if (GetTickCount() - last_ping > 30000) {
            last_ping = GetTickCount();
            if (!send_all(s, "{\"t\":\"ping\",\"n\":" + std::to_string(last_ping) + "}\n")) return "send failed";
        }
    }
    return "";
}

void run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fo::log("chat: WSAStartup failed"); return; }
    DWORD backoff = 1000;
    bool told = false;
    while (g_run) {
        std::string why;
        SOCKET s = dial(why);
        if (s != INVALID_SOCKET) {
            fo::log("chat: connected to %s:%u", g_host.c_str(), g_port);
            g_up = true;
            told = false;
            backoff = 1000;
            push_in({Event::Kind::Connected, ""});
            why = session(s);
            closesocket(s);
            g_up = false;
            { std::lock_guard<std::mutex> l(g_mu); g_out.clear(); }
            if (!g_run) break;
            fo::log("chat: disconnected (%s)", why.c_str());
            push_in({Event::Kind::Disconnected, why});
        } else if (!told) {
            fo::log("chat: %s", why.c_str());
            push_in({Event::Kind::Disconnected, why});
            told = true;  // say it once, then retry quietly
        }
        for (DWORD waited = 0; g_run && waited < backoff; waited += 100) Sleep(100);
        backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
    }
    WSACleanup();
}
}  // namespace

void chat_start(const std::string& host, unsigned short port, const std::string& hello_line) {
    if (g_run) return;
    g_host = host;
    g_port = port;
    g_hello = hello_line;
    g_run = true;
    g_thread = new std::thread(run);
}

void chat_stop() {
    if (!g_run) return;
    g_run = false;
    if (g_thread && g_thread->joinable()) g_thread->join();
}

void chat_send(const std::string& line) {
    if (!g_up) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_out.size() < kMaxOut) g_out.push_back(line);
}

std::vector<Event> chat_poll() {
    std::lock_guard<std::mutex> l(g_mu);
    std::vector<Event> o;
    o.swap(g_in);
    return o;
}

bool chat_connected() { return g_up; }
}  // namespace fo::net
