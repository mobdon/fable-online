// Canonical-Huffman inflate in the style of zlib's contrib/puff: slow per byte but tiny, and
// game.bin is only inflated once per launch.
#include "inflate.h"

#include <cstring>

namespace fo::util {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLitLen = 286;
constexpr int kMaxDist = 30;

struct In {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool bad = false;

    int bits(int need) {
        uint32_t v = bitbuf;
        while (bitcnt < need) {
            if (pos >= n) { bad = true; return 0; }
            v |= static_cast<uint32_t>(p[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = v >> need;
        bitcnt -= need;
        return static_cast<int>(v & ((1u << need) - 1));
    }
};

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];  // the fixed code has 288 literal/length symbols
};

// Builds the decoding tables. Returns <0 for an over-subscribed code, 0 complete, >0 incomplete.
int construct(Huffman& h, const short* length, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
    return left;
}

int decode(In& in, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= in.bits(1);
        if (in.bad) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(In& in, std::vector<uint8_t>& out, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int sym = decode(in, lencode);
        if (sym < 0) return false;
        if (sym < 256) { out.push_back(static_cast<uint8_t>(sym)); continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = kLenBase[sym] + in.bits(kLenExtra[sym]);
        int ds = decode(in, distcode);
        if (ds < 0 || ds >= 30) return false;
        size_t dist = static_cast<size_t>(kDistBase[ds] + in.bits(kDistExtra[ds]));
        if (in.bad || dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
}

bool stored(In& in, std::vector<uint8_t>& out) {
    in.bitbuf = 0;
    in.bitcnt = 0;
    if (in.pos + 4 > in.n) return false;
    unsigned len = in.p[in.pos] | (in.p[in.pos + 1] << 8);
    unsigned nlen = in.p[in.pos + 2] | (in.p[in.pos + 3] << 8);
    in.pos += 4;
    if (len != (~nlen & 0xFFFF) || in.pos + len > in.n) return false;
    out.insert(out.end(), in.p + in.pos, in.p + in.pos + len);
    in.pos += len;
    return true;
}

bool fixed(In& in, std::vector<uint8_t>& out) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        construct(lencode, lengths, 288);
        for (s = 0; s < kMaxDist; ++s) lengths[s] = 5;
        construct(distcode, lengths, kMaxDist);
        built = true;
    }
    return codes(in, out, lencode, distcode);
}

bool dynamic(In& in, std::vector<uint8_t>& out) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[kMaxLitLen + kMaxDist];
    int nlen = in.bits(5) + 257;
    int ndist = in.bits(5) + 1;
    int ncode = in.bits(4) + 4;
    if (in.bad || nlen > kMaxLitLen || ndist > kMaxDist) return false;
    int index = 0;
    for (; index < ncode; ++index) lengths[order[index]] = static_cast<short>(in.bits(3));
    for (; index < 19; ++index) lengths[order[index]] = 0;
    Huffman lencode{}, distcode{};
    if (construct(lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(in, lencode);
        if (sym < 0) return false;
        if (sym < 16) { lengths[index++] = static_cast<short>(sym); continue; }
        short len = 0;
        int rep;
        if (sym == 16) {
            if (index == 0) return false;
            len = lengths[index - 1];
            rep = 3 + in.bits(2);
        } else if (sym == 17) {
            rep = 3 + in.bits(3);
        } else {
            rep = 11 + in.bits(7);
        }
        if (in.bad || index + rep > nlen + ndist) return false;
        while (rep--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;
    int err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(in, out, lencode, distcode);
}

}  // namespace

uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *data++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

bool zlib_inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n < 6) return false;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    In in{src + 2, n - 2};
    int last;
    do {
        last = in.bits(1);
        int type = in.bits(2);
        if (in.bad) return false;
        bool ok = type == 0 ? stored(in, out) : type == 1 ? fixed(in, out) : type == 2 ? dynamic(in, out) : false;
        if (!ok || in.bad) return false;
    } while (!last);
    // The Adler-32 trailer starts at the next whole byte.
    size_t at = 2 + in.pos;
    if (at + 4 > n) return false;
    uint32_t want = (static_cast<uint32_t>(src[at]) << 24) | (src[at + 1] << 16) | (src[at + 2] << 8) | src[at + 3];
    return want == adler32(out.data(), out.size());
}

std::vector<uint8_t> zlib_store(const uint8_t* data, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n + n / 65535 * 5 + 11);
    out.push_back(0x78);
    out.push_back(0x01);
    size_t pos = 0;
    do {
        size_t len = n - pos < 65535 ? n - pos : 65535;
        bool last = pos + len == n;
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len));
        out.push_back(static_cast<uint8_t>(~len >> 8));
        out.insert(out.end(), data + pos, data + pos + len);
        pos += len;
    } while (pos < n);
    uint32_t a = adler32(data, n);
    out.push_back(static_cast<uint8_t>(a >> 24));
    out.push_back(static_cast<uint8_t>(a >> 16));
    out.push_back(static_cast<uint8_t>(a >> 8));
    out.push_back(static_cast<uint8_t>(a));
    return out;
}

}  // namespace fo::util
