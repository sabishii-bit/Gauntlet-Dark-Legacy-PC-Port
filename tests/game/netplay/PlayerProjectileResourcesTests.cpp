#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/PlayerArsenal.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("player projectile preloading is atomic and supports four independently owned seats",
          "[netplay][player-projectile-resources][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    const std::array<TextureSet*, 1> lenders{&weapons.textures};
    ProjectileResources resources;
    REQUIRE(resources.addStreak(40000, device.whiteTexture()));
    CHECK_FALSE(resources.addPlayers(device, weapons, {}, lenders));
    std::array<std::unique_ptr<PlayerFigure>, 4> owners;
    std::array<PlayerFigure*, 4> figures{};
    for (usize seat = 0; seat < figures.size(); ++seat) {
        CharacterSave save;
        save.character = static_cast<s32>(seat);
        save.color = static_cast<s32>(seat);
        save.progress().experience = levelExperience(80);
        owners[seat] = PlayerFigure::load(device, root, save, false);
        REQUIRE(owners[seat]);
        figures[seat] = owners[seat].get();
    }
    auto duplicate = figures;
    duplicate[3] = duplicate[0];
    CHECK_FALSE(resources.addPlayers(device, weapons, duplicate, lenders));
    CHECK(resources.modelId(&figures[0]->missile()) == 0);
    CHECK(resources.streakId(&device.whiteTexture()) == 40000);
    REQUIRE(resources.addPlayers(device, weapons, figures, lenders));
    for (usize seat = 0; seat < figures.size(); ++seat) {
        CHECK(resources.modelId(&figures[seat]->missile()) == 4096 + seat * 8192);
        CHECK(resources.modelId(&figures[seat]->familiarMissile()) == 4097 + seat * 8192);
    }
    CHECK(resources.streakId(&device.whiteTexture()) == 40000);
}

TEST_CASE("native player projectile catalogs cover every costume class and future equipped shots",
          "[netplay][player-projectile-resources][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ItemArchive clientWeapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    REQUIRE(clientWeapons.load(root / "WEAPONS"));
    ClassDataSet classes;
    REQUIRE(classes.load(root / "pdata"));
    const std::array<TextureSet*, 1> lenders{&weapons.textures};
    const std::array<TextureSet*, 1> clientLenders{&clientWeapons.textures};
    WorldCollision collision;
    LevelSoundscape audio;
    const EnemyMissiles enemies;
    for (s32 character = 0; character < kStartingClassCount * 2; ++character) {
        CAPTURE(character);
        CharacterSave save;
        save.character = character;
        save.color = character % kColorCount;
        save.progress().experience = levelExperience(80);
        auto figure = PlayerFigure::load(device, root, save, false);
        auto clientFigure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        REQUIRE(clientFigure);
        const std::array<PlayerFigure*, 4> figures{nullptr, nullptr, nullptr, figure.get()};
        const std::array<PlayerFigure*, 4> clientFigures{nullptr, nullptr, nullptr,
                                                         clientFigure.get()};
        ProjectileResources host;
        ProjectileResources client;
        REQUIRE(host.addPlayers(device, weapons, figures, lenders));
        REQUIRE(client.addPlayers(device, clientWeapons, clientFigures, clientLenders));
        CHECK_FALSE(host.addPlayers(device, weapons, figures, lenders));
        EffectTrees effects;
        effects.setTextureLenders(lenders);
        PlayerArsenal arsenal;
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}}, lenders);
        PlayerActor actor;
        actor.spawn(3, save, classes.stats(character), {0, 0, 0}, 0);
        const auto inspect = [&] {
            CombatSnapshot state;
            state.motion.epoch = 1;
            state.motion.cameraContinuity = 1;
            REQUIRE(ProjectileCapture::append(state, host, arsenal.missiles(), enemies, &effects));
            REQUIRE_FALSE(state.projectiles.empty());
            const auto wire = CombatPacket::encode(state);
            REQUIRE(wire);
            const auto received = CombatPacket::decode(*wire);
            REQUIRE(received);
            ReplicaProjectiles replica;
            REQUIRE(replica.begin(1));
            REQUIRE(replica.show(*received, client));
            const auto textures = device.texturesCreated;
            device.draws.clear();
            replica.draw(device, client, Mat4{1}, {}, CameraFrame::at({10, 20, 30}));
            CHECK_FALSE(device.draws.empty());
            CHECK(device.texturesCreated == textures);
            // Clear only after receipt/draw: no per-shot asset registration or GPU loads.
            arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}},
                         lenders);
            effects.clear();
            effects.setTextureLenders(lenders);
        };
        actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
        arsenal.launchFamiliar(actor, figure.get());
        REQUIRE(arsenal.missiles().count() == 3);
        inspect();
        for (u32 element = 1; element <= 4; ++element) {
            CAPTURE(element);
            actor.save().progress().inventory = {};
            actor.save().progress().inventory.addPowerup(powerup::kWeapon, element, 0, 60);
            arsenal.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
            REQUIRE(arsenal.missiles().count() == 1);
            inspect();
        }
        actor.save().progress().inventory = {};
        actor.save().progress().inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 5, 60);
        arsenal.launchSuperShot(actor, figure.get());
        REQUIRE(arsenal.missiles().count() == 1);
        inspect();
        for (const bool left : {false, true}) {
            actor.save().progress().inventory = {};
            actor.save().progress().inventory.addPowerup(
                powerup::kSpecial, left ? powerup::kLeftGauntlet : powerup::kRightGauntlet, 0, 60);
            arsenal.launchGauntlet(actor, figure.get(), left);
            REQUIRE(arsenal.missiles().count() == 1);
            inspect();
        }
        for (s32 potion = 1; potion <= 4; ++potion) {
            actor.save().progress().inventory.addPotions(potion, 1);
            arsenal.throwPotion(actor, 30);
            REQUIRE(arsenal.missiles().count() == 1);
            arsenal.burstPotion(potion, {0, 0, 5}, 20, false);
            inspect();
        }
    }
}
} // namespace
