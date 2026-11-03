#include "menu_model.h"
#include <algorithm>

namespace fo::menu {
namespace {
std::string upper(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}
}  // namespace

Model::Model() { relayout(); }

void Model::resize(float w, float h) {
    if (w <= 0 || h <= 0 || (w == screen_w_ && h == screen_h_)) return;
    screen_w_ = w;
    screen_h_ = h;
    relayout();
}

void Model::show() {
    visible_ = true;
    screen_ = Screen::Title;
    focus_ = 0;
    relayout();
}

void Model::relayout() {
    scale_ = std::min(screen_w_ / kDesignW, screen_h_ / kDesignH);
    ox_ = (screen_w_ - kDesignW * scale_) / 2;
    oy_ = (screen_h_ - kDesignH * scale_) / 2;
    buttons_.clear();
    auto add = [&](const std::string& label, Action a, float x, float y, float w, float h) {
        Button b;
        b.label = label;
        b.action = a;
        b.x = sx(x);
        b.y = sy(y);
        b.w = w * scale_;
        b.h = h * scale_;
        buttons_.push_back(b);
    };
    if (screen_ == Screen::Title) {
        const char* labels[] = {"ENTER ALBION", "HEROES", "REALM", "OPTIONS", "LEAVE ALBION"};
        Action acts[] = {Action::EnterAlbion, Action::Heroes, Action::Realm, Action::Options, Action::Quit};
        for (int i = 0; i < 5; ++i) add(labels[i], acts[i], 640 - 160, 268 + i * 82.0f, 320, 80);
    } else {
        add(upper(hero_name_), Action::SelectHero, 56, 112, 320, 80);
        add("NEW HERO", Action::NewHero, 56, 194, 320, 80);
        add("BACK", Action::Back, 56, 720 - 44 - 64, 256, 64);
        add("ENTER ALBION", Action::EnterAlbion, 56 + 264, 720 - 44 - 64, 256, 64);
    }
    if (focus_ >= static_cast<int>(buttons_.size())) focus_ = 0;
    for (size_t i = 0; i < buttons_.size(); ++i) buttons_[i].lit = static_cast<int>(i) == focus_ || static_cast<int>(i) == hover_;
}

Rect Model::details_panel() const { return {sx(56 + 360), sy(112), 300 * scale_, 220 * scale_}; }

int Model::hit(float x, float y) const {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button& b = buttons_[i];
        // The art has ornaments at both ends; only the middle 80% and inner 70% height are the bar.
        if (x >= b.x + b.w * 0.1f && x <= b.x + b.w * 0.9f && y >= b.y + b.h * 0.15f && y <= b.y + b.h * 0.85f)
            return static_cast<int>(i);
    }
    return -1;
}

void Model::pointer(float x, float y) {
    if (!visible_) return;
    int h = hit(x, y);
    if (h != hover_) {
        hover_ = h;
        if (h >= 0) focus_ = h;   // mouse and keyboard share one highlight, like Fable's menus
        relayout();
    }
}

Action Model::click(float x, float y) {
    if (!visible_) return Action::None;
    int h = hit(x, y);
    return h >= 0 ? activate(h) : Action::None;
}

Action Model::key(Key k) {
    if (!visible_) return Action::None;
    int n = static_cast<int>(buttons_.size());
    switch (k) {
    case Key::Up: case Key::Left: focus_ = (focus_ + n - 1) % n; hover_ = -1; relayout(); return Action::None;
    case Key::Down: case Key::Right: focus_ = (focus_ + 1) % n; hover_ = -1; relayout(); return Action::None;
    case Key::Enter: return activate(focus_);
    case Key::Escape:
        if (screen_ == Screen::Heroes) return activate(2);  // Back
        return Action::None;
    }
    return Action::None;
}

Action Model::activate(int i) {
    if (i < 0 || i >= static_cast<int>(buttons_.size())) return Action::None;
    Action a = buttons_[i].action;
    switch (a) {
    case Action::Heroes:
        screen_ = Screen::Heroes;
        focus_ = 0;
        hover_ = -1;
        relayout();
        break;
    case Action::Back:
        screen_ = Screen::Title;
        focus_ = 1;
        hover_ = -1;
        relayout();
        break;
    case Action::Realm:
        set_status(realm_line_.empty() ? "Realms are chosen in the launcher." : realm_line_ + ". Realms are chosen in the launcher.");
        break;
    case Action::NewHero:
        set_status("New heroes arrive with the game server.");
        break;
    case Action::SelectHero:
        focus_ = 3;  // jump to Enter Albion
        relayout();
        break;
    case Action::EnterAlbion:
    case Action::Options:
        visible_ = false;
        break;
    default:
        break;
    }
    return a;
}

}  // namespace fo::menu
