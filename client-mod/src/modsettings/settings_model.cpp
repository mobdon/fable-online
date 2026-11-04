#include "settings_model.h"
#include <algorithm>
#include <cmath>

namespace fo::modsettings {

void Model::set_pages(std::vector<Page> pages) {
    pages_ = std::move(pages);
    if (page_ >= static_cast<int>(pages_.size())) page_ = 0;
    if (focus_ >= rows_on_page()) focus_ = rows_on_page() > 0 ? 0 : -1;
    relayout();
}

void Model::open(int page) {
    open_ = true;
    page_ = page >= 0 && page < static_cast<int>(pages_.size()) ? page : 0;
    focus_ = rows_on_page() > 0 ? 0 : -1;
    first_ = 0;
    hover_ = -1;
    relayout();
}

int Model::rows_on_page() const {
    return pages_.empty() ? 0 : static_cast<int>(pages_[page_].rows.size());
}

void Model::resize(float screen_w, float screen_h, const Metrics& m) {
    if (screen_w == sw_ && screen_h == sh_ && m.line == m_.line && m.title == m_.title) return;
    sw_ = screen_w;
    sh_ = screen_h;
    m_ = m;
    relayout();
}

void Model::relayout() {
    const float line = m_.line > 0 ? m_.line : 16;
    const float pad = std::round(line * 1.4f);
    float w = std::max(640.0f, line * 46);
    w = std::min(w, sw_ - 32);
    float h = std::min(sh_ - 32, std::max(480.0f, line * 34));
    panel_ = {std::round((sw_ - w) / 2), std::round((sh_ - h) / 2), std::round(w), std::round(h)};
    row_h_ = std::round(line * 1.9f);
    tab_h_ = std::round(line * 1.9f);
    title_y_ = panel_.y + pad;
    tabs_y_ = title_y_ + m_.title + std::round(line * 0.7f);
    rows_y_ = tabs_y_ + (pages_.size() > 1 ? tab_h_ + std::round(line * 0.9f) : 0);
    const float back_h = std::round(line * 1.9f), back_w = std::round(std::max(160.0f, line * 11));
    back_ = {std::round(panel_.x + (panel_.w - back_w) / 2), panel_.y + panel_.h - pad - back_h, back_w, back_h};
    help_y_ = back_.y - std::round(line * 1.8f);
    visible_ = std::max(1, static_cast<int>((help_y_ - std::round(line * 0.5f) - rows_y_) / row_h_));
    keep_visible();
}

Box Model::tab(int i) const {
    const int n = static_cast<int>(pages_.size());
    const float pad = std::round((m_.line > 0 ? m_.line : 16) * 1.4f), gap = 8;
    const float w = n > 0 ? (panel_.w - 2 * pad - gap * (n - 1)) / n : 0;
    return {std::round(panel_.x + pad + i * (w + gap)), tabs_y_, std::round(w), tab_h_};
}

Box Model::row(int i) const {
    const float pad = std::round((m_.line > 0 ? m_.line : 16) * 1.4f);
    return {panel_.x + pad, rows_y_ + (i - first_) * row_h_, panel_.w - 2 * pad, row_h_ - 2};
}

Box Model::value_box(int i) const {
    Box r = row(i);
    const float w = std::round(r.w * 0.38f);
    return {r.x + r.w - w, r.y, w, r.h};
}

void Model::keep_visible() {
    const int n = rows_on_page();
    if (focus_ >= 0) {
        if (focus_ < first_) first_ = focus_;
        if (focus_ >= first_ + visible_) first_ = focus_ - visible_ + 1;
    }
    first_ = std::max(0, std::min(first_, n - visible_));
}

void Model::scroll(int rows) {
    first_ += rows;
    const int n = rows_on_page();
    first_ = std::max(0, std::min(first_, n - visible_));
}

const Row* Model::find(int id) const {
    for (const auto& p : pages_)
        for (const auto& r : p.rows)
            if (r.id == id) return &r;
    return nullptr;
}

void Model::set_value(int id, int value) {
    for (auto& p : pages_)
        for (auto& r : p.rows)
            if (r.id == id && !r.choices.empty())
                r.value = std::max(0, std::min(value, static_cast<int>(r.choices.size()) - 1));
}

Event Model::change(int i, int delta) {
    if (i < 0 || i >= rows_on_page()) return {};
    Row& r = pages_[page_].rows[i];
    const int n = static_cast<int>(r.choices.size());
    if (n == 0) return {};
    r.value = ((r.value + delta) % n + n) % n;
    return {Event::Changed, r.id, r.value};
}

Event Model::activate(int i) {
    if (i == -1) { open_ = false; return {Event::Closed, 0, 0}; }
    if (i < 0 || i >= rows_on_page()) return {};
    const Row& r = pages_[page_].rows[i];
    if (r.choices.empty()) return {Event::Action, r.id, 0};
    return change(i, +1);
}

Event Model::key(Key k) {
    if (!open_) return {};
    const int n = rows_on_page();
    switch (k) {
    case Key::Up:
        focus_ = focus_ == -1 ? n - 1 : focus_ == 0 ? -1 : focus_ - 1;
        break;
    case Key::Down:
        focus_ = focus_ == -1 ? (n > 0 ? 0 : -1) : focus_ + 1 >= n ? -1 : focus_ + 1;
        break;
    case Key::PageUp: focus_ = n > 0 ? std::max(0, (focus_ < 0 ? n : focus_) - visible_) : -1; break;
    case Key::PageDown: focus_ = n > 0 ? std::min(n - 1, std::max(0, focus_) + visible_) : -1; break;
    case Key::Left: return change(focus_, -1);
    case Key::Right: return change(focus_, +1);
    case Key::Enter: return activate(focus_);
    case Key::Escape: open_ = false; return {Event::Closed, 0, 0};
    case Key::Tab:
    case Key::BackTab:
        if (pages_.size() > 1) {
            const int pn = static_cast<int>(pages_.size());
            page_ = (page_ + (k == Key::Tab ? 1 : pn - 1)) % pn;
            focus_ = rows_on_page() > 0 ? 0 : -1;
            first_ = 0;
        }
        break;
    }
    keep_visible();
    return {};
}

int Model::hit(float x, float y) const {
    auto in = [&](const Box& b) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; };
    if (in(back_)) return -2;
    if (pages_.size() > 1)
        for (int i = 0; i < static_cast<int>(pages_.size()); ++i)
            if (in(tab(i))) return -3 - i;
    const int n = rows_on_page();
    for (int i = first_; i < n && i < first_ + visible_; ++i)
        if (in(row(i))) return i;
    return -1;
}

void Model::pointer(float x, float y) {
    if (!open_) return;
    hover_ = hit(x, y);
    if (hover_ >= 0) focus_ = hover_;
    else if (hover_ == -2) focus_ = -1;
}

Event Model::click(float x, float y) {
    if (!open_) return {};
    const int h = hit(x, y);
    hover_ = h;
    if (h == -2) return activate(-1);
    if (h <= -3) {
        page_ = -3 - h;
        focus_ = rows_on_page() > 0 ? 0 : -1;
        first_ = 0;
        keep_visible();
        return {};
    }
    if (h < 0) return {};
    focus_ = h;
    const Row& r = pages_[page_].rows[h];
    if (r.choices.empty()) return activate(h);
    Box v = value_box(h);
    if (x >= v.x && x < v.x + v.w / 2) return change(h, -1);   // left half of the value: back one
    return change(h, +1);
}

}  // namespace fo::modsettings
