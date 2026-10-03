#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/PartyFigures.h"
#include "game/world/DynamicLights.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a shield armour is borne on the arm, reflecting before fire before lightning",
          "[game][screens][figures]") {
    CHECK(PartyFigures::shieldObjectOf(0).empty());
    CHECK(PartyFigures::shieldObjectOf(powerup::kLightningShield) == "L_SHLD");
    CHECK(PartyFigures::shieldObjectOf(powerup::kFireShield | powerup::kLightningShield) ==
          "FW_SHLD");
    CHECK(PartyFigures::shieldObjectOf(powerup::kReflectShield | powerup::kFireShield) ==
          "RF_SHLD");
}

TEST_CASE("in a dark level each standing player carries a lantern", "[game][screens][figures]") {
    std::array<PlayerRuntime, 2> players;
    CharacterSave red;
    red.color = 0;
    players[0].actor.spawn(0, red, nullptr, Vec3{1.0f, 2.0f, 3.0f}, 0.0f);
    players[1].actor.spawn(1, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    LevelInfo level;
    std::vector<PointLight> lights;
    PartyFigures::addLanterns(lights, players, &level);
    PartyFigures::addLanterns(lights, players, nullptr);
    CHECK(lights.empty()); // a lit level needs none

    level.flags = DynamicLights::kDarkLevel;
    players[1].life = PlayerLife::InTower;
    PartyFigures::addLanterns(lights, players, &level);
    REQUIRE(lights.size() == 1); // the fallen carry none
    CHECK(lights[0].position ==
          players[0].actor.followPoint() + Vec3{0.0f, DynamicLights::kLanternLift, 0.0f});
    CHECK(lights[0].color == DynamicLights::lantern(0));
    CHECK(lights[0].radius == DynamicLights::kLanternRadius);
    CHECK(lights[0].intensity == DynamicLights::kLanternIntensity);
}

TEST_CASE("a party without figures draws nothing", "[game][screens][figures]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    const PortalDeparture departure;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    const PartyFigures::Scene scene{.world = world, .weapons = weapons, .departure = departure};
    const PartyFigures figures;
    figures.draw(device, players, scene, Mat4{1.0f}, CameraFrame{});
    PartyFigures::drawShadows(device, players, scene, Mat4{1.0f}, Vec3{0.0f});
    CHECK(device.draws.empty());
}

TEST_CASE("the portal's skin, then the damage flash, then the chrome is worn",
          "[game][screens][figures][assets]") {
    const auto weaponsDirectory = test::assetOrSkip("WEAPONS/textures.ngc").parent_path();
    const auto powerupsDirectory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ItemArchive powerups;
    REQUIRE(weapons.load(weaponsDirectory));
    REQUIRE(powerups.load(powerupsDirectory));
    PartyFigures figures;
    figures.loadSkins(device, powerups, weapons);
    REQUIRE(figures.hitFlash() != nullptr);
    PlayerRuntime runtime;
    const PortalDeparture departure;
    PowerupEffects worn;
    CHECK(figures.skinOf(runtime, worn, departure) == nullptr);
    worn.armor = powerup::kInvulnerable;
    worn.invulnerableLeft = 20.0f;
    const Texture* silver = figures.skinOf(runtime, worn, departure);
    REQUIRE(silver != nullptr);
    worn.armor |= powerup::kGoldInvulnerable;
    const Texture* gold = figures.skinOf(runtime, worn, departure);
    REQUIRE(gold != nullptr);
    CHECK(gold != silver);
    runtime.hitFlashTicks = 2;
    CHECK(figures.skinOf(runtime, worn, departure) == figures.hitFlash());
    figures.clear();
    CHECK(figures.hitFlash() == nullptr);
    weapons.release();
    powerups.release();
}

TEST_CASE("a head gem appearing bursts once about its wearer", "[game][screens][figures][assets]") {
    const auto powerupsDirectory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(powerupsDirectory));
    REQUIRE(powerups.trees.find(HeadGem::kAppearEffect).has_value());
    REQUIRE(powerups.models.find(HeadGem::kHandOfDeath).has_value());
    REQUIRE(powerups.models.find(HeadGem::kHealthVamp).has_value());
    EffectTrees effects;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    PartyFigures::greetGems(device, players, powerups, effects);
    CHECK(effects.count() == 0);
    players[0].actor.save().progress().inventory.addPowerup(powerup::kSpecial,
                                                            powerup::kHandOfDeath, 0.0f, 30.0f);
    PartyFigures::greetGems(device, players, powerups, effects);
    CHECK(effects.count() == 1);
    CHECK(players[0].gem.shown() == HeadGem::kHandOfDeath);
    PartyFigures::greetGems(device, players, powerups, effects);
    CHECK(effects.count() == 1);
    effects.clear();
    powerups.release();
}

TEST_CASE("portal departure sinks and hides survivors without changing a dying teammate",
          "[game][screens][figures][assets][multiplayer]") {
    const auto root = test::assetOrSkip("WEAPONS/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    LevelWorld world;
    PortalDeparture departure;
    const PartyFigures::Scene scene{.world = world, .weapons = weapons, .departure = departure};
    const PartyFigures figures;
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players[0];
    const CharacterSave save;
    runtime.actor.spawn(3, save, nullptr, Vec3{20, 8, -5}, 0);
    runtime.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(runtime.figure);
    runtime.life = PlayerLife::Dying;
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
    const auto original = device.draws;
    REQUIRE_FALSE(original.empty());
    departure.begin(device, weapons.textures);
    REQUIRE(departure.skin() != nullptr);
    CHECK(figures.skinOf(runtime, PowerupEffects{}, departure) == nullptr);
    for (s32 phase = 0; phase < 2; ++phase) {
        departure.update(25);
        device.draws.clear();
        figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
        REQUIRE(device.draws.size() == original.size());
        for (usize i = 0; i < original.size(); ++i) {
            CHECK(device.draws[i].texture == original[i].texture);
            CHECK(device.draws[i].transform == original[i].transform);
            REQUIRE(device.draws[i].vertices.size() == original[i].vertices.size());
            for (usize v = 0; v < original[i].vertices.size(); ++v) {
                CHECK(device.draws[i].vertices[v].position == original[i].vertices[v].position);
            }
        }
    }
    REQUIRE(departure.finished());
    runtime.life = PlayerLife::Standing;
    device.draws.clear();
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
    CHECK(device.draws.empty());
    departure.begin(device, weapons.textures);
    departure.update(25);
    CHECK(figures.skinOf(runtime, PowerupEffects{}, departure) == departure.skin());
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().vertices.front().position !=
          original.front().vertices.front().position);
    runtime.life = PlayerLife::InTower;
    device.draws.clear();
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
    CHECK(device.draws.empty());
}

} // namespace
