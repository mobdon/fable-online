#pragma once
// Draws the overlay with Direct3D 9 in Fable's own HUD style: its bitmap fonts and its
// pewter-framed panels, read from the player's install (assets/fable_ui.h). Everything goes
// into one atlas texture, so a frame is a single draw call. Without the game files it falls
// back to Windows' Tahoma and a plain frame.
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../assets/fable_ui.h"

namespace fo::overlay {

enum class Face { Chat = 0, Small = 1, Title = 2 };

class Renderer {
public:
    // art must stay alive while the renderer is used. False if not ready this frame.
    bool begin(IDirect3DDevice9* dev, const assets::UiArt* art, int fallback_px);
    void end();
    void on_lost();                                   // before Reset: drop default-pool objects
    void release();

    void rect(float x, float y, float w, float h, uint32_t argb);
    // A Fable panel: dark fill, pewter frame with rivets in the corners.
    void panel(float x, float y, float w, float h, float alpha);
    // Fable's pewter scroll bar (its menu buttons): ornate ends when `ends`, warm when `on`.
    // Returns the width of one end piece at this height, so callers can inset text.
    float bar(float x, float y, float w, float h, float alpha, bool on, bool ends);
    // A thin pewter rule, like the dividers in Fable's menus.
    void rule(float x, float y, float w, float alpha);
    // UTF-8 text with Fable's soft black shadow (or a full outline for name tags).
    float text(float x, float y, const std::string& utf8, uint32_t argb, Face f = Face::Chat, bool outline = false);
    float measure(const std::string& utf8, Face f = Face::Chat) const;
    std::vector<std::string> wrap(const std::string& utf8, float max_w, Face f = Face::Chat) const;
    float line_height(Face f = Face::Chat) const;
    float screen_w() const { return screen_w_; }
    float screen_h() const { return screen_h_; }
    bool using_fable_art() const { return fable_fonts_; }

private:
    struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };
    struct Rect { int x = 0, y = 0, w = 0, h = 0; };
    struct FaceInfo { const assets::Font* font = nullptr; Rect at; };

    bool build_atlas(const assets::UiArt* art, int fallback_px);
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c);
    void sprite(const Rect& r, float x, float y, float w, float h, uint32_t c);
    const assets::Glyph& glyph(Face f, char32_t c) const;
    void flush();

    IDirect3DDevice9* dev_ = nullptr;
    IDirect3DTexture9* tex_ = nullptr;      // managed pool: survives Reset
    IDirect3DStateBlock9* saved_ = nullptr; // default pool: dropped on Reset
    int tex_w_ = 1024, tex_h_ = 1024;
    FaceInfo faces_[3];
    assets::Font fallback_[3];              // GDI Tahoma, when fonts.big couldn't be read
    Rect white_, tl_, tr_, bl_, br_, top_, bottom_, left_, right_;
    Rect bar_[2][3];                         // [off/on][left, middle, right]
    bool fable_fonts_ = false, fable_frame_ = false, fable_bar_ = false;
    uint32_t fill_ = 0xFF141E26;
    float screen_w_ = 0, screen_h_ = 0;
    std::vector<Vertex> verts_;
};

}  // namespace fo::overlay
