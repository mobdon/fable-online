#pragma once
// Start-up menu logic: screens, buttons, layout, focus and actions. No Windows or Direct3D,
// so it builds and tests anywhere (client-mod/tests). menu.cpp draws it and feeds it input.
#include <string>
#include <vector>

namespace fo::menu {

enum class Screen { Title, Heroes };
enum class Action { None, EnterAlbion, Heroes, Realm, Options, ModSettings, Quit, Back, SelectHero, NewHero };
enum class Key { Up, Down, Left, Right, Enter, Escape };

struct Button {
    std::string label;
    Action action = Action::None;
    float x = 0, y = 0, w = 0, h = 0;   // screen pixels
    bool lit = false;                    // drawn with the "on" art (focus or hover)
};

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// The design is 1280x720; everything scales with screen height and centres horizontally.
constexpr float kDesignW = 1280.0f, kDesignH = 720.0f;

class Model {
public:
    Model();
    void resize(float screen_w, float screen_h);
    float scale() const { return scale_; }
    float screen_w() const { return screen_w_; }
    float screen_h() const { return screen_h_; }
    float sx(float design_x) const { return ox_ + design_x * scale_; }   // design -> screen
    float sy(float design_y) const { return oy_ + design_y * scale_; }

    bool visible() const { return visible_; }
    void show();
    void hide() { visible_ = false; }
    Screen screen() const { return screen_; }
    const std::vector<Button>& buttons() const { return buttons_; }
    int focus() const { return focus_; }

    // Input. Each returns the action that fired (None if nothing did). The model handles
    // screen changes itself; the caller handles EnterAlbion, Options, ModSettings and Quit.
    void pointer(float x, float y);
    Action click(float x, float y);
    Action key(Key k);

    // Shown on screen.
    void set_hero_name(const std::string& n) { hero_name_ = n; relayout(); }
    const std::string& hero_name() const { return hero_name_; }
    void set_realm_line(const std::string& s) { realm_line_ = s; }
    const std::string& realm_line() const { return realm_line_; }
    const std::string& status() const { return status_; }
    unsigned status_serial() const { return status_serial_; }   // changes each time a new status is set

    // Panels to draw on the Heroes screen (design coordinates converted to screen).
    Rect details_panel() const;

private:
    Action activate(int i);
    void relayout();
    void set_status(const std::string& s) { status_ = s; ++status_serial_; }
    int hit(float x, float y) const;

    float screen_w_ = kDesignW, screen_h_ = kDesignH, scale_ = 1, ox_ = 0, oy_ = 0;
    bool visible_ = true;
    Screen screen_ = Screen::Title;
    std::vector<Button> buttons_;
    int focus_ = 0, hover_ = -1;
    std::string hero_name_ = "Hero", realm_line_, status_;
    unsigned status_serial_ = 0;
};

}  // namespace fo::menu
