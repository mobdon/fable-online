#include "trade_model.h"
#include <cctype>

namespace fo::ui {

std::string item_label(const std::string& def) {
    std::string d = def;
    for (const char* p : {"OBJECT_", "ITEM_"})
        if (d.rfind(p, 0) == 0) { d = d.substr(std::string(p).size()); break; }
    std::string out;
    bool start = true;
    for (char c : d) {
        if (c == '_') { out += ' '; start = true; continue; }
        out += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                     : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        start = false;
    }
    return out.empty() ? def : out;
}

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
uint32_t u32(double v) { return v > 0 && v < 4.2e9 ? static_cast<uint32_t>(v) : 0; }
}  // namespace

TradeSide TradeModel::parse_side(const json::Value& v) {
    TradeSide s;
    s.gold = u32(v.num("gold"));
    for (const auto& it : v["items"].arr) s.items.push_back({it.str("item"), it.str("def"), u32(it.num("qty", 1))});
    return s;
}

void TradeModel::reset() {
    phase_ = Phase::Idle;
    partner_ = 0;
    partner_name_.clear();
    trade_.clear();
    version_ = 0;
    mine_ = theirs_ = TradeSide{};
    stock_.clear();
    accepted_me_ = accepted_them_ = false;
    focus_ = Focus::Pack;
    sel_ = 0;
    gold_text_ = "0";
    dirty_ = awaiting_ = false;
    problem_.clear();
}

bool TradeModel::on_frame(const json::Value& m, double now) {
    const std::string t = m.str("t");
    if (t.rfind("trade_", 0) != 0) return false;
    if (t == "trade_invite") {
        if (phase_ == Phase::Open) return true;   // the server won't open a second window anyway
        phase_ = Phase::Invited;
        partner_ = u32(m.num("from"));
        partner_name_ = m.str("name");
        invite_until_ = now + m.num("expires_ms", 30000) / 1000.0;
        note(partner_name_ + " wants to trade with you. Y to accept, N to decline.");
    } else if (t == "trade_asked") {
        if (phase_ == Phase::Open) return true;
        phase_ = Phase::Asking;
        partner_ = u32(m.num("to"));
        partner_name_ = m.str("name");
        note("You asked " + partner_name_ + " to trade.");
    } else if (t == "trade_declined") {
        if (phase_ == Phase::Asking) {
            note(m.str("name") + " does not want to trade.");
            reset();
        }
    } else if (t == "trade_invite_gone") {
        if (phase_ == Phase::Invited && u32(m.num("from")) == partner_) reset();
    } else if (t == "trade_state") {
        const std::string id = m.str("trade");
        if (phase_ != Phase::Open || id != trade_) {
            reset();
            phase_ = Phase::Open;
            trade_ = id;
            note("Trading with " + m.str("name") + ".");
        }
        partner_ = u32(m.num("with"));
        partner_name_ = m.str("name");
        version_ = u32(m.num("version"));
        settle_until_ = now + m.num("settle_ms") / 1000.0;
        theirs_ = parse_side(m["theirs"]);
        // While the player is still editing, their own side stays as they have it.
        if (!dirty_) {
            mine_ = parse_side(m["mine"]);
            gold_text_ = std::to_string(mine_.gold);
        }
        awaiting_ = false;
        accepted_me_ = m["accepted"].boolean("me");
        accepted_them_ = m["accepted"].boolean("them");
        purse_ = u32(m.num("gold"));
        if (m["stock"].type == json::Value::Type::Array) {
            stock_.clear();
            for (const auto& it : m["stock"].arr) stock_.push_back({it.str("item"), it.str("def"), u32(it.num("qty", 1))});
        }
        clamp_selection();
    } else if (t == "trade_done") {
        note("Trade with " + partner_name_ + " done. You now carry " + std::to_string(u32(m.num("gold"))) + " gold.");
        reset();
    } else if (t == "trade_closed") {
        if (phase_ == Phase::Idle) return true;
        const std::string r = m.str("reason");
        std::string who = partner_name_.empty() ? "The other hero" : partner_name_;
        if (r == "cancelled") note(who + " cancelled the trade.");
        else if (r == "left") note(who + " left, so the trade is off.");
        else if (r == "too_far") note("You moved too far apart, so the trade is off.");
        else if (r == "expired") note("The trade timed out.");
        else note("The trade was stopped: one of you can't trade right now.", kColorError);
        reset();
    } else if (t == "trade_error") {
        const std::string msg = m.str("message", m.str("error"));
        if (phase_ == Phase::Open) {
            problem_ = msg;
        } else {
            note("Trade: " + msg, kColorError);
            if (phase_ == Phase::Asking) reset();
        }
    }
    return true;
}

void TradeModel::on_disconnected(double) {
    if (phase_ == Phase::Idle) return;
    note("Trade closed: the connection dropped.", kColorError);
    reset();
}

void TradeModel::command(const std::string& arg, const std::vector<TradeTarget>& nearby, double) {
    if (phase_ == Phase::Open) { note("You are already trading with " + partner_name_ + ".", kColorError); return; }
    const std::string want = lower(trim(arg));
    const TradeTarget* pick = nullptr;
    if (want.empty()) {
        for (const auto& t : nearby)
            if (t.distance <= kReach && (!pick || t.distance < pick->distance)) pick = &t;
        if (!pick) { note("Nobody is close enough to trade with. Stand next to them, or type /trade Name.", kColorError); return; }
    } else {
        int prefix = 0;
        const TradeTarget* byPrefix = nullptr;
        for (const auto& t : nearby) {
            const std::string n = lower(t.name);
            if (n == want) { pick = &t; break; }
            if (n.rfind(want, 0) == 0) { ++prefix; byPrefix = &t; }
        }
        if (!pick && prefix == 1) pick = byPrefix;
        if (!pick) { note("No hero called " + trim(arg) + " is nearby.", kColorError); return; }
    }
    outbox_.push_back("{\"t\":\"trade_request\",\"to\":" + std::to_string(pick->id) + "}");
}

std::vector<TradeItem> TradeModel::pack() const {
    std::vector<TradeItem> out;
    for (const auto& s : stock_) {
        uint32_t offered = 0;
        for (const auto& o : mine_.items)
            if (o.item == s.item) offered = o.qty;
        if (offered < s.qty) out.push_back({s.item, s.def, s.qty - offered});
    }
    return out;
}

void TradeModel::clamp_selection() {
    size_t n = focus_ == Focus::Pack ? pack().size() : focus_ == Focus::Offer ? mine_.items.size() : 1;
    if (n == 0) sel_ = 0;
    else if (sel_ >= n) sel_ = n - 1;
}

double TradeModel::settle_left(double now) const { return std::max(0.0, settle_until_ - now); }

bool TradeModel::can_accept(double now) const {
    return phase_ == Phase::Open && !dirty_ && !awaiting_ && now >= settle_until_;
}

void TradeModel::edited(double now) {
    dirty_ = true;
    send_at_ = now + kSendDelay;
    accepted_me_ = accepted_them_ = false;   // the server clears both ticks on any change
    problem_.clear();
}

void TradeModel::send_offer() {
    std::string s = "{\"t\":\"trade_offer\",\"gold\":" + std::to_string(mine_.gold) + ",\"items\":[";
    for (size_t i = 0; i < mine_.items.size(); ++i) {
        const auto& o = mine_.items[i];
        uint32_t full = 0;
        for (const auto& st : stock_)
            if (st.item == o.item) full = st.qty;
        if (i) s += ',';
        s += "{\"id\":" + json::quote(o.item);
        if (o.qty < full) s += ",\"qty\":" + std::to_string(o.qty);
        s += '}';
    }
    s += "]}";
    outbox_.push_back(std::move(s));
    dirty_ = false;
    awaiting_ = true;
}

void TradeModel::step(double now) {
    if (phase_ == Phase::Invited && now >= invite_until_) {
        note("The trade invitation from " + partner_name_ + " ran out.");
        reset();
    }
    if (phase_ == Phase::Open && dirty_ && now >= send_at_) send_offer();
}

bool TradeModel::key(Key k, double now) {
    if (phase_ == Phase::Invited) {
        if (k == Key::Enter) return character(U'y', now);
        if (k == Key::Escape) return character(U'n', now);
        return true;
    }
    if (phase_ != Phase::Open) return false;
    switch (k) {
    case Key::Escape:
        outbox_.push_back("{\"t\":\"trade_cancel\"}");
        note("You cancelled the trade with " + partner_name_ + ".");
        reset();
        return true;
    case Key::Tab:
    case Key::BackTab: {
        int f = static_cast<int>(focus_) + (k == Key::Tab ? 1 : 2);
        focus_ = static_cast<Focus>(f % 3);
        sel_ = 0;
        clamp_selection();
        return true;
    }
    case Key::Up:
        if (sel_ > 0) --sel_;
        return true;
    case Key::Down:
        ++sel_;
        clamp_selection();
        return true;
    case Key::Enter:
        if (focus_ == Focus::Pack) {
            auto p = pack();
            if (sel_ >= p.size()) return true;
            const TradeItem it = p[sel_];
            bool found = false;
            for (auto& o : mine_.items)
                if (o.item == it.item) { o.qty += it.qty; found = true; }
            if (!found) {
                if (mine_.items.size() >= kMaxItems) { problem_ = "You can offer at most 16 items."; return true; }
                mine_.items.push_back(it);
            }
            edited(now);
            clamp_selection();
        } else if (focus_ == Focus::Offer && sel_ < mine_.items.size()) {
            mine_.items.erase(mine_.items.begin() + static_cast<long>(sel_));
            edited(now);
            clamp_selection();
        } else if (focus_ == Focus::Gold) {
            focus_ = Focus::Pack;
            clamp_selection();
        }
        return true;
    case Key::Left:
    case Key::Right:
    case Key::PageUp:
    case Key::PageDown: {
        if (focus_ != Focus::Offer || sel_ >= mine_.items.size()) return true;
        auto& o = mine_.items[sel_];
        uint32_t full = o.qty;
        for (const auto& st : stock_)
            if (st.item == o.item) full = st.qty;
        int step = (k == Key::Left || k == Key::Right) ? 1 : 10;
        int dir = (k == Key::Right || k == Key::PageDown) ? 1 : -1;
        long q = static_cast<long>(o.qty) + dir * step;
        q = std::max(1L, std::min(static_cast<long>(full), q));
        if (static_cast<uint32_t>(q) != o.qty) {
            o.qty = static_cast<uint32_t>(q);
            edited(now);
        }
        return true;
    }
    case Key::Backspace:
        if (focus_ == Focus::Gold) return character(U'\b', now);
        return true;
    default:
        return true;   // the window has the keyboard; nothing reaches the game or the chat box
    }
}

bool TradeModel::character(char32_t c, double now) {
    if (phase_ == Phase::Invited) {
        if (c == U'y' || c == U'Y') {
            outbox_.push_back("{\"t\":\"trade_answer\",\"from\":" + std::to_string(partner_) + ",\"yes\":true}");
            phase_ = Phase::Asking;   // the window opens when the server's first trade_state comes
        } else if (c == U'n' || c == U'N') {
            outbox_.push_back("{\"t\":\"trade_answer\",\"from\":" + std::to_string(partner_) + ",\"yes\":false}");
            note("You declined " + partner_name_ + "'s trade.");
            reset();
        }
        return true;
    }
    if (phase_ != Phase::Open) return false;
    if (focus_ == Focus::Gold && ((c >= U'0' && c <= U'9') || c == U'\b')) {
        if (c == U'\b') {
            if (!gold_text_.empty()) gold_text_.pop_back();
        } else if (gold_text_.size() < 9) {
            if (gold_text_ == "0") gold_text_.clear();
            gold_text_ += static_cast<char>(c);
        }
        uint32_t g = gold_text_.empty() ? 0 : static_cast<uint32_t>(std::stoul(gold_text_));
        if (g > purse_) {
            g = purse_;
            gold_text_ = std::to_string(g);
        }
        if (g != mine_.gold) {
            mine_.gold = g;
            edited(now);
        }
        return true;
    }
    if (c == U'a' || c == U'A') {
        if (accepted_me_) {
            outbox_.push_back("{\"t\":\"trade_unaccept\"}");
        } else if (can_accept(now)) {
            outbox_.push_back("{\"t\":\"trade_accept\",\"version\":" + std::to_string(version_) + "}");
            problem_.clear();
        } else {
            problem_ = "The offer just changed: look it over, then accept.";
        }
        return true;
    }
    return true;
}

std::vector<std::string> TradeModel::take_outbox() {
    std::vector<std::string> out;
    out.swap(outbox_);
    return out;
}

std::vector<std::pair<std::string, uint32_t>> TradeModel::take_notes() {
    std::vector<std::pair<std::string, uint32_t>> out;
    out.swap(notes_);
    return out;
}

}  // namespace fo::ui
