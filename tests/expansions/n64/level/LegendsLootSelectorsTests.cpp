#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsLootSelectors.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value >> 8U);
    bytes[at + 1] = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
struct Tables {
    std::vector<u8> powers = std::vector<u8>(4 + 5 * 28);
    std::vector<u8> randoms = std::vector<u8>(4 + 7 * 8);
    Tables() {
        put32(powers, 0, 5);
        const std::array<u16, 5> keys{0, 0x301, 0x401, 0x2201, 0x2202};
        for (usize i = 0; i < keys.size(); ++i) {
            put16(powers, 4 + i * 28, keys[i]);
            put16(powers, 6 + i * 28, i < 3 ? 10 : 0);
        }
        put32(powers, 12 + 3 * 28, 0x54455354); // TEST
        put32(powers, 12 + 4 * 28, 0x4e455354); // NEST
        put32(randoms, 0, 7);
        put32(randoms, 4, 0x54455354);
        put16(randoms, 8, 3);
        put16(randoms, 10, 1);
        put16(randoms, 16, 4);
        put16(randoms, 18, 1);
        put32(randoms, 20, 0x53544f50); // STOP
        put32(randoms, 28, 0x4e455354);
        put16(randoms, 32, 0x22);
        put16(randoms, 34, 1);
        put16(randoms, 40, 2);
        put16(randoms, 42, 0x1b); // absent, native lookup falls back to index zero
        put32(randoms, 44, 0x53544f50);
        put32(randoms, 52, 0x454e4420); // END
    }
    static LegendsN64Fixture container(u32 index = 0) {
        LegendsN64Fixture result;
        result.kind = LegendsN64FixtureKind::Container;
        result.nativeIndex = index;
        result.templateKey = 0x1901;
        result.containedPowerup = 0x2201;
        return result;
    }
};

TEST_CASE("Concealed native loot retains its initial cache and exact zero-before-decrement clock",
          "[legends][loot-selectors]") {
    Tables data;
    const std::array fixtures{Tables::container()};
    LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
    REQUIRE(loot.slots().size() == 1);
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0);
    for (s32 i = 0; i < 20; ++i) {
        loot.tick();
    }
    CHECK(loot.slots()[0].waiting == 0);
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0);
    loot.tick();
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x401);
    CHECK(loot.slots()[0].waiting == 50);
    for (s32 i = 0; i < 50; ++i) {
        loot.tick();
    }
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x401);
    loot.tick();
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x301);
}

TEST_CASE(
    "Unset native concealed caches wait for the first selection without reading before PPower",
    "[legends][loot-selectors]") {
    Tables data;
    auto fixture = Tables::container();
    fixture.quantity = -1;
    const std::array fixtures{fixture};
    LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
    CHECK_FALSE(loot.selected(0));
    CHECK(loot.outcomes(0).size() == 2);
    SECTION("native timer populates the unset cache") {
        for (s32 i = 0; i < 20; ++i) {
            loot.tick();
            CHECK_FALSE(loot.selected(0));
        }
        loot.tick();
        REQUIRE(loot.selected(0));
        CHECK(loot.selected(0)->resolvedTemplateKey == 0x401);
    }
    SECTION("opening before the first selection freezes no invented item") {
        loot.freeze(0);
        for (s32 i = 0; i < 100; ++i) {
            loot.tick();
        }
        CHECK_FALSE(loot.selected(0));
        CHECK_FALSE(loot.slots()[0].active);
    }
}

TEST_CASE("Native concealed slots share cursors even when their parent is multiplayer-gated",
          "[legends][loot-selectors]") {
    Tables data;
    auto first = Tables::container(0);
    first.minimumPlayers = 4;
    const std::array fixtures{first, Tables::container(1)};
    LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
    REQUIRE(loot.slots().size() == 2);
    for (s32 i = 0; i < 21; ++i) {
        loot.tick();
    }
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x401);
    CHECK(loot.selected(1).value().resolvedTemplateKey == 0x301);
    loot.freeze(1);
    for (s32 i = 0; i < 153; ++i) {
        loot.tick();
    }
    CHECK(loot.selected(1).value().resolvedTemplateKey == 0x301);
    CHECK_FALSE(loot.slots()[1].active);
}

TEST_CASE("Native floor initialization and opening at timer zero resolve the shared selector",
          "[legends][loot-selectors]") {
    Tables data;
    auto floor = Tables::container();
    floor.kind = LegendsN64FixtureKind::Powerup;
    floor.templateKey = 0x2201;
    const std::array fixtures{Tables::container(), floor};
    LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
    REQUIRE(loot.slots().size() == 2);
    CHECK_FALSE(loot.slots()[0].contained);
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x401);
    loot.tick(); // floor initializes again at timer zero, then waits fifty ticks
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x301);
    for (s32 i = 0; i < 19; ++i) {
        loot.tick();
    }
    loot.freeze(1); // zero timer resolves now, without waiting for the next update
    CHECK(loot.selected(1).value().resolvedTemplateKey == 0x401);
    loot.disable(0);
    for (s32 i = 0; i < 100; ++i) {
        loot.tick();
    }
    CHECK(loot.selected(0).value().resolvedTemplateKey == 0x301);
}

TEST_CASE("Native nested selectors retain absent-key fallback and reject malformed cycles",
          "[legends][loot-selectors]") {
    Tables data;
    auto fixture = Tables::container();
    fixture.containedPowerup = 0x2202;
    const std::array fixtures{fixture};
    SECTION("native missing-key fallback and recursion") {
        LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
        REQUIRE(loot.outcomes(0).size() == 3);
        for (s32 i = 0; i < 21; ++i) {
            loot.tick();
        }
        CHECK(loot.selected(0).value().resolvedTemplateKey == 0);
        for (s32 i = 0; i < 51; ++i) {
            loot.tick();
        }
        CHECK(loot.selected(0).value().resolvedTemplateKey == 0x401);
    }
    SECTION("cyclic nested selector") {
        put16(data.randoms, 32, 0x22);
        put16(data.randoms, 34, 2);
        CHECK_THROWS_AS(LegendsLootSelectors(data.powers, data.randoms, fixtures, 1), FormatError);
    }
    SECTION("truncated table") {
        data.powers.pop_back();
        CHECK_THROWS_AS(LegendsLootSelectors(data.powers, data.randoms, fixtures, 1), FormatError);
    }
    SECTION("prebinding multiplication has a shared outcome budget") {
        std::vector<LegendsN64Fixture> crowded;
        for (u32 i = 0; i < 3000; ++i) {
            crowded.push_back(Tables::container(i));
        }
        CHECK_THROWS_AS(LegendsLootSelectors(data.powers, data.randoms, crowded, 1), FormatError);
    }
}

TEST_CASE("Native loot selection is invariant under render-step subdivision",
          "[legends][loot-selectors]") {
    Tables data;
    const std::array fixtures{Tables::container()};
    for (const s32 rate : {30, 60, 144}) {
        LegendsLootSelectors loot(data.powers, data.randoms, fixtures, 1);
        for (s32 i = 0; i < rate * 10; ++i) {
            loot.advance(1.0f / static_cast<f32>(rate));
        }
        CHECK(loot.slots()[0].waiting == 26);
        CHECK(loot.selected(0).value().resolvedTemplateKey == 0x301);
    }
}

TEST_CASE("All native maps resolve every concealed selector branch without invented loot",
          "[legends][loot-selectors][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root / "levels")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto generic = root / "items/generic";
    const auto powers = readFile(generic / "ppow.rom");
    const auto randoms = readFile(generic / "prnd.rom");
    const auto dec = readFile(generic / "pdec.rom");
    const auto gen = readFile(generic / "pgen.rom");
    const auto traps = readFile(generic / "ptrap.rom");
    const auto portals = readFile(generic / "ptel.rom");
    const auto containers = readFile(generic / "pcon.rom");
    usize maps = 0;
    usize hidden = 0;
    for (const auto& directory : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(directory.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(directory.path());
        const auto fixtures =
            decodeLegendsN64Fixtures(readFile(directory.path() / "ipr.rom"),
                                     {powers, dec, gen, traps, portals, containers, randoms}, 1);
        LegendsLootSelectors loot;
        REQUIRE_NOTHROW(loot = LegendsLootSelectors(powers, randoms, fixtures, 1));
        for (usize i = 0; i < loot.slots().size(); ++i) {
            hidden += loot.slots()[i].contained ? 1 : 0;
            const auto outcomes = loot.outcomes(i);
            REQUIRE_FALSE(outcomes.empty());
            for (const auto& choice : outcomes) {
                CHECK((choice.resolvedTemplateKey >> 8U) != 0x22);
            }
        }
        for (s32 i = 0; i < 300; ++i) {
            loot.tick();
        }
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(hidden > 100);
}
} // namespace
