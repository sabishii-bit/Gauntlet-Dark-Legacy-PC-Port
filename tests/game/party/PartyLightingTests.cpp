#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "game/effects/DynamicLights.h"
#include "game/party/AttackSceneFixture.h"
#include "game/party/PartyFigures.h"
#include "game/party/PartyLighting.h"

namespace {
using namespace gdl;
using namespace gdl::game;
void sameLights(std::span<const PointLight> actual, std::span<const PointLight> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK(actual[index].position == expected[index].position);
        CHECK(actual[index].color == expected[index].color);
        CHECK(actual[index].radius == expected[index].radius);
        CHECK(actual[index].intensity == expected[index].intensity);
    }
}
} // namespace

TEST_CASE("party lighting rebuild preserves native effect order truncation and moving lanterns",
          "[rollback][party-lighting]") {
    auto fixture = std::make_unique<test::AttackSceneFixture>();
    auto& native = *fixture;
    LevelInfo level;
    level.flags = DynamicLights::kDarkLevel;
    std::vector<PointLight> actual;
    std::vector<PointLight> expected;
    const auto nativeGather = [&] {
        expected.clear();
        native.effects.lights(expected);
        native.opponents.lights(expected);
        PartyFigures::addLanterns(expected, native.players, &level);
        if (expected.size() > WorldLighting::kMostPoints) {
            expected.resize(WorldLighting::kMostPoints);
        }
    };
    PartyLighting::gather(actual, native.effects, native.opponents, native.players, &level);
    nativeGather();
    sameLights(actual, expected);
    REQUIRE(actual.size() == 1);
    const auto oldPosition = actual[0].position;
    native.players[0].actor.place(native.players[0].actor.position() + Vec3{3, 0, 1});
    PartyLighting::gather(actual, native.effects, native.opponents, native.players, &level);
    nativeGather();
    sameLights(actual, expected);
    CHECK(actual[0].position != oldPosition);

    for (usize index = 0; index < WorldLighting::kMostPoints + 2; ++index) {
        EffectTrees::Setting setting;
        setting.seconds = 2;
        setting.light = EffectTrees::Light{{1, 0.25f, 0.5f}, 10, false};
        REQUIRE(native.effects.startSet(native.device, native.weapons, "BURST",
                                        Vec3{static_cast<f32>(index), 0, 0}, setting) != 0);
    }
    PartyLighting::gather(actual, native.effects, native.opponents, native.players, &level);
    nativeGather();
    sameLights(actual, expected);
    REQUIRE(actual.size() == WorldLighting::kMostPoints);
    CHECK(actual.front().position.x == static_cast<f32>(WorldLighting::kMostPoints + 1));
    native.effects.clear();
    PartyLighting::gather(actual, native.effects, native.opponents, {}, nullptr);
    CHECK(actual.empty());
}
