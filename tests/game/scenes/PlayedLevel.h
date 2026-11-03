#pragma once
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/PlayScene.h"

namespace gdl::test {

/**
 * A level as the console's own commands find it: opened for two seats of TESTERs with plenty of
 * health and no welcome, or nothing in play at all (the scene closed, the world never loaded,
 * nothing unpacked). It cannot move: the scene borrows the world and the context.
 */
struct PlayedLevel {
    static constexpr s32 kSeats = 2;

    std::filesystem::path root;
    FakeRenderDevice device;
    game::GameConfig config;
    game::LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    game::LevelWorld world;
    game::GameServices context;
    game::PlayScene scene;

    /** Nothing in play: an empty folder is taken as the unpacked data. */
    PlayedLevel() : root(scratchDirectory("played-level-unpacked")) {}

    /** `levelName` open for two seats (the tower for "L1"); skips without the game data. */
    explicit PlayedLevel(std::string_view levelName)
        : root(assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path()) {
        assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", levelName));
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(dataDirectory() / "text", "en"));
        const auto level =
            levelName == "L1" ? std::optional{game::LevelRef::tower()} : catalog.byName(levelName);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        std::vector<game::PartyMember> party;
        for (s32 seat = 0; seat < kSeats; ++seat) {
            game::CharacterSave save;
            save.name = "TESTER";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        game::PlayOptions options;
        options.welcome = false;
        REQUIRE(
            scene.open(device, context, world, party, options, {.stopTimeTotal = &stopTimeTotal}));
    }

    /** The scene while it is open, as offline play's console reads it; else null. */
    game::PlayScene* openScene() { return scene.isOpen() ? &scene : nullptr; }
    /** The world of the open scene; else null. */
    const game::LevelWorld* openWorld() const { return scene.isOpen() ? &world : nullptr; }

    /** The first seat with a character in, as the console speaks for it; -1 with nobody in. */
    s32 player() const {
        for (s32 seat = 0; seat < game::PlayScene::kPlayerCount; ++seat) {
            if (scene.actor(seat) != nullptr) {
                return seat;
            }
        }
        return -1;
    }

    /** The save of the character the console speaks for; null with nobody in. */
    const game::CharacterSave* playerSave() const {
        const game::PlayerActor* actor = scene.actor(player());
        return actor != nullptr ? &actor->save() : nullptr;
    }

    game::CharacterSave& save(s32 seat) {
        game::CharacterSave* live = scene.liveSave(seat);
        REQUIRE(live != nullptr);
        return *live;
    }
};

} // namespace gdl::test
