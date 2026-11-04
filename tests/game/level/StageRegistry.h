#pragma once

#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"

#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"

namespace gdl::test {

/** Registers what a loaded level lends the owners standing in it, as the scene does before
 * hashing them: the renderer's white texture, the item archives in search order, the powerups,
 * the empty textures of a realm archive that is absent, the level's own textures, and its
 * collision, layout and hazards. Two loads of one level registered this way hash alike; the
 * owners register their own models afterwards. */
inline void registerStage(game::NativeStateResources& resources, game::LevelWorld& world,
                          const RenderDevice& device) {
    REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    const AssetScope scope = world.itemScope();
    usize slot = 0;
    for (const ItemArchive* archive : scope.archives()) {
        REQUIRE(resources.registerArchive(*archive, std::format("stage/items/{}", slot)));
        ++slot;
    }
    if (world.powerups().loaded()) {
        REQUIRE(resources.registerArchive(world.powerups(), "powerups"));
    }
    if (!world.realmItems().loaded()) {
        REQUIRE(resources.registerTextures(world.realmItems().textures, "stage/empty-realm"));
    }
    REQUIRE(resources.registerTextures(world.textures(), "stage/textures"));
    REQUIRE(resources.registerCollision(world.collision(), "stage"));
    REQUIRE(resources.registerLayout(world.layout(), "stage"));
    REQUIRE(resources.registerHazards(world.hazards(), "stage"));
}

/** Registers what a synthetic level lends the item owners standing in it, under the keys every
 * load of it shares: the renderer's white texture, the item archive when it is loaded, the
 * layout, and the collision when there is one. */
inline void registerItemStage(game::NativeStateResources& resources, const ItemArchive& items,
                              const WorldLayout& layout, const WorldCollision* collision,
                              const RenderDevice& device) {
    REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    if (items.loaded()) {
        REQUIRE(resources.registerArchive(items, "stage/items/0"));
    }
    REQUIRE(resources.registerLayout(layout, "stage"));
    if (collision != nullptr) {
        REQUIRE(resources.registerCollision(*collision, "stage"));
    }
}

} // namespace gdl::test
