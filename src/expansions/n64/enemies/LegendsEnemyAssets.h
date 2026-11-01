#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
struct ItemArchive;
namespace formats {
struct AnimationFile;
}
namespace game {
/** Native swarm resources translated into the common enemy animation contract. */
std::unique_ptr<ItemArchive> loadLegendsEnemyAssets(const std::filesystem::path& nativeRoot,
                                                    s32 kind, std::span<const u8> program,
                                                    std::vector<std::string>& warnings);
void translateLegendsEnemyAnimations(formats::AnimationFile& animation, s32 kind,
                                     std::span<const u8> program);
} // namespace game
} // namespace gdl
