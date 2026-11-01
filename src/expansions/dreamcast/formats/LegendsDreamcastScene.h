#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"

#include "expansions/shared/formats/LegendsMap.h"
#include "formats/WorldFile.h"

namespace gdl::formats {

/** Native Dreamcast fixture descriptions. Types and parameters retain their
 * original meanings; reading them does not instantiate GameCube gameplay. */
struct LegendsDreamcastScene {
    std::vector<ItemInfoRecord> itemInfos;
    std::vector<ItemInstanceRecord> itemInstances;
    std::vector<WorldLocator> cameraMarkers;

    static LegendsDreamcastScene parse(std::span<const u8> worlds);
};

/** LOD-zero meshes in native object-index order, including unnamed animation
 * frames. Names come from the native definition table; UVs remain in texels. */
std::vector<LegendsMapObject> decodeDreamcastModelLibrary(std::span<const u8> models);

struct LegendsDreamcastFixtureScope {
    std::vector<std::string>
        archives; ///< Native-root-relative library directories, in lookup order.
    std::array<s32, 6> enemyClasses{-1, -1, -1, -1, -1, -1};
    s32 realmType = -1;
    std::optional<LevelCameraInfo> camera;
};

/** Selects the native realm metadata file, not an arbitrary artwork search. */
std::string dreamcastLegendsWorldDataFile(std::string_view levelName);

/** Resolves authored library names and selected enemy roster from a native realm WAD.
 * Returns empty if that WAD does not actually contain the requested level. */
std::optional<LegendsDreamcastFixtureScope>
dreamcastLegendsFixtureScope(std::span<const u8> worlddata, std::string_view levelName);

/** Initial visual name, including native instance overrides, hidden placements, and
 * generator species/tier selection. Does not resolve random-choice gameplay records. */
std::string dreamcastLegendsFixtureName(const ItemInfoRecord& info,
                                        const ItemInstanceRecord& instance,
                                        const LegendsDreamcastFixtureScope& scope);

} // namespace gdl::formats
