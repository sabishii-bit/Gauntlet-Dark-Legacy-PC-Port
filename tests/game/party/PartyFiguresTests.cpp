#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/DynamicLights.h"
#include "game/level/LevelCatalog.h"
#include "game/party/PartyFigures.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PlayerDeeds.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("held figures do not oscillate their final animated pose during a scene cut",
          "[game][screens][figures][presentation]") {
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players.front();
    runtime.figure = std::make_unique<PlayerFigure>();
    PartyFigures::snapshot(players);
    runtime.figure->animate(0, 1, 1.0f / 60);
    CHECK(PartyFigures::presentationBlend(runtime, 0.25f) == 0.25f);
    PartyFigures::snapshot(players);
    CHECK(PartyFigures::presentationBlend(runtime, 0.25f) == 1);
    CHECK(PartyFigures::presentationBlend(runtime, 0.75f) == 1);
}

TEST_CASE("drawing a moving player samples completed states without changing collision",
          "[game][screens][figures][cadence]") {
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players.front();
    runtime.actor.spawn(0, CharacterSave{}, nullptr, Vec3{0}, 0);
    PartyFigures::snapshot(players);
    runtime.actor.place(Vec3{0.25f, 0, 0});
    const Mat4 halfway = PartyFigures::presentationBody(runtime, 0.5f);
    CHECK(halfway[3].x == Catch::Approx(0.125f));
    CHECK(runtime.actor.position().x == 0.25f);
    CHECK(PartyFigures::presentationBody(runtime, 0)[3].x == 0);
    CHECK(PartyFigures::presentationBody(runtime, 1)[3].x == 0.25f);
    // Relocation never draws a trail through the intervening walls.
    runtime.actor.place(Vec3{100, 0, 0});
    CHECK(PartyFigures::presentationBody(runtime, 0.5f)[3].x == 100);
    PartyFigures::snapshot(players);
    REQUIRE(runtime.transport.begin(Vec3{50, 0, 0}));
    runtime.actor.place(Vec3{100.25f, 0, 0});
    CHECK(PartyFigures::presentationBody(runtime, 0.5f)[3].x == 100.25f);
}

TEST_CASE("a shield armour is borne on the arm, reflecting before fire before lightning",
          "[game][screens][figures]") {
    CHECK(PartyFigures::shieldObjectOf(0).empty());
    CHECK(PartyFigures::shieldObjectOf(armor::kElectricArmor) == "L_SHLD");
    CHECK(PartyFigures::shieldObjectOf(armor::kFireArmor | armor::kElectricArmor) == "FW_SHLD");
    CHECK(PartyFigures::shieldObjectOf(armor::kReflect | armor::kFireArmor) == "RF_SHLD");
}

TEST_CASE("who is it takes the sign off only while dying, falling, getting up or whirled",
          "[game][screens][figures][it]") {
    // DoPlayerAction marks these rows with hud flag 2 (0x800AD6E0), and the victory pose's
    // 123, which no action here plays.
    constexpr std::array<u8, 6> kDownRows{126, 131, 132, 133, 134, 135};
    for (usize i = 0; i < PlayerAnimator::kActionCount; ++i) {
        CAPTURE(PlayerAnimator::kSequenceNames[i]);
        const bool down = std::ranges::contains(kDownRows, PlayerAnimator::kNativeActions[i]);
        CHECK(PartyFigures::wearsItSign(static_cast<PlayerAnimator::Action>(i)) == !down);
    }
    CHECK_FALSE(PartyFigures::wearsItSign(PlayerAnimator::Action::FallForward));
    CHECK(PartyFigures::wearsItSign(PlayerAnimator::Action::HitReact));
}

TEST_CASE("who is it wears the sign of an ordinary level's items, but not while down",
          "[game][screens][figures][it][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G3")));
    // G3, where IT is placed, has no item archive of its own: the realm's is the level's.
    REQUIRE_FALSE(world.realmItems().loaded());
    const auto object = world.items().models.find("IT_SIGN");
    REQUIRE(object);
    const auto& sign = world.items().models.mesh(*object);
    ItemArchive weapons;
    const PortalDeparture departure;
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players[0];
    runtime.actor.spawn(0, CharacterSave{}, nullptr, Vec3{5, 2, 9}, 0);
    runtime.figure = PlayerFigure::load(device, root, runtime.actor.save(), Entrance::Skip);
    REQUIRE(runtime.figure);
    const PartyFigures::Scene scene{.world = world, .weapons = weapons, .departure = departure};
    const PartyFigures figures;
    const auto signDrawn = [&] {
        const Mat4 body = runtime.figure->placement(
            PartyFigures::presentationBody(runtime, 1), runtime.actor.save(),
            PowerupEffects::of(runtime.actor.save().progress().inventory));
        device.draws.clear();
        figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
        REQUIRE_FALSE(device.draws.empty());
        return std::ranges::any_of(device.draws, [&](const test::RecordedDraw& draw) {
            return !draw.vertices.empty() &&
                   std::ranges::any_of(sign.vertices, [&](const auto& vertex) {
                       return glm::distance(draw.vertices.front().position,
                                            Vec3{body * Vec4{vertex.position, 1}}) < 0.0001f;
                   });
        });
    };
    CHECK_FALSE(signDrawn()); // nobody is it yet
    runtime.itTicks = 1;
    CHECK(signDrawn());
    runtime.figure->animate(0, 1, 1.0f / 60, PlayerDeed::FallBack);
    REQUIRE(runtime.figure->animator().action() == PlayerAnimator::Action::FallBack);
    CHECK_FALSE(signDrawn());
    // Up again, it wears the sign again.
    for (s32 tick = 0;
         tick < 600 && !PartyFigures::wearsItSign(runtime.figure->animator().action()); ++tick) {
        runtime.figure->animate(0, 1, 1.0f / 60);
    }
    REQUIRE(PartyFigures::wearsItSign(runtime.figure->animator().action()));
    CHECK(signDrawn());
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

TEST_CASE("an invisible player is drawn among the translucent bodies, after the solids",
          "[game][screens][figures][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    const PortalDeparture departure;
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players[0];
    runtime.actor.spawn(0, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    runtime.figure = PlayerFigure::load(device, root, runtime.actor.save(), Entrance::Skip);
    REQUIRE(runtime.figure);
    const PartyFigures::Scene scene{.world = world, .weapons = weapons, .departure = departure};
    const PartyFigures figures;
    figures.draw(device, players, scene, Mat4{1.0f}, CameraFrame{});
    const auto solid = device.draws.size();
    CHECK(solid > 0);
    figures.draw(device, players, scene, Mat4{1.0f}, CameraFrame{},
                 PartyFigures::Bodies::Translucent);
    CHECK(device.draws.size() == solid); // a solid body is never drawn twice

    device.draws.clear();
    runtime.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kInvisible,
                                                         0.0f, 10.0f);
    REQUIRE(PowerupEffects::of(runtime.actor.save().progress().inventory).bodyAlpha() < 1);
    figures.draw(device, players, scene, Mat4{1.0f}, CameraFrame{});
    CHECK(device.draws.empty());
    figures.draw(device, players, scene, Mat4{1.0f}, CameraFrame{},
                 PartyFigures::Bodies::Translucent);
    REQUIRE_FALSE(device.draws.empty());
    // Faded, it still writes depth, so the level's sorted scenery stays behind it.
    CHECK(device.draws.front().state.depthWrite);
    CHECK(
        std::ranges::all_of(device.draws, [](const auto& draw) { return draw.state.depthWrite; }));
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

TEST_CASE("Mikey is drawn independently of its owner's figure and disappears on dismissal",
          "[game][screens][figures][mikey][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/objects.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(directory));
    LevelWorld world;
    ItemArchive weapons;
    const PortalDeparture departure;
    std::array<PlayerRuntime, 1> players;
    auto& runtime = players.front();
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 120);
    inventory.powerups.front().on = true;
    runtime.mikey.update(1.0f / 30, inventory, Vec3{1, 2, 3});
    EffectTrees effects;
    PartyFigures::updateDecoys(device, players, powerups, 1.0f / 30, effects);
    REQUIRE(runtime.mikeyFigure != nullptr);
    REQUIRE(runtime.mikeyFigure->shown());
    REQUIRE(runtime.figure == nullptr);
    const PartyFigures figures;
    const PartyFigures::Scene scene{.world = world, .weapons = weapons, .departure = departure};
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
    CHECK_FALSE(device.draws.empty());
    runtime.mikey.clear();
    PartyFigures::updateDecoys(device, players, powerups, 1.0f / 30, effects);
    device.draws.clear();
    figures.draw(device, players, scene, Mat4{1}, CameraFrame{});
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
    worn.armor = armor::kInvulnerable;
    worn.invulnerableLeft = 20.0f;
    const Texture* silver = figures.skinOf(runtime, worn, departure);
    REQUIRE(silver != nullptr);
    worn.armor |= armor::kGoldInvulnerable;
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
    runtime.figure = PlayerFigure::load(device, root, save, Entrance::Skip);
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
