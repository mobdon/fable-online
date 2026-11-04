// Gateway client. Contract: shared/protocol/launcher-api.md. Blocking calls.
#pragma once
#include "config.h"
#include <string>
#include <vector>

namespace api {

struct Region {
    std::string id, name, status;
    int players = 0, capacity = 0;
    bool joinable() const { return status == "online" || status == "busy"; }
};

struct Status {
    bool reachable = false;
    bool online = false;
    std::string motd;
    int protocol = 0;
    std::vector<Region> regions;
    std::string error;
};

struct NewsItem { std::string title, date, body, url; };

struct Session {
    std::string access_token, refresh_token, account;
    unsigned long long expires_at = 0;  // GetTickCount64 ms
    bool valid() const;
};

struct Ticket {
    std::string ticket, host;
    int port = 0;
    // Realm chat: where the mod's chat client connects, and its login token. Empty when the
    // server runs without chat.
    std::string chat_host, chat_token;
    int chat_port = 0;
};

Status status(const Config& c);
bool news(const Config& c, std::vector<NewsItem>& out, std::string& error);
// create = true uses /register instead of /login.
bool login(const Config& c, const std::string& user, const std::string& password, bool create,
           Session& out, std::string& error);
bool refresh(const Config& c, Session& s, std::string& error);
void logout(const Config& c, const Session& s);
bool join(const Config& c, Session& s, const std::string& region, const std::string& version,
          Ticket& out, std::string& error);

}  // namespace api
