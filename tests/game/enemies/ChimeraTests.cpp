#include <algorithm>
#include <array>
#include <set>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/CombatantFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct ChimeraFixture {
    test::FakeRenderDevice renderer;
    test::CombatantFixture fight;
    std::array<EnemyView, 1> players;
    ChimeraFixture() {
        const auto root = test::unpackedOrSkip("critter/CHIMERA.json").parent_path().parent_path();
        test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
        fight.open(renderer, root, nullptr, {}, 'A');
        REQUIRE(fight.spawn("CHIMERA", Vec3{0}, 0));
        players[0].player = 0;
        players[0].position = Vec3{0, 0, 40};
        players[0].height = 6;
        players[0].radius = 1;
    }
    void step() { fight.update(2, 1.0f / 30.0f, players); }
};

TEST_CASE("Chimera loads and runs all three head move tables", "[game][chimera][unpacked]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    REQUIRE(body.childCount() == 3);
    REQUIRE(body.child(1)->maxHealth() == 1200);
    REQUIRE(body.child(2)->maxHealth() == 1500);
    REQUIRE(body.child(3)->maxHealth() == 1200);
    for (s32 id = 1; id <= 3; ++id) {
        REQUIRE(body.child(id)->data()->moves().size() == 15);
        REQUIRE(body.child(id)->moveType() == -1); // START is copied from the body.
    }
    std::set<s32> shooters;
    std::set<std::string> effects;
    bool sync = false;
    for (s32 frame = 0; frame < 3600; ++frame) {
        // Move the target between projectile and close-combat ranges, not an idle launch.
        fixture.players[0].position = Vec3{0, 0, frame < 1800 ? 40.0f : 18.0f};
        fixture.step();
        sync |= body.moveType() == 1;
        for (const auto& shot : body.takeShots()) {
            if (shot.critter > 0) {
                shooters.insert(shot.critter);
                REQUIRE(shot.data == body.child(shot.critter)->data());
                REQUIRE(shot.target.has_value());
                REQUIRE(glm::distance(shot.origin, body.position()) > 1);
            }
        }
        for (const auto& cue : body.takeCues()) {
            if (cue.critter > 0 && !cue.tree.empty()) {
                effects.insert(cue.tree);
            }
        }
        body.takeBlows();
    }
    REQUIRE(shooters == std::set<s32>{1, 2, 3});
    REQUIRE(sync);
    REQUIRE_FALSE(effects.empty());
}

TEST_CASE("Chimera routes head damage and shares body damage without applying armor twice",
          "[game][chimera][unpacked]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    EnemyHit hit;
    hit.damage = 62; // TYPE armor is two; sixty reaches the selected creature.
    body.hurt(hit);
    REQUIRE(body.health() == Approx(5940));
    REQUIRE(body.child(1)->health() == Approx(1190));
    REQUIRE(body.child(2)->health() == Approx(1490));
    REQUIRE(body.child(3)->health() == Approx(1190));
    body.hurt(hit, 2);
    REQUIRE(body.health() == Approx(5880));
    REQUIRE(body.child(2)->health() == Approx(1430));
    REQUIRE(body.child(1)->health() == Approx(1190));
    hit.damage = 2000;
    body.draw(fixture.renderer, Mat4{1}, {});
    const usize beforeDraws = fixture.renderer.draws.size();
    body.hurt(hit, 2);
    REQUIRE_FALSE(body.child(2)->alive());
    REQUIRE(body.alive());
    REQUIRE(body.health() == Approx(5880)); // Lethal child hits do not forward.
    const auto targets = body.bodyTargets();
    REQUIRE(std::ranges::none_of(targets, [](const auto& t) { return t.id == 2; }));
    REQUIRE(std::ranges::any_of(targets, [](const auto& t) { return t.id == 1; }));
    bool stump = false;
    for (s32 frame = 0; frame < 180; ++frame) {
        fixture.step();
        for (const auto& cue : body.takeCues()) {
            if (cue.tree == "STUMPL") {
                REQUIRE_FALSE(stump);
                stump = true;
                REQUIRE(cue.critter == 2);
                REQUIRE(cue.node == "BODY1_LION");
                REQUIRE(cue.placement.has_value());
            }
        }
        for (const auto& shot : body.takeShots()) {
            REQUIRE(shot.critter != 2);
        }
    }
    REQUIRE(stump);
    REQUIRE(body.child(2)->dying());
    fixture.renderer.draws.clear();
    body.draw(fixture.renderer, Mat4{1}, {});
    REQUIRE_FALSE(fixture.renderer.draws.empty());
    REQUIRE(fixture.renderer.draws.size() < beforeDraws);
}

TEST_CASE("Chimera scimitar removes the lion only on impact, without subtracting body health",
          "[game][chimera][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/CHIMERA.json").parent_path().parent_path();
    test::FakeRenderDevice renderer;
    Bosses bosses;
    bosses.open(renderer, root, nullptr, {}, 'A');
    REQUIRE(bosses.spawn(35, Vec3{0}, 0));
    REQUIRE(bosses.bringLegend(0));
    const f32 health = bosses.view().health;
    EnemyView player;
    player.player = 0;
    player.position = Vec3{0, 0, 40};
    const std::array players{player};
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(2, 1.0f / 30.0f, players);
        bosses.takeCues();
    }
    REQUIRE(bosses.legend().thrown());
    const auto before = bosses.targets();
    REQUIRE(std::ranges::any_of(before, [](const auto& target) { return target.id == 2; }));
    REQUIRE(bosses.view().health == health);
    bosses.landLegend();
    const auto after = bosses.targets();
    REQUIRE(std::ranges::none_of(after, [](const auto& target) { return target.id == 2; }));
    REQUIRE(std::ranges::any_of(after, [](const auto& target) { return target.id == 1; }));
    REQUIRE(std::ranges::any_of(after, [](const auto& target) { return target.id == 3; }));
    REQUIRE(bosses.view().health == health);
    const auto losses = bosses.takeLosses();
    REQUIRE(std::ranges::count_if(losses, [](const auto& loss) { return loss.killed; }) == 1);
    bosses.landLegend();
    REQUIRE(bosses.takeLosses().empty());
    bool stump = false;
    for (s32 frame = 0; frame < 90; ++frame) {
        bosses.update(2, 1.0f / 30.0f, players);
        for (const auto& cue : bosses.takeCues()) {
            REQUIRE(cue.tree != "STUMPL");
            stump |= cue.tree == "STUMPLQ";
        }
    }
    REQUIRE(stump);
}

TEST_CASE("Chimera shared death stops surviving heads and emits only body reward",
          "[game][chimera][unpacked]") {
    ChimeraFixture fixture;
    auto& body = fixture.fight.actor;
    for (s32 frame = 0; frame < 300; ++frame) {
        fixture.step();
        body.takeShots();
        body.takeCues();
        body.takeBlows();
    }
    EnemyHit hit;
    hit.damage = 10000;
    body.hurt(hit);
    REQUIRE(body.dying());
    s32 rewards = 0;
    for (s32 frame = 0; frame < 900 && body.present(); ++frame) {
        fixture.step();
        REQUIRE(body.takeShots().empty());
        REQUIRE(body.takeBlows().empty());
        for (const auto& spew : body.takeSpews()) {
            REQUIRE(spew.critter == 0);
            ++rewards;
        }
        body.takeCues();
    }
    REQUIRE_FALSE(body.present());
    REQUIRE(rewards == 1);
}
} // namespace
