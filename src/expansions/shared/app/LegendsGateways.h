#pragma once

#include <filesystem>
#include <vector>

#include "game/tower/TowerGateways.h"

namespace gdl::game {

/** The towers Sumner's gateways lead to, one for each Legends campaign, in the order he names
 * them: the N64's, then the Dreamcast's, each world's id being its campaign's LegendsPlatform.
 * A tower is usable where its campaign's extraction, found under the roots as legendsMaps finds
 * it, holds what opening the tower needs: its world and object files and, for the N64, the
 * program and tower tables that make it a tower with Sumner in it, for the Dreamcast its
 * textures. One campaign's fault never hides the other's tower. */
std::vector<GatewayWorld> legendsGatewayWorlds(const std::filesystem::path& assetRoot,
                                               const std::filesystem::path& executableDirectory,
                                               const std::filesystem::path& workingDirectory);

} // namespace gdl::game
