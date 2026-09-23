#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixtures.h"
namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 3> players;
    std::vector<std::string> calls;
    LevelFixtures::Events events{
        .hurt =
            [this](usize i, f32 damage, HurtKind kind, bool directed) {
                REQUIRE(damage == 5);
                REQUIRE(kind == HurtKind::Blow);
                REQUIRE(directed);
                calls.push_back("player" + std::to_string(i));
            },
        .help = [](s32, usize) { FAIL("Empty scenery has no help event"); },
        .card = [](s32, std::string_view) { FAIL("Empty scenery has no pickup card"); },
        .opponents =
            [this](const Vec3&, f32 radius, f32 damage) {
                REQUIRE(radius == 2);
                REQUIRE(damage == 5);
                calls.emplace_back("opponents");
            }};
    Fixture() {
        fixtures.bind({device, world, weapons, effects, audio, 1});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{100, 0, 100}, 0);
        players[2].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
        players[2].life = PlayerLife::InTower;
    }
};

TEST_CASE("fixture explosions resolve live nearby players before opponents and drain once",
          "[game][screens][level-fixtures]") {
    Fixture f;
    f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
    REQUIRE(f.calls == std::vector<std::string>{"player0", "opponents"});
    f.fixtures.settleBlasts(f.players, f.events);
    REQUIRE(f.calls.size() == 2);
    f.fixtures.clear();
    f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
    REQUIRE(f.calls.size() == 2);
}

TEST_CASE("Dragon arena vents retain the realm's figures alongside boss-specific items",
          "[game][screens][level-fixtures][boss-stage][unpacked]") {
    const auto root = test::unpackedOrSkip("ITEMS/LEVELB/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("ITEMS/LEVELB6/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELB6/world.json");
    test::unpackedOrSkip("wdata/MOUNT.json");
    Fixture fixture;
    fixture.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("B6");
    REQUIRE(level.has_value());
    REQUIRE(fixture.world.load(fixture.device, root, *level));
    REQUIRE(fixture.world.items().trees.find("WIZARD").has_value());
    REQUIRE_FALSE(fixture.world.items().trees.find("FLAMEV").has_value());
    REQUIRE(fixture.world.realmItems().trees.find("FLAMEV").has_value());
    fixture.fixtures.bind(
        {fixture.device, fixture.world, fixture.weapons, fixture.effects, fixture.audio, 1});
    const Traps& traps = fixture.fixtures.traps();
    REQUIRE(traps.size() == 10);
    for (usize i = 0; i < traps.size(); ++i) {
        const ItemFigure& vent = traps.trap(i).figure;
        REQUIRE(vent.hasFigure());
        REQUIRE(vent.sequenceCount() == 4); // OFF, ONA, ON, ONB, not an invented one-tick cycle
        REQUIRE(vent.ticksOf(2) > 1);
    }
    fixture.fixtures.clear(); // borrowed figures must go before either archive
    fixture.world.clear();
    REQUIRE_FALSE(fixture.world.realmItems().loaded());
}

TEST_CASE("fixture updates age per-player hazard cooldowns without reordering the party",
          "[game][screens][level-fixtures]") {
    Fixture f;
    f.players[0].hitSoundGap = 3;
    f.players[1].hitSoundGap = 1;
    f.players[0].cloudGap = 0.1f;
    f.fixtures.update(2, 0.2f, f.players, f.events);
    REQUIRE(f.calls.empty());
    REQUIRE(f.players[0].hitSoundGap == 1);
    REQUIRE(f.players[1].hitSoundGap == 0);
    REQUIRE(f.players[0].cloudGap == 0);
    REQUIRE(f.players[0].actor.player() == 3);
    REQUIRE(f.players[1].actor.player() == 1);
}
} // namespace
