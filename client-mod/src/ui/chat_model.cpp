#include "chat_model.h"
#include <algorithm>
#include <cctype>

namespace fo::ui {

std::u32string utf8_to_u32(const std::string& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { o += U'?'; ++i; continue; }
        bool bad = false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (bad) { o += U'?'; ++i; continue; }
        o += cp;
        i += extra + 1;
    }
    return o;
}

std::string u32_to_utf8(const std::u32string& s) {
    std::string o;
    for (char32_t cp : s) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    return o;
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Splits off the first word, or a "quoted name" (hero names can have spaces).
std::string take_word(std::string& rest) {
    size_t i = rest.find_first_not_of(' ');
    if (i == std::string::npos) { rest.clear(); return ""; }
    std::string w;
    if (rest[i] == '"') {
        size_t end = rest.find('"', i + 1);
        if (end == std::string::npos) { w = rest.substr(i + 1); rest.clear(); return w; }
        w = rest.substr(i + 1, end - i - 1);
        rest = rest.substr(end + 1);
    } else {
        size_t end = rest.find(' ', i);
        w = rest.substr(i, end == std::string::npos ? std::string::npos : end - i);
        rest = end == std::string::npos ? "" : rest.substr(end + 1);
    }
    return w;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

uint32_t channel_color(const std::string& ch) {
    if (ch == "region") return ChatModel::kColorRegion;
    if (ch == "realm") return ChatModel::kColorRealm;
    if (ch == "trade") return ChatModel::kColorTrade;
    if (ch == "party") return ChatModel::kColorParty;
    if (ch == "guild") return ChatModel::kColorGuild;
    if (ch == "whisper") return ChatModel::kColorWhisper;
    if (ch == "system") return ChatModel::kColorSystem;
    return ChatModel::kColorSay;
}

uint8_t bit(ChatTab t) { return static_cast<uint8_t>(1u << static_cast<int>(t)); }

uint8_t tabs_for(const std::string& ch) {
    uint8_t all = bit(ChatTab::All);
    if (ch == "say" || ch == "region") return all | bit(ChatTab::Region);
    if (ch == "realm" || ch == "trade") return all | bit(ChatTab::Realm);
    if (ch == "guild") return all | bit(ChatTab::Guild);
    if (ch == "whisper") return all | bit(ChatTab::Whispers);
    return all;  // party
}

const char* kHelp[] = {
    "Enter: chat.  Esc: close.  Tab: next tab.  Up/Down: history.  PgUp/PgDn: scroll.",
    "/s say (nearby)  /re region  /1 realm  /2 trade  /p party  /g guild",
    "/w Name text: whisper (\"quotes\" for names with spaces)  /r text: reply",
    "/ignore Name  /unignore Name  /report Name reason  /tags: name tags on/off  /clear",
    "/barter: trade items and gold with the hero next to you  /barter Name: with that hero",
    "Party: /invite Name  /accept [Name]  /decline [Name]  /leave  /regroup",
    "       /party kick Name  /party lead Name  (leader only)",
};
}  // namespace

const char* tab_name(ChatTab t) {
    switch (t) {
    case ChatTab::All: return "All";
    case ChatTab::Region: return "Region";
    case ChatTab::Realm: return "Realm";
    case ChatTab::Guild: return "Guild";
    case ChatTab::Whispers: return "Whispers";
    default: return "";
    }
}

void ChatModel::set_tab(ChatTab t) {
    tab_ = t;
    scroll_ = 0;
    unread_ &= static_cast<uint8_t>(~bit(t));
    switch (t) {
    case ChatTab::Region: channel_ = "region"; whisper_to_.clear(); break;
    case ChatTab::Realm: channel_ = "realm"; whisper_to_.clear(); break;
    case ChatTab::Guild: channel_ = "guild"; whisper_to_.clear(); break;
    case ChatTab::Whispers:
        if (!reply_to_.empty()) { channel_ = "whisper"; whisper_to_ = reply_to_; }
        break;
    default: break;
    }
}

void ChatModel::whisper_to(const std::string& name) {
    if (name.empty() || name == me_) return;
    channel_ = "whisper";
    whisper_to_ = name;
}

void ChatModel::scroll_by(int lines) {
    if (lines > 0) scroll_ = std::min(scroll_ + static_cast<size_t>(lines), log_.size() > 1 ? log_.size() - 1 : 0);
    else scroll_ = scroll_ > static_cast<size_t>(-lines) ? scroll_ - static_cast<size_t>(-lines) : 0;
}

void ChatModel::set_cursor(size_t at) { cursor_ = std::min(at, input_.size()); }

void ChatModel::open(char32_t first) {
    open_ = true;
    input_.clear();
    cursor_ = 0;
    scroll_ = 0;
    history_pos_ = history_.size();
    if (first) { input_ += first; cursor_ = 1; }
}

void ChatModel::close() {
    open_ = false;
    input_.clear();
    cursor_ = 0;
    scroll_ = 0;
}

bool ChatModel::key(Key k, double now) {
    if (!open_) {
        if (k == Key::Enter) { open(); return true; }
        return false;
    }
    switch (k) {
    case Key::Enter: submit(now); close(); break;
    case Key::Escape: close(); break;
    case Key::Backspace: if (cursor_ > 0) input_.erase(--cursor_, 1); break;
    case Key::Delete: if (cursor_ < input_.size()) input_.erase(cursor_, 1); break;
    case Key::Left: if (cursor_ > 0) --cursor_; break;
    case Key::Right: if (cursor_ < input_.size()) ++cursor_; break;
    case Key::Home: cursor_ = 0; break;
    case Key::End: cursor_ = input_.size(); break;
    case Key::Up:
        if (history_pos_ > 0) { input_ = history_[--history_pos_]; cursor_ = input_.size(); }
        break;
    case Key::Down:
        if (history_pos_ < history_.size()) {
            ++history_pos_;
            input_ = history_pos_ < history_.size() ? history_[history_pos_] : U"";
            cursor_ = input_.size();
        }
        break;
    case Key::PageUp: scroll_ = std::min(scroll_ + 5, log_.size() > 1 ? log_.size() - 1 : 0); break;
    case Key::PageDown: scroll_ = scroll_ > 5 ? scroll_ - 5 : 0; break;
    case Key::Tab:
    case Key::BackTab: {
        int n = static_cast<int>(ChatTab::Count);
        int next = (static_cast<int>(tab_) + (k == Key::Tab ? 1 : n - 1)) % n;
        set_tab(static_cast<ChatTab>(next));
        break;
    }
    }
    return true;  // everything is ours while the box is open
}

bool ChatModel::character(char32_t c, double) {
    if (!open_) return false;
    if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c < 0xE000)) return true;
    if (input_.size() >= kMaxInput) return true;
    input_.insert(cursor_++, 1, c);
    return true;
}

void ChatModel::send_say(const std::string& ch, const std::string& text) {
    outbox_.push_back("{\"t\":\"say\",\"ch\":" + json::quote(ch) + ",\"text\":" + json::quote(text) + "}");
}

void ChatModel::send_whisper(const std::string& to, const std::string& text) {
    outbox_.push_back("{\"t\":\"whisper\",\"to\":" + json::quote(to) + ",\"text\":" + json::quote(text) + "}");
}

void ChatModel::send_party(const char* op, const std::string& name, const std::string& text) {
    std::string m = "{\"t\":\"party\",\"op\":" + json::quote(op);
    if (!name.empty()) m += ",\"name\":" + json::quote(name);
    if (!text.empty()) m += ",\"text\":" + json::quote(text);
    world_outbox_.push_back(m + "}");
}

// Party requests, typed as "/party <what>" or the short forms (/invite, /accept, ...).
// Returns false when `cmd` is not one of them. Without "/party" in front, "kick" and "lead"
// are not party requests: /kick is the moderators' command.
bool ChatModel::party_command(const std::string& cmd, std::string rest, double now, bool after_party) {
    if (!after_party && (cmd == "kick" || cmd == "lead")) return false;
    static const char* const kNeedsName[] = {"invite", "kick", "lead"};
    std::string op = cmd;
    if (op == "inv") op = "invite";
    else if (op == "promote") op = "lead";
    else if (op == "pkick") op = "kick";
    else if (op == "plead") op = "lead";
    else if (op == "pleave") op = "leave";
    bool known = op == "invite" || op == "accept" || op == "decline" || op == "leave" || op == "regroup" ||
                 op == "kick" || op == "lead";
    if (!known) return false;
    std::string name = take_word(rest);
    for (const char* n : kNeedsName)
        if (op == n && name.empty()) { system("Usage: /party " + op + " Name", now, kColorError); return true; }
    send_party(op.c_str(), name);
    return true;
}

void ChatModel::party_line(const std::string& from, const std::string& text, double now) {
    push("[Party] " + from + ": " + text, kColorParty, now, tabs_for("party"));
}

void ChatModel::party_note(const std::string& text, double now, bool error) {
    push(text, error ? kColorError : kColorParty, now);
}

void ChatModel::remember_contact(const std::string& name) {
    if (name.empty()) return;
    for (auto it = contacts_.begin(); it != contacts_.end(); ++it)
        if (*it == name) { contacts_.erase(it); break; }
    contacts_.push_front(name);
    if (contacts_.size() > kMaxContacts) contacts_.pop_back();
}

void ChatModel::submit(double now) {
    std::string line = trim(u32_to_utf8(input_));
    if (line.empty()) return;
    if (history_.empty() || history_.back() != input_) history_.push_back(input_);
    if (history_.size() > 50) history_.erase(history_.begin());

    if (line[0] != '/') {
        if (channel_ == "whisper") send_whisper(whisper_to_, line);
        else if (channel_ == "party") send_party("say", "", line);
        else send_say(channel_, line);
        return;
    }
    std::string rest = line.substr(1);
    std::string cmd = lower(take_word(rest));
    rest = trim(rest);
    // "/party invite Name" and friends; "/party" alone or "/party some words" is party chat.
    if (cmd == "party") {
        std::string sub = rest;
        std::string first = lower(take_word(sub));
        if (party_command(first, sub, now, true)) return;
    }
    if (cmd != "p" && cmd != "party" && party_command(cmd, rest, now, false)) return;
    struct Alias { const char* name; const char* ch; };
    static const Alias talk[] = {
        {"s", "say"}, {"say", "say"}, {"re", "region"}, {"region", "region"}, {"y", "region"}, {"sh", "region"}, {"shout", "region"},
        {"1", "realm"}, {"realm", "realm"}, {"world", "realm"}, {"2", "trade"}, {"trade", "trade"},
        {"p", "party"}, {"party", "party"}, {"g", "guild"}, {"guild", "guild"},
    };
    for (const auto& a : talk) {
        if (cmd != a.name) continue;
        channel_ = a.ch;  // typing "/1" alone switches the box to Realm chat
        whisper_to_.clear();
        if (!rest.empty()) {
            if (channel_ == "party") send_party("say", "", rest);
            else send_say(channel_, rest);
        }
        return;
    }
    if (cmd == "w" || cmd == "whisper" || cmd == "t" || cmd == "tell" || cmd == "r" || cmd == "reply") {
        std::string to = (cmd == "r" || cmd == "reply") ? reply_to_ : take_word(rest);
        rest = trim(rest);
        if (to.empty()) { system(cmd[0] == 'r' ? "Nobody to reply to yet." : "Usage: /w Name message", now, kColorError); return; }
        channel_ = "whisper";
        whisper_to_ = to;
        if (!rest.empty()) send_whisper(to, rest);
        return;
    }
    if (cmd == "ignore" || cmd == "unignore") {
        std::string who = take_word(rest);
        if (who.empty()) { system("Usage: /" + cmd + " Name", now, kColorError); return; }
        outbox_.push_back("{\"t\":\"ignore\",\"name\":" + json::quote(who) + ",\"on\":" + (cmd == "ignore" ? "true" : "false") + "}");
        return;
    }
    if (cmd == "report") {
        std::string who = take_word(rest);
        if (who.empty()) { system("Usage: /report Name reason", now, kColorError); return; }
        outbox_.push_back("{\"t\":\"report\",\"name\":" + json::quote(who) + ",\"reason\":" + json::quote(trim(rest)) + "}");
        return;
    }
    if (cmd == "mute" || cmd == "unmute" || cmd == "kick" || cmd == "ban") {
        std::string who = take_word(rest);
        if (who.empty()) { system("Usage: /" + cmd + " Name [minutes] [reason]", now, kColorError); return; }
        std::string msg = "{\"t\":\"mod\",\"action\":" + json::quote(cmd) + ",\"name\":" + json::quote(who);
        std::string r = trim(rest);
        size_t sp = r.find(' ');
        std::string first = r.substr(0, sp);
        if (!first.empty() && std::all_of(first.begin(), first.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            msg += ",\"minutes\":" + first.substr(0, 6);
            r = sp == std::string::npos ? "" : trim(r.substr(sp + 1));
        }
        if (!r.empty()) msg += ",\"reason\":" + json::quote(r);
        outbox_.push_back(msg + "}");
        return;
    }
    if (cmd == "settings" || cmd == "mod" || cmd == "modsettings" || cmd == "options") {
        settings_requested_ = true;   // the overlay opens the Mod settings screen
        return;
    }
    if (cmd == "tags") {
        tags_enabled_ = !tags_enabled_;
        system(tags_enabled_ ? "Name tags on." : "Name tags off.", now);
        return;
    }
    if (cmd == "barter") { local_.push_back({cmd, rest}); return; }   // player trading: the overlay's trade window
    if (cmd == "clear") { log_.clear(); scroll_ = 0; return; }
    if (cmd == "help" || cmd == "?") { for (const char* h : kHelp) system(h, now); return; }
    system("Unknown command /" + cmd + ". Type /help.", now, kColorError);
}

void ChatModel::push(std::string text, uint32_t color, double now, uint8_t tabs) {
    // A line for a tab you aren't looking at lights that tab up (All never does).
    uint8_t others = static_cast<uint8_t>(tabs & ~bit(ChatTab::All) & ~bit(tab_));
    if (tabs != 0xFF) unread_ |= others;
    log_.push_back({std::move(text), color, now, tabs});
    while (log_.size() > kMaxLog) log_.pop_front();
    if (scroll_ > 0 && scroll_ + 1 < log_.size()) ++scroll_;  // keep the view still while reading back
}

void ChatModel::system(const std::string& text, double now, uint32_t color) { push(text, color, now); }

void ChatModel::on_connection(bool up, const std::string& why, double now) {
    if (up) system("Connected to chat.", now);
    else system("Chat disconnected" + (why.empty() ? std::string(".") : ": " + why) + " Reconnecting...", now, kColorError);
}

void ChatModel::on_server(const json::Value& m, double now) {
    std::string t = m.str("t");
    if (t == "welcome") {
        me_ = m.str("name");
        std::string motd = m.str("motd");
        if (!motd.empty()) system(motd, now);
    } else if (t == "msg") {
        std::string ch = m.str("ch"), from = m.str("from"), text = m.str("text");
        std::string line;
        if (ch == "whisper") {
            std::string who = from == me_ ? m.str("to") : from;
            if (from == me_) line = "To " + who + ": " + text;
            else { line = from + " whispers: " + text; reply_to_ = from; }
            remember_contact(who);
        } else if (ch == "say") {
            line = from + ": " + text;
        } else {
            std::string label = ch;
            if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            line = "[" + label + "] " + from + ": " + text;
        }
        push(line, channel_color(ch), now, tabs_for(ch));
    } else if (t == "sys") {
        system(m.str("text"), now, m.str("level") == "warn" ? kColorError : kColorSystem);
    } else if (t == "err") {
        system(m.str("text"), now, kColorError);
    }
}

std::vector<std::pair<std::string, std::string>> ChatModel::take_local() {
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(local_);
    return out;
}

std::vector<std::string> ChatModel::take_outbox() {
    std::vector<std::string> o;
    o.swap(outbox_);
    return o;
}

std::vector<std::string> ChatModel::take_world_outbox() {
    std::vector<std::string> o;
    o.swap(world_outbox_);
    return o;
}

std::vector<VisibleLine> ChatModel::visible(size_t rows, double now) const {
    std::vector<VisibleLine> out;
    if (log_.empty() || rows == 0) return out;
    const uint8_t want = bit(tab_);
    std::vector<const ChatLine*> lines;
    for (const auto& l : log_) if (l.tabs & want) lines.push_back(&l);
    if (lines.empty()) return out;
    size_t end = lines.size() - std::min(scroll_, lines.size() - 1);
    size_t begin = end > rows ? end - rows : 0;
    for (size_t i = begin; i < end; ++i) {
        float a = 1.0f;
        if (!open_) {
            double age = now - lines[i]->at;
            if (age > kFadeAfter + kFadeTime) continue;
            if (age > kFadeAfter) a = static_cast<float>(1.0 - (age - kFadeAfter) / kFadeTime);
        }
        out.push_back({lines[i], a});
    }
    return out;
}

std::string ChatModel::input_utf8() const { return u32_to_utf8(input_); }

std::string ChatModel::channel_label() const {
    if (channel_ == "whisper") return "To " + whisper_to_;
    std::string l = channel_;
    if (!l.empty()) l[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(l[0])));
    return l;
}

}  // namespace fo::ui
