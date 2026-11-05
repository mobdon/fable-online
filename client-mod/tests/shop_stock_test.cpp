// Shop stock filter and the zlib reader. Needs the fixture from shop_stock_check.ts:
//   (cd server && npx tsx ../client-mod/tests/shop_stock_check.ts make DIR)
//   shop_stock_tests DIR        (writes DIR/filtered.bin)
//   (cd server && npx tsx ../client-mod/tests/shop_stock_check.ts check DIR)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/game/shop_stock.h"
#include "../src/util/inflate.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::vector<uint8_t> load(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::printf("missing %s\n", p.c_str()); std::exit(2); }
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: shop_stock_tests FIXTURE_DIR\n"); return 2; }
    std::string dir = argv[1];
    using namespace fo;

    // zlib reader against Node's zlib, every level and the fixed-Huffman strategy.
    auto count = load(dir + "/vectors");
    int vectors = std::atoi(std::string(count.begin(), count.end()).c_str());
    CHECK(vectors > 0);
    for (int i = 0; i < vectors; ++i) {
        auto raw = load(dir + "/vec" + std::to_string(i) + ".raw");
        auto z = load(dir + "/vec" + std::to_string(i) + ".z");
        std::vector<uint8_t> out;
        CHECK(util::zlib_inflate(z.data(), z.size(), out));
        CHECK(out == raw);
        z[z.size() / 2] ^= 0x55;  // corrupt: must be refused (Huffman error or Adler-32 mismatch)
        CHECK(!util::zlib_inflate(z.data(), z.size(), out) || out != raw);
    }
    // Stored round trip, including more than one 64 KB block and an empty input.
    for (size_t n : {size_t(0), size_t(1), size_t(65535), size_t(65536), size_t(200000)}) {
        std::vector<uint8_t> data(n);
        for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>(i * 7 + (i >> 9));
        auto z = util::zlib_store(data.data(), data.size());
        std::vector<uint8_t> out;
        CHECK(util::zlib_inflate(z.data(), z.size(), out));
        CHECK(out == data);
    }

    CHECK(game::shop_stock::fable_hash("FRONT_END") == 0x0A7D3E6Cu);

    auto game = load(dir + "/game.bin");
    auto names = load(dir + "/names.bin");
    std::vector<uint8_t> out;
    auto r = game::shop_stock::filter(game, names, out);
    if (!r.ok) std::printf("filter error: %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.shops == 1);
    CHECK(r.banned_defs == 3);  // phial, plate shirt, platinum helmet
    CHECK(r.rows_cleared == 3);
    CHECK(r.chunks_rewritten == 1);
    CHECK((r.cleared == std::vector<std::string>{"OBJECT_HERO_HELMET_PLATINUM", "OBJECT_HERO_SHIRT_PLATE", "OBJECT_RESURRECTION_PHIAL"}));
    {
        std::ofstream f(dir + "/filtered.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }

    // Running it again finds nothing more to do and returns the file unchanged.
    std::vector<uint8_t> again;
    auto r2 = game::shop_stock::filter(out, names, again);
    CHECK(r2.ok && r2.rows_cleared == 0 && again == out);

    // A mismatched names.bin or a damaged file is refused, never half-written.
    auto bad_names = names;
    bad_names[4] ^= 1;
    CHECK(!game::shop_stock::filter(game, bad_names, again).ok && again.empty());
    auto bad_game = game;
    bad_game[bad_game.size() - 10] ^= 0xFF;
    CHECK(!game::shop_stock::filter(bad_game, names, again).ok && again.empty());

    std::printf(g_fail ? "shop_stock: %d failures\n" : "shop_stock: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
