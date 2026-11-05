#pragma once
// The Mod settings screen's logic: pages, rows, focus, values, layout and input. No Windows
// or Direct3D, so it builds and tests anywhere (client-mod/tests). modsettings.cpp binds the
// rows to the mod's settings, draws the screen and feeds it input.
#include <string>
#include <vector>

namespace fo::modsettings {

enum class Key { Up, Down, Left, Right, Enter, Escape, Tab, BackTab, PageUp, PageDown };

struct Row {
    int id = 0;                         // the caller's own id for the setting or action
    std::string label;
    std::string help;                   // one line shown at the bottom while the row has focus
    std::vector<std::string> choices;   // empty: an action button
    int value = 0;                      // index into choices
    bool restart = false;               // the change applies the next time the game starts
};

struct Page {
    std::string title;
    std::vector<Row> rows;
};

struct Box { float x = 0, y = 0, w = 0, h = 0; };

struct Event {
    enum Kind { None, Changed, Action, Closed } kind = None;
    int id = 0;
    int value = 0;
};

// Pixel sizes the renderer's fonts need; the screen is laid out around them.
struct Metrics {
    float line = 16;     // body text line height
    float title = 24;    // title line height
};

class Model {
public:
    void set_pages(std::vector<Page> pages);
    const std::vector<Page>& pages() const { return pages_; }

    bool is_open() const { return open_; }
    void open(int page);
    void close() { open_ = false; }

    void resize(float screen_w, float screen_h, const Metrics& m);
    float screen_w() const { return sw_; }
    float screen_h() const { return sh_; }

    int page() const { return page_; }
    int focus() const { return focus_; }        // row index on the current page, -1 = the Back button
    int first_visible() const { return first_; }
    int visible_rows() const { return visible_; }

    // Layout (screen pixels), valid after resize().
    Box panel() const { return panel_; }
    Box tab(int i) const;                        // the page tabs along the top
    Box row(int i) const;                        // row i of the current page (only when visible)
    Box value_box(int i) const;                  // the value area at the right of row i
    Box back_button() const { return back_; }
    float title_y() const { return title_y_; }
    float help_y() const { return help_y_; }
    int hover() const { return hover_; }         // row under the pointer, -1 none, -2 Back, -3 - n tab n

    // Sets a value from outside (the setting changed elsewhere, or a live state such as spawning).
    void set_value(int id, int value);
    const Row* find(int id) const;

    // Input. Each returns what happened.
    Event key(Key k);
    void pointer(float x, float y);
    Event click(float x, float y);
    void scroll(int rows);

private:
    Event change(int row, int delta);
    Event activate(int row);
    void keep_visible();
    void relayout();
    int hit(float x, float y) const;
    int rows_on_page() const;

    std::vector<Page> pages_;
    bool open_ = false;
    int page_ = 0, focus_ = 0, first_ = 0, visible_ = 1, hover_ = -1;
    float sw_ = 1280, sh_ = 720;
    Metrics m_;
    Box panel_, back_;
    float row_h_ = 30, rows_y_ = 0, tabs_y_ = 0, tab_h_ = 30, title_y_ = 0, help_y_ = 0;
};

}  // namespace fo::modsettings
