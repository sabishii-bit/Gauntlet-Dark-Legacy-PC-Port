#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/LevelWorld.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("every realm's final stage preloads item and boss effects without running an encounter",
          "[netplay][stage-projectile-resources][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    ItemArchive weapons;
    TextureSet common;
    REQUIRE(weapons.load(root / "WEAPONS"));
    REQUIRE(common.load(root / "STATIC"));
    CharacterSave save;
    save.progress().experience = levelExperience(80);
    auto player = PlayerFigure::load(device, root, save, false);
    REQUIRE(player);
    const std::array<PlayerFigure*, 4> figures{player.get(), nullptr, nullptr, nullptr};
    usize stages = 0;
    for (const auto& realm : catalog.realms()) {
        REQUIRE_FALSE(realm.levels.empty());
        const auto level = catalog.byName(realm.levels.back());
        REQUIRE(level);
        CAPTURE(level->name);
        LevelWorld world;
        REQUIRE(world.load(device, root, *level));
        const std::array<TextureSet*, 5> lenders{&weapons.textures, &world.items().textures,
                                                 &world.realmItems().textures,
                                                 &world.powerups().textures, &common};
        Bosses bosses;
        const std::array creatureTextures{&world.textures()};
        bosses.open(device, root, nullptr, {}, level->name.front(), creatureTextures);
        if (world.level() != nullptr && !bossNameOf(world.level()->bossType).empty()) {
            auto* stock = bosses.preload(world.level()->bossType);
            REQUIRE(stock != nullptr);
            CHECK(bosses.preload(world.level()->bossType) == stock);
            CHECK(bosses.preload(-1) == nullptr);
            CHECK_FALSE(bosses.view().alive);
            CHECK(bosses.position() == nullptr);
            CHECK(bosses.takeShots().empty());
            CHECK(bosses.takeCues().empty());
        }
        const std::array archives{&world.items(), &world.realmItems(), &world.powerups()};
        ProjectileResources resources;
        REQUIRE(resources.addPlayers(device, weapons, figures, lenders));
        REQUIRE(resources.addStage(device, archives, bosses.resources(), lenders));
        CHECK_FALSE(resources.addStage(device, archives, bosses.resources(), lenders));
        CHECK(resources.modelId(&player->missile()) == 4096);
        // Native tree IDs are established before effects are emitted. Rendering
        // will borrow these assets; no boss actor or gameplay cue was created.
        usize registered = 0;
        for (auto* archive : archives) {
            for (usize i = 0; i < archive->trees.size(); ++i) {
                EffectTrees::Effect effect;
                effect.archive = archive;
                effect.tree = &archive->trees.tree(static_cast<u32>(i));
                effect.lenders.assign(lenders.begin(), lenders.end());
                registered += resources.effectId(effect) != 0 ? 1U : 0U;
            }
        }
        CHECK(registered > 0);
        ++stages;
    }
    CHECK(stages >= 12);
}
} // namespace
