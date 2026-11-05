#pragma once
// TCP connection to Realm chat (server/src/chat, inside the game server). One JSON object per line.
// Runs on its own thread; reconnects with backoff. The game thread only touches the queues.
#include <string>
#include <vector>

namespace fo::net {

struct Event {
    enum class Kind { Line, Connected, Disconnected } kind;
    std::string text;   // the JSON line, or why we disconnected
};

void chat_start(const std::string& host, unsigned short port, const std::string& hello_line);
void chat_stop();
void chat_send(const std::string& json_line);   // queued; dropped while disconnected
std::vector<Event> chat_poll();                   // everything received since the last call
bool chat_connected();

}  // namespace fo::net
