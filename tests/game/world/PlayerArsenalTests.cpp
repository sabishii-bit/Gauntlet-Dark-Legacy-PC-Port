#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "game/world/PlayerArsenal.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    ItemArchive weapons;
    WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    PlayerActor actor;
    Fixture() {
        actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr});
    }
};

TEST_CASE("player arsenal throws the next potion from its owner's hand",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.actor.save().progress().inventory.addPotions(2, 1);
    f.actor.save().progress().inventory.addPotions(4, 1);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto& missile = f.arsenal.missiles().missile(0);
    REQUIRE(missile.owner == 3);
    REQUIRE(missile.potion == 4);
    REQUIRE(missile.position == Vec3{10, 4, 22});
    REQUIRE(missile.velocity.y == Approx(5 * 0.707f));
    REQUIRE(missile.velocity.z == Approx(5 * 0.707f));
    REQUIRE(missile.potency == Approx(0.75f * f.arsenal.magicPowerOf(f.actor)));
    REQUIRE(missile.model != nullptr);
    REQUIRE(f.actor.save().progress().inventory.nextPotion() == 2);
    f.arsenal.usePotion(f.actor);
    REQUIRE(f.actor.save().progress().inventory.potions.empty());
    REQUIRE(f.arsenal.missiles().count() == 1); // Immediate potion doesn't launch a bottle.
}

TEST_CASE("assisted weapon launch aims from its actual muzzle without retaining a lock",
          "[game][world][player-arsenal][target-assist]") {
    Fixture f;
    PlayerFigure figure;
    const Vec3 target{16, 7, 40};
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false, target);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto first = f.arsenal.missiles().missile(0);
    const Vec3 offset = target - first.position;
    const f32 time =
        std::hypot(offset.x, offset.z) / std::hypot(first.velocity.x, first.velocity.z);
    const Vec3 reached = first.position + first.velocity * time -
                         Vec3{0, 0.5f * first.spec->weight * time * time, 0};
    REQUIRE(glm::distance(reached, target) == Approx(0).margin(0.0001f));
    REQUIRE(first.velocity.x > 0);
    // No selected target preserves the ordinary forward throw (no sticky lock).
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false);
    REQUIRE(f.arsenal.missiles().count() == 2);
    REQUIRE(f.arsenal.missiles().missile(1).velocity.x == 0);
}

TEST_CASE("player arsenal tolerates missing artwork and clears projectiles before rebinding",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.arsenal.launchWeapon(f.actor, nullptr, Vec3{0, 0, 1}, 1, true);
    REQUIRE(f.arsenal.missiles().count() == 0);
    f.actor.save().progress().inventory.addPotions(1, 2);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr});
    REQUIRE(f.arsenal.missiles().count() == 0);
    REQUIRE(f.arsenal.missiles().takeImpacts().empty());
    f.arsenal.clear();
    f.arsenal.clear();
    f.arsenal.throwPotion(f.actor);
    f.arsenal.usePotion(f.actor);
    REQUIRE(f.actor.save().progress().inventory.potions.size() == 1);
    REQUIRE(f.arsenal.missiles().count() == 0);
}
} // namespace
