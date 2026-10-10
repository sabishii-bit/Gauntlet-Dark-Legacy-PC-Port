#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/PortalDeparture.h"
#include "game/screens/ReplicaCompanions.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/PlayerArsenal.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CompanionState stateOf(const CompanionVisual& visual) {
    return {visual.form,
            visual.placement,
            {0, visual.sequence, visual.generation, visual.frame, 1},
            visual.textureClock,
            visual.alpha};
}
void compare(std::span<const test::RecordedDraw> actual,
             std::span<const test::RecordedDraw> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        const auto& a = actual[i];
        const auto& b = expected[i];
        REQUIRE(a.vertices.size() == b.vertices.size());
        CHECK(a.texture == b.texture);
        CHECK(a.state.depthWrite == b.state.depthWrite);
        CHECK(a.state.depthTest == b.state.depthTest);
        CHECK(a.state.alphaTest == b.state.alphaTest);
        CHECK(a.blend() == b.blend());
        for (usize n = 0; n < a.vertices.size(); ++n) {
            const auto av = a.transform * Vec4{a.vertices[n].position, 1};
            const auto bv = b.transform * Vec4{b.vertices[n].position, 1};
            for (s32 axis = 0; axis < 3; ++axis) {
                CHECK(av[axis] == Approx(bv[axis]).margin(0.0001f));
            }
            CHECK(a.vertices[n].color == b.vertices[n].color);
            CHECK(a.vertices[n].uv.x == Approx(b.vertices[n].uv.x).margin(0.0001f));
            CHECK(a.vertices[n].uv.y == Approx(b.vertices[n].uv.y).margin(0.0001f));
        }
    }
}
struct Stock {
    std::filesystem::path root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    Stock() {
        REQUIRE(powerups.load(root / "POWERUPS"));
        REQUIRE(weapons.load(root / "WEAPONS"));
    }
};
CombatSnapshot snapshot(const PlayerRuntime& player, u64 tick = 0) {
    MotionSnapshot motion;
    motion.epoch = motion.cameraContinuity = 1;
    motion.tick = tick;
    const auto seat = static_cast<usize>(player.actor.player());
    motion.players[seat] = SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
    const Enemies enemies;
    const auto state = CombatCapture::capture(motion, std::span{&player, 1}, enemies);
    REQUIRE(state);
    const auto encoded = CombatPacket::encode(*state);
    REQUIRE(encoded);
    const auto decoded = CombatPacket::decode(*encoded);
    REQUIRE(decoded);
    return *decoded;
}

TEST_CASE("native familiar and Phoenix checkpoints match both passes at each earned tier",
          "[netplay][replica-companions][assets]") {
    Stock stock;
    const auto camera = CameraFrame::of(WorldCamera{{20, 25, 40}, 0.4f, 0.6f, 0});
    for (s32 character = 0; character < 16; ++character) {
        for (const s32 level : {30, 80}) {
            CAPTURE(character, level);
            PlayerRuntime player;
            CharacterSave save;
            save.character = character;
            save.color = 3;
            save.progress().experience = levelExperience(level);
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
            player.actor.spawn(2, save, nullptr, {3, 4, 5}, 0.6f);
            player.figure = PlayerFigure::load(stock.device, stock.root, save, false);
            REQUIRE(player.figure);
            CompanionResources resources;
            resources.bindEarned(*player.figure);
            REQUIRE(resources.bindPowerups(stock.device, stock.powerups, &stock.weapons));
            player.figure->setMelee({MeleeRange::Beyond, false, 0});
            bool attacked = false;
            for (u64 tick = 0; tick < 70; ++tick) {
                auto& inventory = player.actor.save().progress().inventory;
                if (tick == 50) {
                    inventory.powerups[0].strength = 0.4f;
                }
                if (tick == 60) {
                    inventory.powerups[0].on = false;
                }
                player.figure->setCompanionPowerups(stock.device, stock.powerups, inventory,
                                                    &stock.weapons);
                player.figure->animate(0, 1, 1.0f / 60, PlayerDeed::Attack);
                const auto state = snapshot(player, tick);
                const auto& companions = state.players[2]->companions;
                REQUIRE(companions[0]);
                REQUIRE(companions[1].has_value() == (tick < 60));
                attacked = attacked || (companions[1] && companions[0]->animation.sequence == 1 &&
                                        companions[1]->animation.sequence == 1);
                if (tick >= 50 && tick < 60) {
                    CHECK(companions[1]->alpha == Approx(0.4f));
                }
                const auto textures = stock.device.texturesCreated;
                const auto revision = player.figure->animationRevision();
                const auto body = PlayerFigure::bodyPlacement(player.actor.transform(), save, {});
                for (const auto pass : {TreeModel::Pass::DepthWriting, TreeModel::Pass::Effects}) {
                    stock.device.draws.clear();
                    player.figure->drawCompanions(stock.device, Mat4{1}, body, {}, 1, &camera, 1,
                                                  pass);
                    const auto expected = stock.device.draws;
                    for (s32 repeat = 0; repeat < 2; ++repeat) {
                        stock.device.draws.clear();
                        for (usize slot = 0; slot < companions.size(); ++slot) {
                            if (companions[slot]) {
                                REQUIRE(resources.accepts(slot, *companions[slot]));
                                resources.draw(stock.device, slot, *companions[slot], Mat4{1}, {},
                                               camera, pass);
                            }
                        }
                        compare(stock.device.draws, expected);
                    }
                }
                CHECK(stock.device.texturesCreated == textures);
                CHECK(player.figure->animationRevision() == revision);
            }
            CHECK(attacked);
            player.life = PlayerLife::InTower;
            const auto hidden = snapshot(player, 71);
            CHECK_FALSE(hidden.players[2]->companions[0]);
            CHECK_FALSE(hidden.players[2]->companions[1]);
        }
    }
}

TEST_CASE("native timed companions retain every authored sequence fade and material",
          "[netplay][replica-companions][assets]") {
    Stock stock;
    CompanionResources resources;
    REQUIRE(resources.bindPowerups(stock.device, stock.powerups, &stock.weapons));
    const auto camera = CameraFrame::at({10, 30, -20});
    const Mat4 placement =
        glm::rotate(glm::translate(Mat4{1}, Vec3{4, 10, 6}), 0.5f, Vec3{0, 1, 0});
    for (u32 kind = 1; kind <= 7; ++kind) {
        CAPTURE(kind);
        PowerupCompanion host;
        host.choose(stock.device, static_cast<PowerupCompanion::Kind>(kind), stock.powerups,
                    &stock.weapons);
        REQUIRE(host.shown());
        for (s32 tick = 0; tick < 90; ++tick) {
            constexpr std::array kActions{PlayerAnimator::Action::Run1,
                                          PlayerAnimator::Action::HitReact,
                                          PlayerAnimator::Action::Death};
            const auto action = kActions[static_cast<usize>(tick / 30)];
            host.update(1.0f / 60, action, tick == 12, tick == 12);
            const auto visual = host.visual(placement, 0.5f);
            REQUIRE(visual);
            const auto state = stateOf(*visual);
            REQUIRE(resources.accepts(1, state));
            stock.device.draws.clear();
            host.draw(stock.device, Mat4{1}, placement, {}, 0.5f, &camera, 1);
            const auto expected = stock.device.draws;
            const auto textures = stock.device.texturesCreated;
            stock.device.draws.clear();
            resources.draw(stock.device, 1, state, Mat4{1}, {}, camera, TreeModel::Pass::All);
            compare(stock.device.draws, expected);
            CHECK(stock.device.texturesCreated == textures);
            auto invalid = state;
            invalid.animation.sequence = 65535;
            CHECK_FALSE(resources.accepts(1, invalid));
            invalid = state;
            invalid.animation.frame = 1'000'000;
            CHECK_FALSE(resources.accepts(1, invalid));
            CHECK_FALSE(resources.accepts(0, state));
        }
        const auto old = host.visual(placement, 1);
        host.clear();
        host.choose(stock.device, static_cast<PowerupCompanion::Kind>(kind), stock.powerups,
                    &stock.weapons);
        REQUIRE(host.visual(placement, 1));
        CHECK(host.visual(placement, 1)->generation != old->generation);
    }
    ItemArchive missing;
    CHECK_FALSE(resources.bindPowerups(stock.device, missing, &stock.weapons));
    PowerupCompanion phoenix;
    phoenix.choose(stock.device, PowerupCompanion::Kind::Phoenix, stock.powerups, &stock.weapons);
    REQUIRE(resources.accepts(1, stateOf(*phoenix.visual(Mat4{1}, 1))));
}

TEST_CASE("companion interpolation holds births switches loops teleports and seat changes",
          "[netplay][replica-companions]") {
    CombatSnapshot first;
    first.motion.epoch = first.motion.cameraContinuity = 1;
    first.motion.players[0] = SeatMotion{1, 1, {}, 0};
    first.players[0] = PlayerCombatState{};
    CompanionState pet;
    pet.form = 1;
    pet.animation = {0, 0, 4, 2, 1};
    first.players[0]->companions[0] = pet;
    auto last = first;
    last.motion.tick = 2;
    auto& next = *last.players[0]->companions[0];
    next.placement[3].x = 10;
    next.animation.frame = 4;
    next.textureClock = 6;
    next.alpha = 0.2f;
    bool continuous = false;
    SECTION("ordinary motion") {
        continuous = true;
    }
    SECTION("different tier") {
        next.form = 2;
    }
    SECTION("restart or loop") {
        ++next.animation.generation;
    }
    SECTION("rewind") {
        next.animation.frame = 0;
    }
    SECTION("teleport") {
        ++last.motion.players[0]->continuity;
    }
    SECTION("new seat owner") {
        ++last.motion.players[0]->grant;
    }
    SECTION("birth") {
        first.players[0]->companions[0].reset();
    }
    SECTION("removal") {
        last.players[0]->companions[0].reset();
    }
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    for (const auto& state : {first, last}) {
        const auto packets = CombatReplica::packets(state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            playback.receive(1, packet);
        }
    }
    const auto middle = playback.sample(1);
    REQUIRE(middle);
    const auto& shown = middle->players[0]->companions[0];
    REQUIRE(shown.has_value() == first.players[0]->companions[0].has_value());
    if (shown) {
        CHECK(shown->placement[3].x == (continuous ? 5 : 0));
        CHECK(shown->animation.frame == (continuous ? 3 : 2));
        CHECK(shown->alpha == Approx(continuous ? 0.6f : 1));
    }
    REQUIRE(playback.sample(2));
    CHECK(CombatPacket::encode(*playback.sample(2)) == CombatPacket::encode(last));
}

TEST_CASE("companion capture follows posed head and back mounts and a sinking portal departure",
          "[netplay][replica-companions][assets]") {
    Stock stock;
    PlayerRuntime player;
    CharacterSave save;
    save.character = 5;
    save.color = 3;
    save.progress().experience = levelExperience(80);
    player.actor.spawn(1, save, nullptr, {2, 3, 4}, 0.8f);
    player.figure = PlayerFigure::load(stock.device, stock.root, save, false);
    REQUIRE(player.figure);
    CompanionResources resources;
    resources.bindEarned(*player.figure);
    REQUIRE(resources.bindPowerups(stock.device, stock.powerups, &stock.weapons));
    const auto camera = CameraFrame::at({15, 20, 30});
    for (const auto flag : {powerup::kFireBreath, powerup::kAcidBreath, powerup::kLightningBreath,
                            powerup::kLevitation}) {
        auto& inventory = player.actor.save().progress().inventory;
        inventory.powerups = {};
        inventory.addPowerup(powerup::kSpecial, flag, 0, 60);
        inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 60);
        player.figure->setCompanionPowerups(stock.device, stock.powerups, inventory,
                                            &stock.weapons);
        PortalDeparture departure;
        departure.begin(stock.device, stock.weapons.textures);
        MotionSnapshot motion;
        motion.epoch = motion.cameraContinuity = 1;
        motion.players[1] = SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
        const Enemies enemies;
        for (s32 tick = 0; tick <= PortalDeparture::kTicks; tick += 5) {
            player.figure->animate(1, 5, 5.0f / 60);
            const auto state =
                CombatCapture::capture(motion, std::span{&player, 1}, enemies, &departure);
            REQUIRE(state);
            const auto& companions = state->players[1]->companions;
            if (departure.finished()) {
                CHECK_FALSE(companions[0]);
                CHECK_FALSE(companions[1]);
            } else {
                REQUIRE(companions[0]);
                REQUIRE(companions[1]);
                const auto worn = PowerupEffects::of(inventory);
                const auto body = PlayerFigure::bodyPlacement(
                    departure.transform(player.actor.transform()), player.actor.save(), worn);
                stock.device.draws.clear();
                player.figure->drawCompanions(stock.device, Mat4{1}, body, {}, 1, &camera, 1,
                                              TreeModel::Pass::All);
                const auto expected = stock.device.draws;
                stock.device.draws.clear();
                for (usize slot = 0; slot < companions.size(); ++slot) {
                    resources.draw(stock.device, slot, *companions[slot], Mat4{1}, {}, camera,
                                   TreeModel::Pass::All);
                }
                compare(stock.device.draws, expected);
            }
            departure.update(5);
        }
    }
}

TEST_CASE(
    "both familiar projectiles survive checkpoints without client release or impact callbacks",
    "[netplay][replica-companions][assets]") {
    Stock stock;
    ClassDataSet classes;
    REQUIRE(classes.load(stock.root / "PDATA"));
    const WorldCollision collision;
    LevelSoundscape audio;
    EffectTrees effects;
    for (const s32 character : {1, 5, 7}) {
        CAPTURE(character);
        CharacterSave save;
        save.character = character;
        save.color = 3;
        save.progress().experience = levelExperience(80);
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        PlayerActor actor;
        actor.spawn(0, save, nullptr, {}, 0);
        auto figure = PlayerFigure::load(stock.device, stock.root, save, false);
        REQUIRE(figure);
        REQUIRE(figure->effects() != nullptr);
        PlayerArsenal arsenal;
        const std::array lenders{&stock.weapons.textures};
        arsenal.bind({stock.device, classes, stock.weapons, collision, effects, audio, nullptr, {}},
                     lenders);
        ProjectileResources resources;
        REQUIRE(resources.addTree(1, *figure->effects(), "FAMILIAR_SPIT", stock.device, lenders));
        REQUIRE(resources.addTree(2, stock.weapons, "PHOENIX_FBALL", stock.device, lenders));
        CHECK_FALSE(resources.addTree(1, stock.weapons, "PHOENIX_FBALL", stock.device, lenders));
        CHECK_FALSE(resources.addTree(3, stock.weapons, "MISSING", stock.device, lenders));
        arsenal.launchFamiliar(actor, figure.get(), Vec3{0, 0, 60});
        REQUIRE(arsenal.missiles().count() == 2);
        CHECK_FALSE(arsenal.missiles().missile(0).breaksPotions);
        CHECK_FALSE(arsenal.missiles().missile(1).breaksPotions);
        ReplicaProjectiles replica;
        REQUIRE(replica.begin(1));
        const EnemyMissiles enemies;
        const auto camera = CameraFrame::at({15, 30, -15});
        for (u64 tick = 0; tick < 25; ++tick) {
            arsenal.missiles().update(1.0f / 60, nullptr, {});
            CombatSnapshot state;
            state.motion.epoch = state.motion.cameraContinuity = 1;
            state.motion.tick = tick;
            REQUIRE(ProjectileCapture::append(state, resources, arsenal.missiles(), enemies));
            REQUIRE(state.projectiles.size() == 2);
            REQUIRE(state.projectiles[0].resource != state.projectiles[1].resource);
            const auto bytes = CombatPacket::encode(state);
            REQUIRE(bytes);
            const auto received = CombatPacket::decode(*bytes);
            REQUIRE(received);
            REQUIRE(replica.show(*received, resources));
            stock.device.draws.clear();
            arsenal.missiles().draw(stock.device, Mat4{1}, {}, &camera);
            const auto expected = stock.device.draws;
            REQUIRE_FALSE(expected.empty());
            const auto textures = stock.device.texturesCreated;
            for (s32 repeat = 0; repeat < 2; ++repeat) {
                stock.device.draws.clear();
                replica.draw(stock.device, resources, Mat4{1}, {}, camera);
                compare(stock.device.draws, expected);
            }
            CHECK(stock.device.texturesCreated == textures);
            CHECK(arsenal.missiles().takeImpacts().empty());
            CHECK_FALSE(figure->familiarReleased());
        }
        arsenal.clear();
    }
}
} // namespace
