#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/characters/Progression.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/TowerAccess.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/TowerWelcome.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::test {

/** A character back from Garm's citadel: experienced, and holding his piece with the class it
 * plays. */
inline game::CharacterSave garmBeaten(std::string_view name) {
    game::CharacterSave save;
    save.name = std::string(name);
    save.progress().experience = game::levelExperience(40);
    save.progress().relics.addShard(game::LevelRef::orderOf(game::TowerAccess::kBattlefieldWorld));
    return save;
}

/** How the journey back from Garm's citadel opens the tower. */
inline game::PlayOptions backFromGarm() {
    game::PlayOptions options;
    options.welcome = false;
    options.arriving = true;
    options.arrivalWorld = static_cast<u32>(game::TowerAccess::kBattlefieldWorld);
    options.beaten =
        game::LevelBeaten{game::TowerAccess::kBattlefieldWorld, game::TowerAccess::kGarmGate};
    return options;
}

/** The tower in English, with the worlds past Sumner's gateways a test's installation offers:
 * the first usable and the second not, unless the test says otherwise. Skips without the game
 * data. */
struct GatewayTower {
    const std::filesystem::path root =
        assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const game::GameConfig config;
    StringTable strings;
    FakeRenderDevice device;
    game::LevelCatalog levels;
    game::LevelWorld world;
    game::GameServices context;
    FixedGateways gateways;
    game::PlayScene scene;

    GatewayTower() {
        REQUIRE(strings.load(dataDirectory() / "text", "en"));
        REQUIRE(levels.load(root));
        REQUIRE(world.load(device, root));
        context.config = &config;
        context.strings = &strings;
        context.levels = &levels;
        context.unpackedRoot = root;
        gateways.offered = {{0, "legends.n64.gateway.label", true},
                            {1, "legends.dreamcast.gateway.label", false}};
    }

    /** Opens the tower for `party` as `options` say, with the gateways (offline) or without
     * them (as online), and lets the party materialise, `step` an update; no scroll shows
     * meanwhile. */
    void arrive(std::span<const game::PartyMember> party, f64 step,
                const game::PlayOptions& options = backFromGarm(), bool offline = true) {
        scene.close();
        REQUIRE(scene.open(device, context, world, party, options,
                           offline ? game::PlayScene::Links{.gateways = &gateways}
                                   : game::PlayScene::Links{}));
        const game::PlayScene::Inputs still{};
        for (s32 i = 0; i < 1200 && scene.spawning(); ++i) {
            REQUIRE_FALSE(scene.scroll().active());
            scene.update(step, still);
        }
        REQUIRE_FALSE(scene.spawning());
    }
    /** Whether the scroll up is Sumner's one page about his gateways, as the English has it. */
    bool tellingGateways() const {
        return scene.scroll().active() && !scene.scrollLook() && scene.scroll().pageCount() == 1 &&
               scene.scroll().lines() ==
                   game::ScrollBox::splitLines(strings.get(game::TowerWelcome::kGatewayScroll));
    }
    /** Turns the scroll's pages as they take a button, until it has burnt away. */
    void readThrough(f64 step) {
        game::PlayScene::Inputs accept{};
        accept[0].menu.select = true;
        const game::PlayScene::Inputs still{};
        for (s32 i = 0; i < 2000 && scene.scroll().active(); ++i) {
            scene.update(step, i % 2 == 0 ? accept : still);
        }
        REQUIRE_FALSE(scene.scroll().active());
    }
};

} // namespace gdl::test
