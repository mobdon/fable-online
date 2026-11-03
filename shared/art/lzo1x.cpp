#include "lzo1x.h"

namespace fo::art {

long lzo1x_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    size_t ip = 0, op = 0;
    size_t t = 0;
    size_t m_pos = 0;
    enum State { Loop, FirstLiteralRun, Match, CopyMatch, MatchDone, MatchNext } st = Loop;

    auto need_in = [&](size_t n) { return ip + n <= in_len; };
    auto need_out = [&](size_t n) { return op + n <= out_cap; };
    auto copy_lit = [&](size_t n) {
        if (!need_in(n) || !need_out(n)) return false;
        for (size_t i = 0; i < n; ++i) out[op++] = in[ip++];
        return true;
    };
    auto copy_back = [&](size_t from, size_t n) {
        if (from >= op || !need_out(n)) return false;
        for (size_t i = 0; i < n; ++i) out[op++] = out[from++];
        return true;
    };
    // Reads a run length: zero bytes add 255 each, the first non-zero byte ends it.
    auto ext = [&](size_t base) -> long {
        size_t v = 0;
        while (true) {
            if (!need_in(1)) return -1;
            uint8_t b = in[ip++];
            if (b) return static_cast<long>(v + base + b);
            v += 255;
        }
    };

    if (in_len == 0) return 0;
    if (in[0] > 17) {
        t = in[ip++] - 17u;
        if (t < 4) {
            st = MatchNext;
        } else {
            if (!copy_lit(t)) return -1;
            st = FirstLiteralRun;
        }
    }

    while (true) {
        switch (st) {
        case Loop: {
            if (!need_in(1)) return static_cast<long>(op);  // input ran out cleanly
            t = in[ip++];
            if (t >= 16) { st = Match; break; }
            if (t == 0) { long e = ext(15); if (e < 0) return -1; t = static_cast<size_t>(e); }
            if (!copy_lit(t + 3)) return -1;
            st = FirstLiteralRun;
            break;
        }
        case FirstLiteralRun: {
            if (!need_in(1)) return static_cast<long>(op);
            t = in[ip++];
            if (t >= 16) { st = Match; break; }
            if (!need_in(1)) return -1;
            size_t back = 1 + 0x0800 + (t >> 2) + (static_cast<size_t>(in[ip++]) << 2);
            if (back > op) return -1;
            if (!copy_back(op - back, 3)) return -1;
            st = MatchDone;
            break;
        }
        case Match: {
            if (t >= 64) {
                if (!need_in(1)) return -1;
                size_t back = 1 + ((t >> 2) & 7) + (static_cast<size_t>(in[ip++]) << 3);
                if (back > op) return -1;
                m_pos = op - back;
                t = (t >> 5) - 1;
                st = CopyMatch;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) { long e = ext(31); if (e < 0) return -1; t = static_cast<size_t>(e); }
                if (!need_in(2)) return -1;
                size_t back = 1 + (in[ip] >> 2) + (static_cast<size_t>(in[ip + 1]) << 6);
                ip += 2;
                if (back > op) return -1;
                m_pos = op - back;
                st = CopyMatch;
            } else if (t >= 16) {
                size_t back = (t & 8) << 11;
                t &= 7;
                if (t == 0) { long e = ext(7); if (e < 0) return -1; t = static_cast<size_t>(e); }
                if (!need_in(2)) return -1;
                back += (in[ip] >> 2) + (static_cast<size_t>(in[ip + 1]) << 6);
                ip += 2;
                if (back == 0) return static_cast<long>(op);  // end-of-stream marker
                back += 0x4000;
                if (back > op) return -1;
                m_pos = op - back;
                st = CopyMatch;
            } else {
                if (!need_in(1)) return -1;
                size_t back = 1 + (t >> 2) + (static_cast<size_t>(in[ip++]) << 2);
                if (back > op) return -1;
                if (!copy_back(op - back, 2)) return -1;
                st = MatchDone;
            }
            break;
        }
        case CopyMatch:
            if (!copy_back(m_pos, t + 2)) return -1;
            st = MatchDone;
            break;
        case MatchDone:
            t = in[ip - 2] & 3u;
            st = t == 0 ? Loop : MatchNext;
            break;
        case MatchNext:
            if (!copy_lit(t)) return -1;
            if (!need_in(1)) return static_cast<long>(op);
            t = in[ip++];
            st = Match;
            break;
        }
    }
}

}  // namespace fo::art
