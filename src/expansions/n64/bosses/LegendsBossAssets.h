#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
class RenderDevice;
class TextureSet;
namespace game {
struct CombatantAssets;
/** Native boss art, skeleton, moves, damage windows and collision nodes. The
 * caller must retain the result until all combatants/projectiles have finished. */
std::unique_ptr<CombatantAssets> loadLegendsBossAssets(RenderDevice& device,
                                                       const std::filesystem::path& nativeRoot,
                                                       const std::filesystem::path& gameCubeRoot,
                                                       s32 kind, std::span<const u8> program,
                                                       std::span<TextureSet* const> textureLenders,
                                                       std::vector<std::string>& warnings);
} // namespace game
} // namespace gdl
