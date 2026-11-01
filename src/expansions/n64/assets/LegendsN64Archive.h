#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"

namespace gdl::game {
/** Decodes one native archive. Combined groups expose native combat sequences on
 * one BODY; the default preserves the independently played fixture trees. */
std::unique_ptr<ItemArchive> loadLegendsN64Archive(
    const std::filesystem::path& directory, std::vector<std::string>& warnings,
    std::span<const u8> program, const std::filesystem::path& animationDirectory = {},
    bool combinedGroups = false, std::span<const std::filesystem::path> sharedDirectories = {},
    const std::function<void(formats::AnimationFile&)>& translate = {});
} // namespace gdl::game
