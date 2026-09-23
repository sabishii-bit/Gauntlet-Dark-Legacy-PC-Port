#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
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
            [this](std::size_t i, float damage, HurtKind kind, bool directed) {
                REQUIRE(damage == 5);
                REQUIRE(kind == HurtKind::Blow);
                REQUIRE(directed);
                calls.push_back("player" + std::to_string(i));
            },
        .help = [](int, std::size_t) { FAIL("Empty scenery has no help event"); },
        .card = [](int, std::string_view) { FAIL("Empty scenery has no pickup card"); },
        .opponents =
            [this](const Vec3&, float radius, float damage) {
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
