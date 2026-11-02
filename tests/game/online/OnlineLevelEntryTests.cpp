#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Party.h"
#include "game/context/GameContext.h"
#include "game/level/LevelCatalog.h"
#include "game/online/OnlineLevelEntry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The unpacked data with G1's map, and a context reading it. */
struct G1Entry {
    std::filesystem::path root =
        test::assetOrSkip("MAPS/LEVELG1/textures.ngc").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    GameContext context;
    G1Entry() {
        test::assetOrSkip("WDATA/TOWN.WAD");
        REQUIRE(catalog.load(root));
        context.unpackedRoot = root;
    }
    /** Updates until the entry leaves `phase`, after one frame shown when `shown`. */
    u32 ticksIn(OnlineLevelEntry& entry, test::FakeRenderDevice& device,
                OnlineLevelEntry::Phase phase, bool shown) const {
        if (shown) {
            entry.render(device, makeScreenProjection(640, 448), 640, 448);
        }
        u32 ticks = 0;
        while (entry.phase() == phase && ticks < 100000) {
            entry.update(device, context, false);
            ++ticks;
        }
        return ticks;
    }
};

TEST_CASE("a level entry whose map cannot show goes on after a wait, a shown one at once",
          "[online-entry][assets]") {
    const G1Entry data;
    const auto level = data.catalog.byName("G1");
    REQUIRE(level);
    const auto mapTicks = [&](bool shown) {
        test::FakeRenderDevice device;
        OnlineLevelEntry entry;
        entry.open(device, data.context, *level, {}); // nobody to want the movie: map, then ready
        REQUIRE(entry.phase() == OnlineLevelEntry::Phase::Map);
        const u32 ticks = data.ticksIn(entry, device, OnlineLevelEntry::Phase::Map, shown);
        CHECK(entry.phase() == OnlineLevelEntry::Phase::Ready);
        return ticks;
    };
    const u32 shown = mapTicks(true);
    CHECK(shown > 1);
    CHECK(mapTicks(false) == shown + OnlineLevelEntry::kUnshownTicks - 1);
}

TEST_CASE("an entry movie waits for its own first frame as its map did",
          "[online-entry][assets][movie]") {
    const G1Entry data;
    test::assetOrSkip("VQMOVIES/movieG1.avi");
    const auto level = data.catalog.byName("G1");
    REQUIRE(level);
    const std::array<PartyMember, 1> party{}; // one who has not seen it yet wants the movie
    const auto movieTicks = [&](bool shown) {
        test::FakeRenderDevice device;
        OnlineLevelEntry entry;
        entry.open(device, data.context, *level, party);
        data.ticksIn(entry, device, OnlineLevelEntry::Phase::Map, true);
        REQUIRE(entry.phase() == OnlineLevelEntry::Phase::Movie);
        const u32 ticks = data.ticksIn(entry, device, OnlineLevelEntry::Phase::Movie, shown);
        CHECK(entry.phase() == OnlineLevelEntry::Phase::Ready);
        return ticks;
    };
    const u32 shown = movieTicks(true);
    CHECK(shown > 1);
    CHECK(movieTicks(false) == shown + OnlineLevelEntry::kUnshownTicks - 1);
}

} // namespace
