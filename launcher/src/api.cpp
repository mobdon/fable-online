#include "api.h"
#include "http.h"
#include "json.h"
#include <windows.h>

namespace api {
namespace {

// A sentence a player can read, from a failed response.
std::string describe(const http::Response& r) {
    if (r.status == 0) return r.error.empty() ? "can't reach the server" : r.error;
    json::Value v;
    if (json::parse(r.body, v)) {
        std::string m = v["message"].str();
        if (!m.empty()) return m;
        std::string e = v["error"].str();
        if (e == "bad_credentials") return "Wrong username or password.";
        if (e == "name_taken") return "That name is taken.";
        if (e == "rate_limited") return "Too many tries. Wait a minute and try again.";
        if (e == "outdated") return "Your client is out of date. Restart the launcher to update.";
        if (e == "region_full") return "That region is full.";
        if (!e.empty()) return e;
    }
    return "server error (HTTP " + std::to_string(r.status) + ")";
}

bool read_session(const http::Response& r, Session& s, std::string& error) {
    json::Value v;
    if (r.status != 200) { error = describe(r); return false; }
    if (!json::parse(r.body, v) || v["access_token"].str().empty()) { error = "bad login reply from server"; return false; }
    s.access_token = v["access_token"].str();
    s.refresh_token = v["refresh_token"].str(s.refresh_token);
    s.account = v["account"]["name"].str(s.account);
    double ttl = v["expires_in"].num(900);
    s.expires_at = GetTickCount64() + static_cast<unsigned long long>(ttl * 1000);
    return true;
}

}  // namespace

bool Session::valid() const {
    // 30 s of slack so a token doesn't expire mid-request.
    return !access_token.empty() && GetTickCount64() + 30000 < expires_at;
}

Status status(const Config& c) {
    Status st;
    http::Response r = http::request("GET", c.api("/status"));
    if (r.status != 200) { st.error = describe(r); return st; }
    json::Value v;
    if (!json::parse(r.body, v)) { st.error = "bad status reply"; return st; }
    st.reachable = true;
    st.online = v["online"].boolean();
    st.motd = v["motd"].str();
    st.protocol = int(v["protocol"].num());
    const json::Value& regs = v["regions"];
    for (size_t i = 0; i < regs.size(); ++i) {
        Region g;
        g.id = regs[i]["id"].str();
        g.name = regs[i]["name"].str(g.id);
        g.status = regs[i]["status"].str("offline");
        g.players = int(regs[i]["players"].num());
        g.capacity = int(regs[i]["capacity"].num());
        if (!g.id.empty()) st.regions.push_back(g);
    }
    return st;
}

bool news(const Config& c, std::vector<NewsItem>& out, std::string& error) {
    http::Response r = http::request("GET", c.api("/news"));
    if (r.status != 200) { error = describe(r); return false; }
    json::Value v;
    if (!json::parse(r.body, v)) { error = "bad news reply"; return false; }
    out.clear();
    const json::Value& items = v["items"];
    for (size_t i = 0; i < items.size() && i < 20; ++i)
        out.push_back({items[i]["title"].str(), items[i]["date"].str(), items[i]["body"].str(), items[i]["url"].str()});
    return true;
}

bool login(const Config& c, const std::string& user, const std::string& password, bool create,
           Session& out, std::string& error) {
    std::string body = "{\"username\":" + json::quote(user) + ",\"password\":" + json::quote(password) + "}";
    http::Response r = http::request("POST", c.api(create ? "/register" : "/login"), body);
    SecureZeroMemory(&body[0], body.size());
    Session s;
    s.account = user;
    if (!read_session(r, s, error)) return false;
    out = s;
    return true;
}

bool refresh(const Config& c, Session& s, std::string& error) {
    if (s.refresh_token.empty()) { error = "not logged in"; return false; }
    http::Response r = http::request("POST", c.api("/refresh"), "{\"refresh_token\":" + json::quote(s.refresh_token) + "}");
    if (r.status == 401) s.refresh_token.clear();
    return read_session(r, s, error);
}

void logout(const Config& c, const Session& s) {
    if (s.valid()) http::request("POST", c.api("/logout"), "{}", s.access_token);
}

bool join(const Config& c, Session& s, const std::string& region, const std::string& version,
          Ticket& out, std::string& error) {
    if (!s.valid() && !refresh(c, s, error)) return false;
    std::string body = "{\"region\":" + json::quote(region) + ",\"client_version\":" + json::quote(version) + "}";
    http::Response r = http::request("POST", c.api("/join"), body, s.access_token);
    if (r.status != 200) { error = describe(r); return false; }
    json::Value v;
    if (!json::parse(r.body, v) || v["ticket"].str().empty()) { error = "bad join reply from server"; return false; }
    out.ticket = v["ticket"].str();
    out.host = v["host"].str();
    out.port = int(v["port"].num());
    const json::Value& chat = v["chat"];
    out.chat_token = chat["token"].str();
    out.chat_host = chat["host"].str().empty() ? out.host : chat["host"].str();
    out.chat_port = int(chat["port"].num());
    return true;
}

}  // namespace api
