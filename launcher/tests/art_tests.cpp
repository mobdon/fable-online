// art_tests.exe <lzo_case_dir> [frontend.big out_dir name...]
// 1. Decompresses every *.lzo in lzo_case_dir and compares with the matching *.raw.
// 2. Dumps named textures from a real frontend.big as raw ARGB (u32 w, u32 h, pixels).
#include "../../shared/art/big.h"
#include "../../shared/art/dxt.h"
#include "../../shared/art/lzo1x.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END);
    d.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
    return d;
}

int main(int argc, char** argv) {
    int fails = 0;
    if (argc < 2) return 2;
    for (int i = 0; i < 64; ++i) {
        std::string base = std::string(argv[1]) + "/case" + std::to_string(i);
        auto c = slurp(base + ".lzo"), r = slurp(base + ".raw");
        if (c.empty() && r.empty()) break;
        std::vector<uint8_t> out(r.size() + 16);
        long n = fo::art::lzo1x_decompress(c.data(), c.size(), out.data(), out.size());
        bool ok = n == long(r.size()) && std::equal(r.begin(), r.end(), out.begin());
        std::printf("%s lzo case%d (%zu bytes)\n", ok ? "ok  " : "FAIL", i, r.size());
        fails += !ok;
        // Truncated input must fail, not crash.
        if (c.size() > 8) fo::art::lzo1x_decompress(c.data(), c.size() / 2, out.data(), out.size());
    }
    if (argc >= 4) {
        fo::art::Bank b;
        std::wstring path(argv[2], argv[2] + std::strlen(argv[2]));
        bool opened = b.open(path);
        std::printf("%s bank opened, %zu entries\n", opened ? "ok  " : "FAIL", b.names().size());
        fails += !opened;
        for (int i = 4; i < argc; ++i) {
            fo::art::Texture t;
            fo::art::Image img;
            bool ok = b.load(argv[i], t) && fo::art::decode(t, img);
            std::printf("%s %s %dx%d\n", ok ? "ok  " : "FAIL", argv[i], img.w, img.h);
            fails += !ok;
            if (!ok) continue;
            FILE* f = std::fopen((std::string(argv[3]) + "/" + argv[i] + ".argb").c_str(), "wb");
            uint32_t hdr[2] = {uint32_t(img.w), uint32_t(img.h)};
            std::fwrite(hdr, 4, 2, f);
            std::fwrite(img.px.data(), 4, img.px.size(), f);
            std::fclose(f);
        }
    }
    std::printf("%s\n", fails ? "SOME TESTS FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
