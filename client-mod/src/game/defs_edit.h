#pragma once
// Reads Fable's compiled defs (game.bin with names.bin) into one byte buffer per entry, lets a
// caller change entries (including their length), and writes the file back. Only the chunks
// that hold a changed entry are re-encoded (as stored zlib blocks); every other byte is kept.
// Layout notes: tools/re-notes/07-game-bin.md.
//
// No Windows or game dependencies: tested on its own (tests/menu_row_test.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace fo::game::defs {

class Bin {
public:
    /** False with `error` set when the file can't be read. */
    bool load(const std::vector<uint8_t>& game_bin, const std::vector<uint8_t>& names_bin);
    /** The rebuilt file. Unchanged input gives the same bytes back. */
    bool save(std::vector<uint8_t>& out);

    /** Entry index of the def with this name, or -1. */
    int find_def(const std::string& name) const;
    uint32_t count() const { return static_cast<uint32_t>(entries_.size()); }
    const std::vector<uint8_t>& entry(uint32_t i) const { return entries_[i]; }
    /** Writable entry; marks its chunk for re-encoding. */
    std::vector<uint8_t>& edit(uint32_t i);

    std::string error;

private:
    std::vector<uint8_t> file_;
    std::vector<std::vector<uint8_t>> entries_;
    std::vector<std::string> def_names_;  // empty for types and components
    std::vector<uint32_t> first_, off_;   // chunk table
    std::vector<uint32_t> chunk_of_;
    std::vector<bool> dirty_;
    size_t table_pos_ = 0, size_pos_ = 0, base_ = 0;
    uint32_t size_field_ = 0;
    size_t max_packed_ = 0, max_plain_ = 0, total_plain_ = 0;
};

uint32_t rd32(const uint8_t* p);
void wr32(uint8_t* p, uint32_t v);
/** Byte offset of the first little-endian `v` at or after `from`, or -1. */
long find32(const uint8_t* p, size_t n, uint32_t v, size_t from = 0);

}  // namespace fo::game::defs
