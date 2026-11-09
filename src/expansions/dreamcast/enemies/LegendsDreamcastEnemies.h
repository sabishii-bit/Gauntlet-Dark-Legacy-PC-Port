#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "game/level/ImportedActors.h"
#include "game/level/ImportedGenerators.h"

namespace gdl::game {

std::unique_ptr<ItemArchive>
loadLegendsDreamcastEnemyAssets(const std::filesystem::path& nativeRoot, s32 kind,
                                std::vector<std::string>& warnings,
                                std::span<const LevelEnemy> roster = {});
std::unique_ptr<ItemArchive>
loadLegendsDreamcastGeneratorAssets(const std::filesystem::path& nativeRoot, s32 kind,
                                    const formats::LegendsDreamcastFixtureScope& scope,
                                    std::vector<std::string>& warnings);

struct LegendsDreamcastGenerator {
    ImportedGenerator gameplay;
    std::array<std::string, 4> models;
};

/** Native SetItem/roster adaptation. Descriptors use Dark Legacy combat;
 * models, placement, damage states and party requirements remain Dreamcast. */
std::optional<LegendsDreamcastGenerator>
legendsDreamcastGenerator(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                          const formats::LegendsDreamcastFixtureScope& scope,
                          std::string* diagnostic = nullptr);
std::optional<ImportedActor>
legendsDreamcastActor(const formats::ItemInfoRecord& info,
                      const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                      const formats::LegendsDreamcastFixtureScope& scope,
                      std::string* diagnostic = nullptr);

} // namespace gdl::game
