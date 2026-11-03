#pragma once
// The chat box's state and rules, with no Windows or Direct3D in it (tested on Linux).
// The overlay feeds it keys and server messages and draws what visible() returns;
// whatever it wants to send to the chat server comes out of take_outbox().
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "json.h"

namespace fo::ui {

enum class Key { Enter, Escape, Backspace, Delete, Left, Right, Home, End, Up, Down, PageUp, PageDown, Tab, BackTab };

// Chat tabs. Each line belongs to All plus at most one other tab; system lines show everywhere.
enum class ChatTab : uint8_t { All = 0, Region, World, Guild, Whispers, Count };
const char* tab_name(ChatTab t);

struct ChatLine {
    std::string text;   // UTF-8
    uint32_t color;     // 0xAARRGGBB
    double at;          // seconds, from the clock passed in
    uint8_t tabs;       // bit per ChatTab
};

struct VisibleLine {
    const ChatLine* line;
    float alpha;        // 0..1, lines fade out when the box is closed
};

class ChatModel {
public:
    static constexpr size_t kMaxLog = 200;
    static constexpr size_t kMaxInput = 200;   // matches the server's limit
    static constexpr double kFadeAfter = 12.0; // seconds a line stays up with the box closed
    static constexpr double kFadeTime = 3.0;

    bool is_open() const { return open_; }
    void open(char32_t first = 0);             // Enter opens; '/' opens with a slash typed
    void close();

    // Return true when the key was used (the game must not see it).
    bool key(Key k, double now);
    bool character(char32_t c, double now);

    // A server line that isn't a name tag message ("msg", "sys", "err", "welcome", ...).
    void on_server(const json::Value& m, double now);
    void on_connection(bool up, const std::string& why, double now);
    void system(const std::string& text, double now, uint32_t color = kColorSystem);

    std::vector<std::string> take_outbox();    // JSON lines, without the trailing newline
    std::vector<VisibleLine> visible(size_t rows, double now) const;

    std::string input_utf8() const;
    size_t cursor() const { return cursor_; }  // in characters
    std::string channel_label() const;         // "Say", "World", "To Hal", ...

    // Tab / Shift+Tab while typing switch tabs. Picking Region, World, Guild or Whispers also
    // makes that the channel you type into.
    ChatTab tab() const { return tab_; }
    void set_tab(ChatTab t);
    bool unread(ChatTab t) const { return (unread_ >> static_cast<int>(t)) & 1; }

    bool tags_enabled() const { return tags_enabled_; }
    const std::string& me() const { return me_; }

    // Fable's HUD palette: parchment text, pewter, the chrome blue of its logo.
    static constexpr uint32_t kColorSay = 0xFFE9E4D6;
    static constexpr uint32_t kColorRegion = 0xFFE8B46A;
    static constexpr uint32_t kColorWorld = 0xFFF4E3A8;
    static constexpr uint32_t kColorTrade = 0xFFD2A878;
    static constexpr uint32_t kColorParty = 0xFF9AA3F4;
    static constexpr uint32_t kColorGuild = 0xFFA8CF8E;
    static constexpr uint32_t kColorWhisper = 0xFFD9A6E0;
    static constexpr uint32_t kColorSystem = 0xFFFFF6DC;
    static constexpr uint32_t kColorError = 0xFFE0705C;

private:
    void submit(double now);
    void push(std::string text, uint32_t color, double now, uint8_t tabs = 0xFF);
    void send_say(const std::string& ch, const std::string& text);
    void send_whisper(const std::string& to, const std::string& text);

    bool open_ = false;
    std::u32string input_;
    size_t cursor_ = 0;
    std::string channel_ = "say";              // sticky: the last channel you typed in
    std::string whisper_to_;                    // set when the sticky channel is a whisper
    std::string reply_to_;                      // last person who whispered you
    std::string me_;
    std::deque<ChatLine> log_;
    size_t scroll_ = 0;                         // lines scrolled up from the bottom
    std::vector<std::u32string> history_;
    size_t history_pos_ = 0;
    std::vector<std::string> outbox_;
    bool tags_enabled_ = true;
    ChatTab tab_ = ChatTab::All;
    uint8_t unread_ = 0;
};

// UTF-8 <-> UTF-32 helpers (also used by the renderer).
std::u32string utf8_to_u32(const std::string& s);
std::string u32_to_utf8(const std::u32string& s);

}  // namespace fo::ui
