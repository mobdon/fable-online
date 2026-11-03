#pragma once
// Reads textures from a Fable: The Lost Chapters graphics .big bank (e.g. data/graphics/pc/frontend.big)
// in the player's own install. Portable C++17: no Windows headers. Format notes: tools/re-notes/04-frontend-art.md.
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo::art {

enum class PixelFormat { DXT1, DXT3, DXT5, BGRA8 };

struct Texture {
    std::string name;
    uint16_t width = 0, height = 0;          // stored size (power of two)
    uint16_t frame_w = 0, frame_h = 0;        // the part that holds the picture (top-left)
    uint16_t frames = 1, format = 0;          // raw format id from the bank
    bool alpha = false;
    PixelFormat pixels = PixelFormat::BGRA8;
    std::vector<uint8_t> mip0;                // top mip level, DXT blocks or BGRA rows
};

class Bank {
public:
    ~Bank();
    bool open(const std::string& path);       // false if missing or not a BIGB bank
    bool open(const std::wstring& path);      // Windows paths (falls back to UTF-8 elsewhere)
    void close();
    bool has(const std::string& name) const { return index_.count(name) != 0; }
    // Reads and inflates one texture's top mip. False if unknown or corrupt.
    bool load(const std::string& name, Texture& out);
    std::vector<std::string> names() const;
    const std::string& error() const { return error_; }

private:
    struct Entry { uint32_t size = 0, offset = 0; std::vector<uint8_t> subheader; };
    bool read_index();
    bool read_at(uint32_t off, void* dst, size_t n);
    std::FILE* f_ = nullptr;
    std::unordered_map<std::string, Entry> index_;
    std::string error_;
};

}  // namespace fo::art
