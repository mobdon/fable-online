#pragma once
// Draws text and boxes over the game with Direct3D 9, using a font baked at start-up with GDI
// (no d3dx9 needed). Latin-1 only: other characters draw as '?'.
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

namespace fo::overlay {

class Renderer {
public:
    bool begin(IDirect3DDevice9* dev, int font_px);  // false if not ready this frame
    void end();
    void on_lost();                                   // before Reset: drop default-pool objects
    void release();

    void rect(float x, float y, float w, float h, uint32_t argb);
    // Draws UTF-8 text with a 1px shadow. Returns the width drawn.
    float text(float x, float y, const std::string& utf8, uint32_t argb, bool shadow = true);
    float measure(const std::string& utf8) const;
    // Splits text into lines no wider than max_w (breaks at spaces when it can).
    std::vector<std::string> wrap(const std::string& utf8, float max_w) const;
    float line_height() const { return static_cast<float>(cell_h_); }
    float screen_w() const { return screen_w_; }
    float screen_h() const { return screen_h_; }

private:
    struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };
    bool build_font(int px);
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c);
    void flush();

    IDirect3DDevice9* dev_ = nullptr;
    IDirect3DTexture9* tex_ = nullptr;      // managed pool: survives Reset
    IDirect3DStateBlock9* saved_ = nullptr; // default pool: dropped on Reset
    int font_px_ = 0;
    int tex_w_ = 512, tex_h_ = 512;
    int cell_w_ = 0, cell_h_ = 0;
    float adv_[256] = {};
    float screen_w_ = 0, screen_h_ = 0;
    std::vector<Vertex> verts_;
};

}  // namespace fo::overlay
