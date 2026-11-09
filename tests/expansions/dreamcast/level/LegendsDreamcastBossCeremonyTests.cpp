#include <array>
#include <bit>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastBossCeremony.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Ceremony = LegendsDreamcastBossCeremony;

std::vector<std::string> translated(const Ceremony& ceremony, const StringTable& strings) {
    std::vector<std::string> pages;
    for (const auto& key : ceremony.groups()[ceremony.groupIndex()].keys) {
        REQUIRE(strings.has(key));
        pages.emplace_back(strings.get(key));
    }
    return pages;
}

TEST_CASE("Dreamcast boss speech selects native party rune and shard branches",
          "[legends][dreamcast-boss-ceremony]") {
    for (s32 realm = 1; realm <= 4; ++realm) {
        for (u64 bits = 0; bits < 8; ++bits) {
            Ceremony visit;
            REQUIRE(visit.begin(26, realm, bits << (3 * (realm - 1)), 0));
            const usize count = static_cast<usize>(std::popcount(bits));
            CHECK(visit.groups()[1].keys[0] ==
                  "legends.dc.victory.runes" + std::to_string(count) + ".0");
            CHECK(visit.groups()[1].voice == 0x27000bU + count);
            CHECK(visit.groups()[2].voice == 0x27000fU);
        }
    }
    Ceremony incomplete;
    REQUIRE(incomplete.begin(27, 1, 0, 0xffffU & ~4U));
    CHECK(incomplete.groups()[2].keys[0] == "legends.dc.victory.remaining_shards.0");
    Ceremony complete;
    REQUIRE(complete.begin(27, 1, 0, 0x1e));
    CHECK(complete.groups()[2].keys[0] == "legends.dc.victory.all_shards.0");
    CHECK(complete.groups()[2].voice == 0x28001eU);
}

TEST_CASE("Dreamcast Skorne aftermath preserves its two different scripts",
          "[legends][dreamcast-boss-ceremony]") {
    Ceremony incomplete;
    REQUIRE(incomplete.begin(32, 5, 0x1000, 0x1e));
    CHECK_FALSE(incomplete.allRunes());
    CHECK(incomplete.groups()[0].keys.size() == 2);
    CHECK(incomplete.groups()[1].keys.size() == 4);
    CHECK(incomplete.groups()[1].voice == 0x2c0016U);
    CHECK(incomplete.groups()[2].keys.empty());
    CHECK(incomplete.ticksLeft() == 120);
    Ceremony complete;
    REQUIRE(complete.begin(32, 5, 0xfff, 0x1e));
    CHECK(complete.allRunes());
    CHECK(complete.groups()[1].keys.size() == 6);
    CHECK(complete.groups()[1].voice == 0x2c0017U);
    Ceremony finalBoss;
    REQUIRE(finalBoss.begin(33, 6, 0x1fff, 0x1e));
    CHECK(finalBoss.groups()[0].keys.size() == 6);
    CHECK(finalBoss.groups()[0].pageHoldTicks == 45);
    CHECK(finalBoss.groups()[0].voice == 0x2e0014U);
    CHECK(finalBoss.groups()[1].keys.empty());
    CHECK(finalBoss.groups()[2].keys.empty());
    CHECK(finalBoss.ticksLeft() == 300);
}

TEST_CASE("Dreamcast apparition wait and opacity use their native clock units",
          "[legends][dreamcast-boss-ceremony]") {
    Ceremony visit;
    CHECK_FALSE(visit.begin(25, 1, 0, 0));
    CHECK_FALSE(visit.running());
    REQUIRE(visit.begin(26, 2, 0, 0));
    CHECK_FALSE(visit.begin(32, 5, 0xfff, 0));
    CHECK(visit.boss() == 26);
    for (s32 tick = 0; tick < 299; ++tick) {
        CHECK_FALSE(visit.step(1, {}, false).appeared);
    }
    CHECK(visit.stage() == Ceremony::Stage::Waiting);
    CHECK(visit.step(1, {}, false).appeared);
    CHECK(visit.stage() == Ceremony::Stage::Appearing);
    CHECK(visit.wizardAlpha() == 0);
    CHECK_FALSE(visit.step(1, {}, false).voice);
    CHECK(visit.wizardAlpha() == 0);
    visit.step(1, {}, false);
    CHECK(visit.wizardAlpha() == Catch::Approx(4.0f / 255.0f));
    for (s32 frame = 1; frame < 63; ++frame) {
        CHECK_FALSE(visit.step(2, {}, false).voice);
    }
    CHECK(visit.wizardAlpha() == Catch::Approx(252.0f / 255.0f));
    CHECK(visit.step(2, {}, false).voice == 0x27000aU);
    CHECK(visit.stage() == Ceremony::Stage::Defeat);
    CHECK(visit.wizardAlpha() == 1);
    CHECK_FALSE(visit.step(0, {}, false).voice);
    CHECK_FALSE(visit.step(-1, {}, false).voice);
}

TEST_CASE("Dreamcast caption cursor uses integer native character costs",
          "[legends][dreamcast-boss-ceremony]") {
    Ceremony visit;
    REQUIRE(visit.begin(26, 1, 0, 0));
    while (visit.stage() != Ceremony::Stage::Defeat) {
        visit.step(2, {}, false);
    }
    const std::array<std::string, 1> pages{"A,\nB"};
    visit.step(2, pages, false);
    REQUIRE(visit.caption());
    CHECK(visit.caption()->shown == 1);
    visit.step(2, pages, false);
    CHECK(visit.caption()->shown == 1);
    visit.step(2, pages, false);
    CHECK(visit.caption()->shown == 2);
    visit.step(4, pages, false);
    CHECK(visit.caption()->shown == 2);
    visit.step(2, pages, false);
    CHECK(visit.caption()->shown == 4);
}

TEST_CASE("Dreamcast boss exit waits for speech and emits one final sparkle",
          "[legends][dreamcast-boss-ceremony]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    for (const s32 boss : {26, 32, 33}) {
        Ceremony visit;
        REQUIRE(visit.begin(boss, boss == 26 ? 2 : 5, 0xfff, 0x1e));
        std::vector<u32> voices;
        s32 frames = 0;
        while (visit.stage() != Ceremony::Stage::Speech && frames++ < 10000) {
            const auto pages = translated(visit, strings);
            const auto events = visit.step(2, pages, true);
            if (events.voice) {
                voices.push_back(*events.voice);
            }
            CHECK_FALSE(events.depart);
        }
        REQUIRE(visit.stage() == Ceremony::Stage::Speech);
        CHECK(voices.size() == (boss == 26 ? 3 : (boss == 32 ? 2 : 1)));
        for (s32 frame = 0; frame < 300; ++frame) {
            CHECK_FALSE(visit.step(2, {}, true).depart);
            CHECK_FALSE(visit.finished());
        }
        CHECK(visit.step(2, {}, false).depart);
        const s32 leaveTicks = boss == 32 ? 240 : 120;
        CHECK(visit.ticksLeft() == leaveTicks);
        s32 sparkles = 0;
        s32 completions = 0;
        for (s32 tick = 2; tick <= leaveTicks; tick += 2) {
            const auto events = visit.step(2, {}, false);
            if (events.sparkle) {
                ++sparkles;
                CHECK(tick == leaveTicks - 20);
            }
            completions += events.finished ? 1 : 0;
            CHECK_FALSE(events.depart);
            CHECK_FALSE(events.voice);
        }
        CHECK(sparkles == 1);
        CHECK(completions == 1);
        CHECK(visit.finished());
        CHECK_FALSE(visit.running());
        CHECK_FALSE(visit.step(600, {}, false).finished);
    }
}

TEST_CASE("Dreamcast missing text cannot trap a completed boss encounter",
          "[legends][dreamcast-boss-ceremony]") {
    Ceremony visit;
    REQUIRE(visit.begin(26, 2, 0, 0));
    for (s32 frame = 0; frame < 2000 && visit.running(); ++frame) {
        visit.step(2, {}, false);
    }
    CHECK(visit.finished());
    visit.clear();
    CHECK(visit.stage() == Ceremony::Stage::None);
    CHECK_FALSE(visit.caption());
}

TEST_CASE("Dreamcast wizard placement follows party floor not boss height",
          "[legends][dreamcast-boss-ceremony]") {
    const Vec3 anchor{30, 100, 60};
    const std::array<Vec3, 2> party{{{0, 2, 0}, {0, 5, 0}}};
    const Vec3 result = Ceremony::wizardPosition(26, anchor, party);
    CHECK(result.x == Catch::Approx(10));
    CHECK(result.y == Catch::Approx(6));
    CHECK(result.z == Catch::Approx(20));
    CHECK(Ceremony::wizardPosition(32, anchor, party) == Vec3{0, -12, 6});
    CHECK(Ceremony::wizardPosition(33, anchor, {}) == Vec3{0, -12, 6});
}

TEST_CASE("Dreamcast victory captions and voices match the native executable tables",
          "[legends][dreamcast-boss-ceremony][assets]") {
    const auto path =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/1ST_READ.BIN";
    if (!std::filesystem::exists(path)) {
        SKIP("optional Dreamcast executable is not installed");
    }
    const auto image = readFile(path);
    constexpr u32 kBase = 0x8c010000;
    const auto word = [&](u32 address) {
        const usize offset = address - kBase;
        REQUIRE(offset + 4 <= image.size());
        return static_cast<u32>(image[offset]) | (static_cast<u32>(image[offset + 1]) << 8) |
               (static_cast<u32>(image[offset + 2]) << 16) |
               (static_cast<u32>(image[offset + 3]) << 24);
    };
    const auto nativeText = [&](u32 table, usize page) {
        const u32 pointer = word(table + static_cast<u32>(page * 12));
        REQUIRE(pointer >= kBase);
        std::string result;
        usize offset = pointer - kBase;
        while (offset < image.size() && image[offset] != 0 && result.size() < 1024) {
            result.push_back(static_cast<char>(image[offset++]));
        }
        REQUIRE(offset < image.size());
        REQUIRE(image[offset] == 0);
        return result;
    };
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    constexpr std::array<u32, 8> kDefeatTables{0x8c14546c, 0x8c145460, 0x8c145478, 0x8c145484,
                                               0x8c14549c, 0x8c145490, 0x8c145388, 0x8c145418};
    for (s32 boss = 26; boss <= 33; ++boss) {
        for (const bool full : {false, true}) {
            Ceremony visit;
            REQUIRE(visit.begin(boss, 1, full ? 0xfff : 0, full ? 0x1e : 0));
            const auto& groups = visit.groups();
            for (usize group = 0; group < groups.size(); ++group) {
                const u32 table = group == 0   ? kDefeatTables[static_cast<usize>(boss - 26)]
                                  : group == 1 ? (boss == 32 ? (full ? 0x8c1453d0 : 0x8c1453a0)
                                                             : (full ? 0x8c1454cc : 0x8c1454a8))
                                               : (full ? 0x8c1454f0 : 0x8c1454d8);
                for (usize page = 0; page < groups[group].keys.size(); ++page) {
                    const auto& key = groups[group].keys[page];
                    INFO(key);
                    REQUIRE(strings.has(key));
                    CHECK(strings.get(key) == nativeText(table, page));
                }
                if (!groups[group].voice) {
                    continue;
                }
                const usize voiceGroup = group == 0 ? 0
                                         : group == 1
                                             ? (boss == 32 ? (full ? 2 : 1) : (full ? 4 : 1))
                                             : (full ? 6 : 5);
                CHECK(*groups[group].voice ==
                      word(0x8c127dcc + static_cast<u32>((boss - 26) * 28 + voiceGroup * 4)));
            }
        }
    }
}
} // namespace
