#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/tower/TowerGateways.h"

namespace gdl::game {

/** A Legends campaign's tower that Sumner's gateway leads to: its campaign and its folder. */
struct LegendsGatewayTower {
    LegendsPlatform platform = LegendsPlatform::N64;
    std::filesystem::path directory;
};

/** The towers Sumner's gateways lead to, one for each Legends campaign, in the order he names
 * them: the N64's, then the Dreamcast's, each world's id being its campaign's LegendsPlatform.
 * A tower is usable where its campaign's extraction, found under the roots as legendsMaps finds
 * it, holds what opening the tower needs: its world and object files and, for the N64, the
 * program and tower tables that make it a tower with Sumner in it, for the Dreamcast its
 * textures. One campaign's fault never hides the other's tower. */
std::vector<GatewayWorld> legendsGatewayWorlds(const std::filesystem::path& assetRoot,
                                               const std::filesystem::path& executableDirectory,
                                               const std::filesystem::path& workingDirectory);

/** The tower of `world` among the worlds legendsGatewayWorlds offers, found as it finds them;
 * none for a world no campaign offers or one whose tower this installation cannot open. */
std::optional<LegendsGatewayTower>
legendsGatewayTower(u8 world, const std::filesystem::path& assetRoot,
                    const std::filesystem::path& executableDirectory,
                    const std::filesystem::path& workingDirectory);

} // namespace gdl::game
