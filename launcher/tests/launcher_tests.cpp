// Runs the launcher's logic against a running dev gateway (tools/dev-gateway).
// Build with -DFO_BUILD_TESTS=ON; run: launcher_tests.exe http://127.0.0.1:8787
#include "../src/api.h"
#include "../src/crypto.h"
#include "../src/http.h"
#include "../src/json.h"
#include "../src/updater.h"
#include "../src/util.h"
#include <windows.h>
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++failures; } else std::printf("ok   %s\n", #x); } while (0)

int main(int argc, char** argv) {
    // json
    json::Value v;
    CHECK(json::parse(R"({"a":[1,2,{"b":"x\u00e9\ud83d\ude00"}],"t":true,"n":null,"f":-1.5e2})", v));
    CHECK(v["a"][2]["b"].str() == "x\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(v["t"].boolean() && v["n"].is_null() && v["f"].num() == -150);
    CHECK(v["missing"]["deeper"][3].is_null());
    CHECK(!json::parse("{\"a\":1,}", v));
    CHECK(!json::parse("[1] x", v));
    CHECK(!json::parse(std::string(100, '['), v));
    CHECK(json::quote("a\"b\\\n\x01") == "\"a\\\"b\\\\\\n\\u0001\"");

    // manifest path safety
    CHECK(updater::safe_path("fable_online.dll"));
    CHECK(updater::safe_path("overlay/fonts/a b.ttf"));
    for (const char* bad : {"", "/etc/x", "../x.dll", "a/../../x", "C:/Windows/x.dll", "a\\b", "a//b", "a/./b", "x.dll.", "con:x"})
        CHECK(!updater::safe_path(bad));

    // http rules
    CHECK(http::url_allowed("https://gateway.example.net/x"));
    CHECK(http::url_allowed("http://127.0.0.1:8787/x"));
    CHECK(!http::url_allowed("http://evil.example.net/x"));
    CHECK(!http::url_allowed("file:///C:/x"));

    // crypto
    std::wstring tmp = self_dir() + L"\\abc.txt";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w; WriteFile(f, "abc", 3, &w, nullptr); CloseHandle(f);
    CHECK(sha256_file(tmp) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    DeleteFileW(tmp.c_str());
    std::string sealed = protect("refresh-token-123");
    CHECK(!sealed.empty() && sealed.find("refresh") == std::string::npos);
    CHECK(unprotect(sealed) == "refresh-token-123");
    CHECK(unprotect("zz").empty());

    if (argc < 2) { std::printf("(no gateway given, skipping network tests)\n"); return failures ? 1 : 0; }
    Config c;
    c.gateway = argv[1];
    api::Status st = api::status(c);
    CHECK(st.reachable && st.online && st.regions.size() == 3);
    CHECK(st.regions[0].joinable() && !st.regions[2].joinable());
    std::vector<api::NewsItem> items; std::string err;
    CHECK(api::news(c, items, err) && items.size() == 2);

    char name[32]; std::snprintf(name, sizeof name, "hero%lu", GetTickCount() % 100000);
    api::Session s;
    CHECK(api::login(c, name, "short", true, s, err) == false && err == "Use at least 8 characters.");
    CHECK(api::login(c, name, "password123", true, s, err) && s.valid() && s.account == name);
    CHECK(!api::login(c, name, "password123", true, s, err) && err == "That name is taken.");
    api::Session wrong;
    CHECK(!api::login(c, name, "nope-nope-nope", false, wrong, err) && err == "Wrong username or password.");
    CHECK(api::login(c, name, "password123", false, s, err));
    std::string old_refresh = s.refresh_token;
    CHECK(api::refresh(c, s, err) && s.refresh_token != old_refresh);
    api::Session stale = s; stale.refresh_token = old_refresh;
    CHECK(!api::refresh(c, stale, err) && stale.refresh_token.empty());

    api::Ticket t;
    CHECK(!api::join(c, s, "albion-1", "0.0.1", t, err) && err.find("out of date") != std::string::npos);
    CHECK(api::join(c, s, "albion-1", "0.1.0", t, err) && !t.ticket.empty() && t.port == 27015);
    CHECK(!api::join(c, s, "test", "0.1.0", t, err) && err == "Test realm is maintenance.");
    // Expired access token: join refreshes first.
    s.expires_at = 0;
    old_refresh = s.refresh_token;
    CHECK(api::join(c, s, "albion-2", "0.1.0", t, err) && s.refresh_token != old_refresh);

    // patching: the gateway serves the "patch" folder; we patch into our own folder.
    updater::Manifest m;
    CHECK(updater::fetch(c, m, err));
    CHECK(m.version == "0.1.0" && !m.files.empty());
    auto need = updater::outdated(m);
    CHECK(need.size() == m.files.size());
    unsigned long long last = 0;
    updater::Result r = updater::apply(need, [&](const std::string&, unsigned long long d, unsigned long long) { last = d; });
    if (!r.ok) std::printf("     apply error: %s\n", r.error.c_str());
    CHECK(r.ok && last > 0);
    CHECK(updater::outdated(m).empty());
    std::printf("%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
