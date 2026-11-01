#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/n64/level/LegendsTowerProgress.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("N64 Tower doors require their native obelisk sets", "[legends][tower-progress]") {
    LegendsProgress saved;
    REQUIRE(LegendsTowerProgress::of(saved).doors == std::array<bool, 4>{});
    constexpr std::array<u8, 4> kMasks{0x60, 7, 0x18, 0x80};
    for (usize door = 0; door < kMasks.size(); ++door) {
        for (u32 bits = 0; bits <= 255; ++bits) {
            saved.obelisks = bits;
            CAPTURE(door, bits);
            REQUIRE(LegendsTowerProgress::of(saved).doors[door] ==
                    ((bits & kMasks[door]) == kMasks[door]));
        }
    }
    saved.obelisks = 0x7f;
    REQUIRE(LegendsTowerProgress::of(saved).doors == std::array<bool, 4>{true, true, true, false});
    saved.obelisks |= 0x80;
    REQUIRE(LegendsTowerProgress::of(saved).doors == std::array<bool, 4>{true, true, true, true});
}

TEST_CASE("N64 Tower completion records do not replace missing obelisks",
          "[legends][tower-progress]") {
    LegendsProgress saved;
    for (const char* const map :
         {"mount1", "mount2", "mount3", "castle1", "castle6", "town1", "town2"}) {
        REQUIRE(saved.recordCompleted(map));
    }
    saved.runestones = 0x1fff;
    saved.shards = 0xf;
    saved.legendItems = ~u64{0};
    const auto state = LegendsTowerProgress::of(saved);
    REQUIRE(state.doors == std::array<bool, 4>{});
    REQUIRE(state.runestonePortal);
    REQUIRE(state.cathedralBeam);
}

TEST_CASE("N64 Tower installed pieces retain their native identities",
          "[legends][tower-progress]") {
    LegendsProgress saved;
    for (s32 bit = 0; bit < 64; ++bit) {
        saved.runestones = u64{1} << bit;
        saved.shards = u64{1} << bit;
        const auto state = LegendsTowerProgress::of(saved);
        CAPTURE(bit);
        REQUIRE(state.runestones == (bit < 13 ? u16{1} << bit : 0));
        REQUIRE(state.shards == (bit < 4 ? u8{1} << bit : 0));
        REQUIRE_FALSE(state.runestonePortal);
        REQUIRE_FALSE(state.cathedralBeam);
        REQUIRE(state.shardOpacity() == Catch::Approx(47.0f / 255.0f));
    }
    saved.runestones = 0x1fff;
    saved.shards = 0xf;
    const auto before = saved;
    const auto full = LegendsTowerProgress::of(saved);
    REQUIRE(full.runestonePortal);
    REQUIRE(full.cathedralBeam);
    REQUIRE(full.shardOpacity() == 1);
    REQUIRE(saved == before);
}
