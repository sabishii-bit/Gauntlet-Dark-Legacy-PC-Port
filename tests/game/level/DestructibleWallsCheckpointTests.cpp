#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/level/DestructibleWallsCheckpoint.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldCollision collision;
    DestructibleWalls walls;
    DestructibleWallsCheckpoint owner{walls};
    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":1,
             "radius":3,"height":6,"collisionOffset":[0,2,0]},
            {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":-1,
             "radius":3,"height":6}],
          "itemInstances":[
            {"info":0,"name":"WALL","minPlayers":2,"position":[10,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]},
            {"info":1,"name":"WALL","position":[30,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]},
            {"info":0,"name":"MISSING","position":[50,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        walls.bind(device, layout, models, textures, collision);
        REQUIRE(walls.size() == 3);
        REQUIRE_FALSE(walls.wall(2).model.bound());
    }
    DestructibleWallsCheckpoint::State state() const {
        const auto result = owner.checkpoint();
        REQUIRE(result);
        return *result;
    }
    std::optional<s32> step(s32 tick, f32 seconds) {
        std::optional<s32> hit;
        if (tick == 0) {
            REQUIRE_FALSE(walls.strike(0, 100, collision)); // Not shown for one player.
            REQUIRE_FALSE(walls.strike(1, 100, collision)); // Negative armour.
            walls.setPlayerCount(2, collision);
            REQUIRE_FALSE(walls.strike(0, 100, collision, damage::kGas));
            walls.reveal(0);
        } else if (tick == 1) {
            hit = walls.strike(0, 10, collision);
            REQUIRE(hit == 16);
        } else if (tick == 3) {
            hit = walls.strike(0, 0.5f, collision);
            REQUIRE(hit == 15);
        } else if (tick == 140) {
            hit = walls.strike(0, 16, collision);
            REQUIRE(hit == 0);
        } else if (tick > 140) {
            REQUIRE_FALSE(walls.strike(0, 100, collision));
        }
        walls.update(seconds);
        return hit;
    }
};

TEST_CASE("wall checkpoints resume native reveal damage collision and stable target storage",
          "[walls-checkpoint]") {
    Fixture host("walls-checkpoint-host");
    Fixture guest("walls-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    const auto originalTarget = host.walls.target(0, 6000);
    const auto* body = &host.walls.wall(0);
    const auto* model = &host.walls.wall(0).model;
    bool blinkBoundary = false;
    bool wentDark = false;
    bool destroyed = false;
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = host.state();
        const auto collisionBefore = host.collision;
        REQUIRE(guest.owner.restore(before));
        guest.collision = collisionBefore; // The enclosing owner restores this independently.
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        const auto hit = host.step(tick, seconds);
        const auto expected = host.state();
        const bool solidAfter = host.collision.solid(host.walls.wall(0).object);
        CHECK(guest.step(tick, seconds) == hit);
        CHECK(guest.state() == expected);
        CHECK(guest.collision.solid(guest.walls.wall(0).object) == solidAfter);
        blinkBoundary |= expected.walls[0].white && expected.walls[0].flash == 0 &&
                         expected.walls[0].blinkTicks >= DestructibleWalls::kBlinkWhite;
        wentDark |= tick > 3 && !expected.walls[0].white;
        destroyed |= expected.walls[0].health == 0;
        host.device.draws.clear();
        host.walls.draw(host.device, Mat4{1}, {});
        const auto draws = host.device.draws;
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(host.state() == before);
        CHECK(host.collision.solid(host.walls.wall(0).object) == solidAfter);
        CHECK(&host.walls.wall(0) == body);
        CHECK(&host.walls.wall(0).model == model);
        CHECK(host.walls.target(0, 6000).surface.data() == originalTarget.surface.data());
        CHECK(host.walls.target(0, 6000).surface.size() == originalTarget.surface.size());
        host.collision = collisionBefore;
        CHECK(host.step(tick, seconds) == hit);
        CHECK(host.state() == expected);
        host.device.draws.clear();
        host.walls.draw(host.device, Mat4{1}, {});
        REQUIRE(host.device.draws.size() == draws.size());
        for (usize draw = 0; draw < draws.size(); ++draw) {
            CHECK(host.device.draws[draw].texture == draws[draw].texture);
            CHECK(host.device.draws[draw].vertices == draws[draw].vertices);
            CHECK(host.device.draws[draw].transform == draws[draw].transform);
            CHECK(host.device.draws[draw].state == draws[draw].state);
        }
    }
    CHECK(blinkBoundary);
    CHECK(wentDark);
    CHECK(destroyed);
    CHECK(host.walls.wall(1).health == 25);
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("wall checkpoints preserve the applied white mask rather than inferring from timers",
          "[walls-checkpoint]") {
    Fixture fixture("walls-checkpoint-white-boundary");
    fixture.walls.setPlayerCount(2, fixture.collision);
    fixture.walls.reveal(0);
    fixture.walls.update(0.5f);
    const auto white = fixture.state();
    REQUIRE(white.walls[0].flash == 0);
    REQUIRE(white.walls[0].blinkTicks == 30);
    REQUIRE(white.walls[0].white);
    fixture.device.draws.clear();
    fixture.walls.draw(fixture.device, Mat4{1}, {});
    const auto expected = fixture.device.draws;
    fixture.walls.update(0);
    REQUIRE_FALSE(fixture.walls.wall(0).white);
    REQUIRE(fixture.owner.restore(white));
    fixture.device.draws.clear();
    fixture.walls.draw(fixture.device, Mat4{1}, {});
    REQUIRE(fixture.device.draws.size() == expected.size());
    for (usize index = 0; index < expected.size(); ++index) {
        CHECK(fixture.device.draws[index].texture == expected[index].texture);
        CHECK(fixture.device.draws[index].vertices == expected[index].vertices);
        CHECK(fixture.device.draws[index].state == expected[index].state);
    }
}

TEST_CASE("wall checkpoint plans reject malformed surfaces and stale binding transactions",
          "[walls-checkpoint]") {
    Fixture fixture("walls-checkpoint-fences");
    const auto before = fixture.state();
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    switch (variant) {
    case 0: invalid.walls[0].source.instance.position.x += 1; break;
    case 1: invalid.walls[0].surface[0].vertices[0].x += 1; break;
    case 2: invalid.walls[0].bounds.radius += 1; break;
    case 3: invalid.walls[0].blinkTicks = std::numeric_limits<f32>::quiet_NaN(); break;
    case 4: invalid.walls[0].health = 100; break;
    case 5: invalid.walls[0].blinks = 4; break;
    case 6: invalid.walls[0].model = false; break;
    case 7: invalid.walls[0].object += 1; break;
    default: break;
    }
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.state() == before);
    auto first = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    REQUIRE(sibling);
    DestructibleWallsCheckpoint other(fixture.walls);
    CHECK_FALSE(other.commitRestore(std::move(*first)));
    REQUIRE(fixture.owner.commitRestore(std::move(*first)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    first = fixture.owner.prepareRestore(before);
    REQUIRE(first);
    fixture.walls.clear();
    CHECK_FALSE(fixture.owner.canCommit(*first));
}

TEST_CASE("native breakaway wall checkpoints retain baked lighting and exact geometry",
          "[walls-checkpoint][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELG4/WORLDS.PS2").parent_path();
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldCollision collision;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(collision.load(directory, layout));
    DestructibleWalls walls;
    walls.bind(device, layout, models, textures, collision);
    REQUIRE(walls.size() == 13);
    DestructibleWallsCheckpoint owner(walls);
    const auto uploads = device.texturesCreated;
    for (usize index = 0; index < walls.size(); ++index) {
        CAPTURE(index);
        REQUIRE(walls.strike(index, 1, collision));
        const auto before = owner.checkpoint();
        REQUIRE(before);
        device.draws.clear();
        walls.draw(device, Mat4{1}, {});
        const auto expected = device.draws;
        walls.update(1.0f / 30);
        REQUIRE(owner.restore(*before));
        CHECK(owner.checkpoint() == before);
        device.draws.clear();
        walls.draw(device, Mat4{1}, {});
        REQUIRE(device.draws.size() == expected.size());
        for (usize draw = 0; draw < expected.size(); ++draw) {
            CHECK(device.draws[draw].texture == expected[draw].texture);
            CHECK(device.draws[draw].vertices == expected[draw].vertices);
            CHECK(device.draws[draw].state == expected[draw].state);
        }
    }
    CHECK(device.texturesCreated == uploads);
}
TEST_CASE("native wall history preserves collision ownership stable surfaces and applied masks",
          "[local-rewind][walls-local-rewind]") {
    Fixture reference("walls-native-reference");
    Fixture corrected("walls-native-corrected");
    struct Snapshot {
        DestructibleWalls::LocalSnapshot walls;
        WorldCollision::LocalSnapshot collision;
    };
    LocalStateHistory<Snapshot> history;
    std::array<DestructibleWalls::LocalSnapshot, 8> expected;
    DestructibleWalls::LocalSnapshot observed;
    const auto target = corrected.walls.target(0, 6000);
    const auto* model = &corrected.walls.wall(0).model;
    const auto uploads = corrected.device.texturesCreated;
    s32 tick = 0;
    for (s32 window = 0; window < 64; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(static_cast<u64>(tick), [&](auto& saved) {
            corrected.walls.saveLocal(saved.walls);
            corrected.collision.saveLocal(saved.collision);
            return true;
        }));
        const auto before = corrected.state();
        auto stale = corrected.owner.prepareRestore(before);
        REQUIRE(stale);
        const s32 distance = 1 + window % 8;
        const auto* saved = history.find(static_cast<u64>(tick));
        REQUIRE(saved);
        for (s32 offset = 0; offset < distance; ++offset) {
            corrected.step(tick + offset, 1.0f / 60);
            corrected.walls.saveLocal(expected[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.walls.restoreLocal(saved->walls));
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        std::vector<std::optional<s32>> hits;
        for (s32 offset = 0; offset < distance; ++offset) {
            hits.push_back(reference.step(tick + offset, 1.0f / 60));
            corrected.walls.setPlayerCount(4, corrected.collision);
            corrected.walls.strike(0, 1000, corrected.collision);
            corrected.walls.update(0.51f);
        }
        const bool solid = corrected.collision.solid(corrected.walls.wall(0).object);
        REQUIRE(corrected.walls.restoreLocal(saved->walls));
        CHECK(corrected.collision.solid(corrected.walls.wall(0).object) == solid);
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        CHECK_FALSE(corrected.owner.canCommit(*stale));
        CHECK(corrected.state() == before);
        for (s32 offset = 0; offset < distance; ++offset) {
            CHECK(corrected.step(tick + offset, 1.0f / 60) == hits[static_cast<usize>(offset)]);
            corrected.walls.saveLocal(observed);
            CHECK(corrected.walls.equalLocal(observed, expected[static_cast<usize>(offset)]));
        }
        CHECK(corrected.state() == reference.state());
        CHECK(corrected.collision.solid(corrected.walls.wall(0).object) ==
              reference.collision.solid(reference.walls.wall(0).object));
        CHECK(corrected.walls.target(0, 6000).surface.data() == target.surface.data());
        CHECK(&corrected.walls.wall(0).model == model);
        reference.device.draws.clear();
        corrected.device.draws.clear();
        reference.walls.draw(reference.device, Mat4{1}, {});
        corrected.walls.draw(corrected.device, Mat4{1}, {});
        REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
        for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
            CHECK(corrected.device.draws[draw].vertices == reference.device.draws[draw].vertices);
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == uploads);
    DestructibleWalls::LocalSnapshot saved;
    corrected.walls.saveLocal(saved);
    CHECK_FALSE(reference.walls.restoreLocal(saved));
    CHECK_FALSE(reference.walls.equalLocal(saved, saved));
    corrected.walls.clear();
    CHECK_FALSE(corrected.walls.restoreLocal(saved));
    CHECK_FALSE(corrected.walls.equalLocal(saved, saved));
    corrected.walls.bind(corrected.device, corrected.layout, corrected.models, corrected.textures,
                         corrected.collision);
    CHECK_FALSE(corrected.walls.restoreLocal(saved));
}
TEST_CASE("native wall equality includes pending reveal blinks before the model changes",
          "[local-rewind][walls-local-rewind]") {
    Fixture fixture("walls-native-hidden-reveal");
    DestructibleWalls::LocalSnapshot before;
    DestructibleWalls::LocalSnapshot after;
    DestructibleWalls::LocalSnapshot restored;
    fixture.walls.setPlayerCount(2, fixture.collision);
    fixture.walls.saveLocal(before);
    const auto health = fixture.walls.wall(0).health;
    const auto transform = fixture.walls.wall(0).transform;
    const auto white = fixture.walls.wall(0).white;
    REQUIRE(fixture.walls.standing(0));
    fixture.walls.reveal(0);
    fixture.walls.saveLocal(after);
    CHECK(fixture.walls.wall(0).health == health);
    CHECK(fixture.walls.wall(0).transform == transform);
    CHECK(fixture.walls.wall(0).white == white);
    CHECK_FALSE(fixture.walls.equalLocal(before, after));
    REQUIRE(fixture.walls.restoreLocal(before));
    fixture.walls.saveLocal(restored);
    CHECK(fixture.walls.equalLocal(before, restored));
}
} // namespace
