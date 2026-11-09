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

struct LegendsDreamcastBossCamera {
    f32 floorOffset = 0;
    f32 attentionHeight = 0;
    f32 radius = 0;
};
/** Native camera subject is separate from the combat/reward origin. */
LegendsDreamcastBossCamera legendsDreamcastBossCamera(std::span<const u8> moves);

/** Bind the six shipped Dreamcast boss banks to shared Dark Legacy combat.
 * Unsupported kinds return null; missing/malformed selected assets throw. */
std::unique_ptr<CombatantAssets>
loadLegendsDreamcastBossAssets(RenderDevice& device, const std::filesystem::path& nativeRoot,
                               const std::filesystem::path& commonRoot, s32 kind,
                               std::span<TextureSet* const> textureLenders,
                               std::vector<std::string>& warnings);

} // namespace game
} // namespace gdl
