#pragma once
// The trade window's state and rules, with no Windows or Direct3D in it (tested on Linux).
//
// The server owns the trade (server/src/economy/trade.ts): this only shows what the server
// last said and turns the player's keys into requests. Frames from the game connection come
// in through on_frame(); what to send comes out of take_outbox(). Nothing here moves gold or
// items: a trade is done when the server says `trade_done`, never before.
//
// Keys while the window is open (keyboard, like the chat box):
//   Tab / Shift+Tab   switch between your pack, your offer and the gold box
//   Up / Down         move the selection
//   Enter             pack: put the item in your offer; offer: take it back out
//   Left / Right      offer: one fewer / one more of a stack (PgUp / PgDn: ten)
//   0-9, Backspace    gold box: type the amount of gold you offer
//   A                 accept the offer as shown, or take your accept back
//   Esc               cancel the trade
// An invitation is answered with Y (or Enter) and N (or Esc).
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "chat_model.h"
#include "json.h"

namespace fo::ui {

struct TradeItem {
    std::string item;   // the server's item ID
    std::string def;    // Fable def name, e.g. OBJECT_HEALTH_POTION
    uint32_t qty = 1;
};

struct TradeSide {
    uint32_t gold = 0;
    std::vector<TradeItem> items;
};

/** A hero the player could trade with, from the game connection's entity table. */
struct TradeTarget {
    uint32_t id = 0;
    std::string name;
    float distance = 0;
};

/** "OBJECT_HEALTH_POTION" -> "Health Potion", for when Fable's own name isn't to hand. */
std::string item_label(const std::string& def);

class TradeModel {
public:
    enum class Phase { Idle, Asking, Invited, Open };
    enum class Focus { Pack, Offer, Gold };

    static constexpr float kReach = 10.0f;       // matches TRADE_REACH on the server
    static constexpr double kSendDelay = 0.35;   // offer edits are sent once typing pauses
    static constexpr size_t kMaxItems = 16;

    Phase phase() const { return phase_; }
    bool window_open() const { return phase_ == Phase::Open; }
    /** True while the trade window or an invitation wants the keyboard. */
    bool wants_keys() const { return phase_ == Phase::Open || phase_ == Phase::Invited; }

    // ---- from the game -------------------------------------------------------------
    /** A frame from the game connection. Returns true if it was a trade frame. */
    bool on_frame(const json::Value& m, double now);
    void on_disconnected(double now);

    // ---- from the player -----------------------------------------------------------
    /** "/trade" (nearest hero) or "/trade Name". `nearby` is who is around right now. */
    void command(const std::string& arg, const std::vector<TradeTarget>& nearby, double now);
    bool key(Key k, double now);
    bool character(char32_t c, double now);
    /** Sends a pending offer edit once typing has paused. Call every frame. */
    void step(double now);

    std::vector<std::string> take_outbox();
    /** One-line notes for the chat log ("Hal wants to trade", "Trade done"...). */
    std::vector<std::pair<std::string, uint32_t>> take_notes();

    // ---- for drawing ---------------------------------------------------------------
    const std::string& partner() const { return partner_name_; }
    uint32_t partner_id() const { return partner_; }
    const TradeSide& mine() const { return mine_; }
    const TradeSide& theirs() const { return theirs_; }
    /** Carried items not (wholly) in the offer, with what is left of each stack. */
    std::vector<TradeItem> pack() const;
    uint32_t purse() const { return purse_; }
    bool accepted_me() const { return accepted_me_; }
    bool accepted_them() const { return accepted_them_; }
    Focus focus() const { return focus_; }
    size_t selected() const { return sel_; }
    const std::string& gold_text() const { return gold_text_; }
    /** Seconds until an accept is allowed (the offer just changed), 0 when it is. */
    double settle_left(double now) const;
    bool can_accept(double now) const;
    /** The last thing the server refused, shown in the window until the next change. */
    const std::string& problem() const { return problem_; }
    double invite_left(double now) const { return phase_ == Phase::Invited ? std::max(0.0, invite_until_ - now) : 0; }

    static constexpr uint32_t kColorTrade = ChatModel::kColorTrade;
    static constexpr uint32_t kColorError = ChatModel::kColorError;

private:
    void reset();
    void note(std::string text, uint32_t color = kColorTrade) { notes_.emplace_back(std::move(text), color); }
    void edited(double now);
    void send_offer();
    void clamp_selection();
    static TradeSide parse_side(const json::Value& v);

    Phase phase_ = Phase::Idle;
    uint32_t partner_ = 0;
    std::string partner_name_;
    std::string trade_;
    uint32_t version_ = 0;
    double settle_until_ = 0;
    double invite_until_ = 0;
    TradeSide mine_, theirs_;
    std::vector<TradeItem> stock_;
    uint32_t purse_ = 0;
    bool accepted_me_ = false, accepted_them_ = false;
    Focus focus_ = Focus::Pack;
    size_t sel_ = 0;
    std::string gold_text_ = "0";
    /** The local offer differs from what the server last confirmed; sent at send_at_. */
    bool dirty_ = false;
    double send_at_ = 0;
    /** An offer was sent and the server's new version has not come back yet. */
    bool awaiting_ = false;
    std::string problem_;
    std::vector<std::string> outbox_;
    std::vector<std::pair<std::string, uint32_t>> notes_;
};

}  // namespace fo::ui
