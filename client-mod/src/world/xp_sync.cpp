#include "xp_sync.h"

#include <cmath>
#include <cstdio>

#include "../ui/json.h"

namespace fo::world {

bool same_xp(const XpSnapshot& a, const XpSnapshot& b) {
    for (int i = 0; i < kXpPools; ++i)
        if (a.pools[i] != b.pools[i]) return false;
    for (int i = 0; i < kXpStats; ++i)
        if (a.stats[i] != b.stats[i]) return false;
    for (int i = 0; i < kXpAbilities; ++i)
        if (a.abilities[i] != b.abilities[i]) return false;
    return true;
}

namespace {

bool ints(const json::Value& v, int32_t* out, int n, double max) {
    if (v.type != json::Value::Type::Array || static_cast<int>(v.arr.size()) > n) return false;
    for (int i = 0; i < n; ++i) out[i] = 0;
    for (size_t i = 0; i < v.arr.size(); ++i) {
        const json::Value& x = v.arr[i];
        if (x.type != json::Value::Type::Number || x.n < 0 || x.n > max || std::floor(x.n) != x.n) return false;
        out[i] = static_cast<int32_t>(x.n);
    }
    return true;
}

void append_ints(std::string& s, const int32_t* v, int n) {
    s += '[';
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += std::to_string(v[i]);
    }
    s += ']';
}

}  // namespace

bool parse_xp_state(const std::string& text, XpState& out) {
    json::Value v;
    if (!json::parse(text, v) || !v.is_object() || v.str("t") != "xp_state") return false;
    XpState st;
    st.seq = v.num("seq", -1);
    if (!(st.seq > 0)) return false;
    if (!ints(v["pools"], st.s.pools, kXpPools, 2e9) || !ints(v["stats"], st.s.stats, kXpStats, 64) ||
        !ints(v["abilities"], st.s.abilities, kXpAbilities, 64))
        return false;
    double total = v.num("total", 0);
    st.s.total = total > 0 && total < 2e9 ? static_cast<int32_t>(total) : 0;
    st.why = v.str("why");
    st.bought = static_cast<int>(v["bought"].arr.size());
    st.refused = static_cast<int>(v["refused"].arr.size());
    out = st;
    return true;
}

std::string xp_sync_frame(const XpSnapshot& local, double base) {
    char head[64];
    std::snprintf(head, sizeof head, "{\"t\":\"xp_sync\",\"base\":%.0f,\"pools\":", base);
    std::string s = head;
    int32_t pools[kXpPools];
    for (int i = 0; i < kXpPools; ++i) pools[i] = local.pools[i] < 0 ? 0 : local.pools[i];
    append_ints(s, pools, kXpPools);
    s += ",\"stats\":";
    append_ints(s, local.stats, kXpStats);
    s += ",\"abilities\":";
    append_ints(s, local.abilities, kXpAbilities);
    s += '}';
    return s;
}

bool XpSync::step(const XpSnapshot& local, uint32_t nowMs, std::string& frame) {
    if (pending_ && nowMs - sentMs_ < kXpAnswerMs) return false;
    if (!sentFirst_) {
        // First contact: the server imports the save once, or answers with what it has.
        sentFirst_ = true;
        pending_ = true;
        sentMs_ = nowMs;
        frame = xp_sync_frame(local, 0);
        return true;
    }
    if (!haveServer_) {
        // The first report went unanswered (an older server, or a dropped frame): ask again.
        if (!pending_) return false;
        sentMs_ = nowMs;
        frame = xp_sync_frame(local, 0);
        return true;
    }
    if (same_xp(local, applied_)) {
        haveLast_ = false;
        pending_ = false;
        return false;
    }
    if (!haveLast_ || !same_xp(local, last_)) {
        last_ = local;
        haveLast_ = true;
        changedMs_ = nowMs;
        return false;
    }
    if (nowMs - changedMs_ < kXpSettleMs) return false;
    pending_ = true;
    sentMs_ = nowMs;
    frame = xp_sync_frame(local, appliedSeq_);
    return true;
}

void XpSync::on_state(const XpState& st) {
    // Frames arrive in order; an older state never replaces a newer one.
    if (haveServer_ && st.seq < server_.seq) return;
    server_ = st;
    haveServer_ = true;
    if (st.why == "sync" || st.why == "import") {
        // The answer to our report: write it even if nothing changed on the server, so a
        // level it refused (or a pool it didn't take) goes back to what it holds.
        pending_ = false;
        forceWrite_ = true;
    }
}

bool XpSync::take_write(const XpSnapshot& local, XpSnapshot& out) {
    if (!haveServer_ || pending_) return false;
    if (server_.seq == appliedSeq_ && !forceWrite_) return false;
    forceWrite_ = false;
    if (same_xp(local, server_.s) && local.total == server_.s.total) {
        // Already there: nothing to write, but this is now the state Fable is in.
        applied_ = server_.s;
        appliedSeq_ = server_.seq;
        haveLast_ = false;
        return false;
    }
    out = server_.s;
    return true;
}

void XpSync::wrote() {
    applied_ = server_.s;
    appliedSeq_ = server_.seq;
    haveLast_ = false;
}

}  // namespace fo::world
