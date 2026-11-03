#include "big.h"
#include <cstring>
#include "lzo1x.h"

namespace fo::art {
namespace {
uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

Bank::~Bank() { close(); }

void Bank::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    index_.clear();
}

bool Bank::open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) { error_ = "can't open " + path; return false; }
    return read_index();
}

bool Bank::open(const std::wstring& path) {
    close();
#ifdef _WIN32
    f_ = _wfopen(path.c_str(), L"rb");
#else
    std::string narrow;
    for (wchar_t c : path) narrow += static_cast<char>(c < 0x80 ? c : '?');
    f_ = std::fopen(narrow.c_str(), "rb");
#endif
    if (!f_) { error_ = "can't open bank"; return false; }
    return read_index();
}

bool Bank::read_at(uint32_t off, void* dst, size_t n) {
    return std::fseek(f_, static_cast<long>(off), SEEK_SET) == 0 && std::fread(dst, 1, n, f_) == n;
}

bool Bank::read_index() {
    uint8_t hdr[16];
    if (!read_at(0, hdr, sizeof hdr) || std::memcmp(hdr, "BIGB", 4) != 0) { error_ = "not a BIGB file"; return false; }
    uint32_t bank_index = u32(hdr + 8);
    // Bank index: u32 bank_count, cstring name, u32 id, u32 entry_count, u32 entries_offset, u32 entries_size.
    std::vector<uint8_t> bi(512);
    std::fseek(f_, 0, SEEK_END);
    long file_size = std::ftell(f_);
    if (bank_index + 8u > static_cast<uint32_t>(file_size)) { error_ = "bad bank index"; return false; }
    size_t bn = std::min<size_t>(bi.size(), static_cast<size_t>(file_size) - bank_index);
    if (!read_at(bank_index, bi.data(), bn)) { error_ = "bad bank index"; return false; }
    size_t p = 4;
    while (p < bn && bi[p]) ++p;
    ++p;
    if (p + 16 > bn) { error_ = "bad bank index"; return false; }
    uint32_t count = u32(&bi[p + 4]), eoff = u32(&bi[p + 8]), esize = u32(&bi[p + 12]);
    if (eoff + static_cast<uint64_t>(esize) > static_cast<uint64_t>(file_size)) { error_ = "bad entry table"; return false; }
    std::vector<uint8_t> e(esize);
    if (!read_at(eoff, e.data(), esize)) { error_ = "bad entry table"; return false; }
    size_t q = 12;  // three u32s before the first entry
    auto need = [&](size_t n) { return q + n <= e.size(); };
    for (uint32_t i = 0; i < count; ++i) {
        // u32 magic, id, type, size, offset, crc, name_len; name; u32 crc, source_count; sources; u32 sub_len; sub
        if (!need(28)) break;
        uint32_t size = u32(&e[q + 12]), offset = u32(&e[q + 16]), nlen = u32(&e[q + 24]);
        q += 28;
        if (!need(nlen + 8)) break;
        std::string name(reinterpret_cast<const char*>(&e[q]), nlen);
        q += nlen;
        uint32_t nsrc = u32(&e[q + 4]);
        q += 8;
        for (uint32_t s = 0; s < nsrc; ++s) {
            if (!need(4)) { q = e.size(); break; }
            q += 4 + u32(&e[q]);
        }
        if (!need(4)) break;
        uint32_t sl = u32(&e[q]);
        q += 4;
        if (!need(sl)) break;
        Entry ent;
        ent.size = size;
        ent.offset = offset;
        ent.subheader.assign(e.begin() + q, e.begin() + q + sl);
        q += sl;
        index_[name] = std::move(ent);
    }
    if (index_.empty()) { error_ = "no entries"; return false; }
    return true;
}

std::vector<std::string> Bank::names() const {
    std::vector<std::string> v;
    for (auto& kv : index_) v.push_back(kv.first);
    return v;
}

bool Bank::load(const std::string& name, Texture& out) {
    auto it = index_.find(name);
    if (it == index_.end() || !f_) { error_ = "no texture " + name; return false; }
    const Entry& ent = it->second;
    const auto& sh = ent.subheader;
    // u16 w, h, depth, frame_w, frame_h, frames, format, ?; u8 alpha, mips; u16 ?; u32 mip0_size, mip0_compressed
    if (sh.size() < 28) { error_ = name + ": short header"; return false; }
    out = Texture{};
    out.name = name;
    out.width = u16(&sh[0]);
    out.height = u16(&sh[2]);
    out.frame_w = u16(&sh[6]);
    out.frame_h = u16(&sh[8]);
    out.frames = u16(&sh[10]);
    out.format = u16(&sh[12]);
    out.alpha = sh[16] != 0;
    uint32_t msize = u32(&sh[20]), mcomp = u32(&sh[24]);
    const uint32_t w = out.width, h = out.height;
    if (!w || !h || msize == 0 || msize > (64u << 20) || ent.size > (64u << 20)) { error_ = name + ": bad size"; return false; }

    std::vector<uint8_t> raw(ent.size);
    if (!read_at(ent.offset, raw.data(), raw.size())) { error_ = name + ": read failed"; return false; }
    out.mip0.assign(msize, 0);
    if (mcomp == msize) {
        if (raw.size() < msize) { error_ = name + ": truncated"; return false; }
        std::memcpy(out.mip0.data(), raw.data(), msize);
    } else {
        // u16 chunk length (0xFFFF: a u32 length follows), then LZO1X.
        size_t p = 0;
        if (raw.size() < 2) { error_ = name + ": truncated"; return false; }
        uint32_t cl = u16(&raw[0]);
        p = 2;
        if (cl == 0xFFFF) {
            if (raw.size() < 6) { error_ = name + ": truncated"; return false; }
            cl = u32(&raw[2]);
            p = 6;
        }
        if (p + cl > raw.size()) cl = static_cast<uint32_t>(raw.size() - p);
        long got = lzo1x_decompress(&raw[p], cl, out.mip0.data(), out.mip0.size());
        // Some chunks end a few bytes short of mip0_size; the tail stays zero (last DXT block).
        if (got < 0 || static_cast<uint32_t>(got) + 16 < msize) { error_ = name + ": corrupt data"; return false; }
    }
    uint32_t frames = out.frames ? out.frames : 1;
    if (msize == w * h / 2 || msize == w * h / 2 * frames) out.pixels = PixelFormat::DXT1;
    else if (msize == w * h || msize == w * h * frames) out.pixels = out.alpha ? PixelFormat::DXT5 : PixelFormat::DXT3;
    else out.pixels = PixelFormat::BGRA8;
    return true;
}

bool Bank::load_raw(const std::string& name, std::vector<uint8_t>& out) {
    auto it = index_.find(name);
    if (it == index_.end() || !f_) { error_ = "no entry " + name; return false; }
    if (it->second.size > (64u << 20)) { error_ = name + ": bad size"; return false; }
    out.resize(it->second.size);
    if (!read_at(it->second.offset, out.data(), out.size())) { error_ = name + ": read failed"; return false; }
    return true;
}

}  // namespace fo::art
