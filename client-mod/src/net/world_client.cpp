#include "world_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "../log.h"

namespace fo::net {
namespace {

constexpr size_t kMaxFrame = 256 * 1024;  // a region's worth of entities, with room to spare
constexpr size_t kMaxQueued = 64;         // outgoing frames; the newest matter, so drop the rest
constexpr int kRecvTimeoutMs = 1000;

std::mutex g_mu;
std::deque<WorldEvent> g_in;
std::deque<std::string> g_out;
std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};
std::atomic<uint64_t> g_received{0};
std::atomic<uint64_t> g_sent{0};
std::thread g_thread;
WorldConfig g_cfg;

void push_event(WorldEvent::Kind kind, std::string text) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_in.size() > 4096) g_in.pop_front();  // the game side has stopped polling
    g_in.push_back(WorldEvent{kind, std::move(text)});
}

// ---------- socket helpers ----------

bool send_all(SOCKET s, const char* data, size_t n) {
    while (n) {
        int put = ::send(s, data, static_cast<int>(n), 0);
        if (put <= 0) return false;
        data += put;
        n -= static_cast<size_t>(put);
    }
    return true;
}

/** Reads exactly n bytes unless the connection ends or we are shutting down. */
bool recv_all(SOCKET s, char* out, size_t n) {
    while (n) {
        int got = ::recv(s, out, static_cast<int>(n), 0);
        if (got == 0) return false;
        if (got < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT && g_running.load()) continue;  // idle, not broken
            return false;
        }
        out += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

SOCKET connect_to(const std::string& host, unsigned short port) {
    char service[8];
    std::snprintf(service, sizeof(service), "%u", port);
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), service, &hints, &list) != 0 || !list) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = list; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(list);
    if (s != INVALID_SOCKET) {
        DWORD timeout = kRecvTimeoutMs;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        BOOL nodelay = TRUE;  // position frames are small and late ones are useless
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    }
    return s;
}

// ---------- WebSocket framing (RFC 6455, text frames, no extensions) ----------

std::string random_key() {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char raw[16];
    for (unsigned char& b : raw) b = static_cast<unsigned char>(rand() & 0xFF);
    std::string out;
    for (int i = 0; i < 16; i += 3) {
        uint32_t n = static_cast<uint32_t>(raw[i]) << 16 | static_cast<uint32_t>(i + 1 < 16 ? raw[i + 1] : 0) << 8 |
                     static_cast<uint32_t>(i + 2 < 16 ? raw[i + 2] : 0);
        out += b64[(n >> 18) & 63];
        out += b64[(n >> 12) & 63];
        out += i + 1 < 16 ? b64[(n >> 6) & 63] : '=';
        out += i + 2 < 16 ? b64[n & 63] : '=';
    }
    return out;
}

bool handshake(SOCKET s, const WorldConfig& cfg) {
    std::string req = "GET " + cfg.path +
                      " HTTP/1.1\r\n"
                      "Host: " + cfg.host + ":" + std::to_string(cfg.port) +
                      "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + random_key() +
                      "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(s, req.data(), req.size())) return false;

    // Read the response headers one byte at a time: the body that follows is frame data.
    std::string head;
    char c = 0;
    while (head.size() < 8192) {
        if (!recv_all(s, &c, 1)) return false;
        head.push_back(c);
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    bool ok = head.compare(0, 12, "HTTP/1.1 101") == 0;
    if (!ok) fo::log("world: server refused the connection: %.60s", head.c_str());
    return ok;
}

bool send_frame(SOCKET s, uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));
    size_t n = payload.size();
    uint8_t mask[4];
    for (uint8_t& m : mask) m = static_cast<uint8_t>(rand() & 0xFF);
    if (n < 126) {
        out.push_back(static_cast<char>(0x80 | n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF));
    }
    out.append(reinterpret_cast<const char*>(mask), 4);
    size_t base = out.size();
    out.append(payload);
    for (size_t i = 0; i < n; ++i) out[base + i] = static_cast<char>(out[base + i] ^ mask[i % 4]);
    return send_all(s, out.data(), out.size());
}

/** Reads one frame. Returns false when the connection is finished with. */
bool read_frame(SOCKET s, uint8_t& opcode, std::string& payload) {
    unsigned char head[2];
    if (!recv_all(s, reinterpret_cast<char*>(head), 2)) return false;
    opcode = head[0] & 0x0F;
    bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (!recv_all(s, reinterpret_cast<char*>(ext), 2)) return false;
        len = static_cast<uint64_t>(ext[0]) << 8 | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!recv_all(s, reinterpret_cast<char*>(ext), 8)) return false;
        len = 0;
        for (unsigned char b : ext) len = len << 8 | b;
    }
    unsigned char mask[4] = {};
    if (masked && !recv_all(s, reinterpret_cast<char*>(mask), 4)) return false;
    if (len > kMaxFrame) {
        fo::log("world: frame of %llu bytes is too large; dropping the connection",
                static_cast<unsigned long long>(len));
        return false;
    }
    payload.assign(static_cast<size_t>(len), '\0');
    if (len && !recv_all(s, payload.data(), payload.size())) return false;
    if (masked)
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
    return true;
}

std::string hello_frame(const WorldConfig& cfg) {
    std::string out = "{\"t\":\"hello\",\"protocol\":" + std::to_string(cfg.protocol) + ",\"exe_timestamp\":\"";
    for (char c : cfg.exeTimestamp)
        if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    out += "\",\"token\":\"";
    for (char c : cfg.ticket)
        if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    out += "\"}";
    return out;
}

void session(SOCKET s) {
    if (!handshake(s, g_cfg)) return;
    if (!send_frame(s, 0x1, hello_frame(g_cfg))) return;
    g_connected = true;
    push_event(WorldEvent::Kind::Connected, g_cfg.host + ":" + std::to_string(g_cfg.port));
    fo::log("world: connected to %s:%u", g_cfg.host.c_str(), g_cfg.port);

    // One thread writes, this one reads: the writer only exists for as long as the socket.
    std::atomic<bool> writing{true};
    std::thread writer([s, &writing]() {
        while (writing.load() && g_running.load()) {
            std::string frame;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (!g_out.empty()) {
                    frame = std::move(g_out.front());
                    g_out.pop_front();
                }
            }
            if (frame.empty()) {
                Sleep(5);
                continue;
            }
            if (!send_frame(s, 0x1, frame)) break;
            ++g_sent;
        }
    });

    uint8_t opcode = 0;
    std::string payload;
    while (g_running.load()) {
        if (!read_frame(s, opcode, payload)) break;
        if (opcode == 0x8) break;                               // close
        if (opcode == 0x9) { send_frame(s, 0xA, payload); continue; }  // ping -> pong
        if (opcode == 0xA) continue;                            // pong
        if (opcode != 0x1 && opcode != 0x0) continue;           // binary and fragments unused
        ++g_received;
        push_event(WorldEvent::Kind::Frame, payload);
    }

    writing = false;
    ::shutdown(s, SD_BOTH);
    if (writer.joinable()) writer.join();
}

void run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fo::log("world: no winsock; the game connection is off");
        return;
    }
    unsigned backoffMs = 1000;
    while (g_running.load()) {
        SOCKET s = connect_to(g_cfg.host, g_cfg.port);
        if (s == INVALID_SOCKET) {
            push_event(WorldEvent::Kind::Disconnected, "cannot reach the server");
            for (unsigned waited = 0; waited < backoffMs && g_running.load(); waited += 100) Sleep(100);
            backoffMs = backoffMs < 15000 ? backoffMs * 2 : 15000;
            continue;
        }
        backoffMs = 1000;
        session(s);
        ::closesocket(s);
        if (g_connected.exchange(false)) {
            push_event(WorldEvent::Kind::Disconnected, "connection closed");
            fo::log("world: disconnected");
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_out.clear();  // a ticket is one-time, and stale input is worse than none
        }
        for (unsigned waited = 0; waited < 1000 && g_running.load(); waited += 100) Sleep(100);
    }
    WSACleanup();
}

}  // namespace

void world_start(const WorldConfig& cfg) {
    world_stop();
    g_cfg = cfg;
    if (g_cfg.host.empty() || g_cfg.port == 0) {
        fo::log("world: no server address; staying offline");
        return;
    }
    g_received = 0;
    g_sent = 0;
    g_running = true;
    g_thread = std::thread(run);
}

void world_stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    g_connected = false;
    std::lock_guard<std::mutex> lock(g_mu);
    g_in.clear();
    g_out.clear();
}

void world_send(const std::string& json) {
    if (!g_connected.load()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_out.size() >= kMaxQueued) g_out.pop_front();
    g_out.push_back(json);
}

std::vector<WorldEvent> world_poll() {
    std::vector<WorldEvent> out;
    std::lock_guard<std::mutex> lock(g_mu);
    out.reserve(g_in.size());
    for (auto& e : g_in) out.push_back(std::move(e));
    g_in.clear();
    return out;
}

bool world_connected() { return g_connected.load(); }

void world_counters(uint64_t& received, uint64_t& sent) {
    received = g_received.load();
    sent = g_sent.load();
}

}  // namespace fo::net
