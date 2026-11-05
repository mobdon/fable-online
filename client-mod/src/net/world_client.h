#pragma once
// The game connection: one WebSocket to the region server, matching server/src/net/.
// Runs on its own thread and reconnects with backoff; the game side only touches queues.
//
// Text frames only, one JSON object per frame. Nothing here understands the game: frames
// go out as strings and come back as strings (client-mod/src/world/snapshot.h decodes them).
#include <cstdint>
#include <string>
#include <vector>

namespace fo::net {

struct WorldEvent {
    enum class Kind { Frame, Connected, Disconnected } kind;
    std::string text;  // the JSON frame, or why the connection ended
};

struct WorldConfig {
    std::string host;
    unsigned short port = 7777;
    std::string path = "/play";
    std::string ticket;       // one-time join ticket from the launcher
    uint32_t protocol = 1;
    std::string exeTimestamp;
};

/**
 * The launcher's join ticket works once. After the first connection the server hands out a
 * resume token (in `welcome`), which the mod passes here so a dropped connection can come
 * back without a new ticket. With neither left, the connection stops and says why.
 */
void world_set_resume(const std::string& token);

/** Starts the connection thread. Calling it twice replaces the connection. */
void world_start(const WorldConfig& cfg);
void world_stop();
/** Queues one frame. Frames queued while disconnected are dropped, not kept. */
void world_send(const std::string& json);
/** Everything that arrived since the last call. */
std::vector<WorldEvent> world_poll();
bool world_connected();
/** Frames received and sent since the connection started, for the log. */
void world_counters(uint64_t& received, uint64_t& sent);

}  // namespace fo::net
