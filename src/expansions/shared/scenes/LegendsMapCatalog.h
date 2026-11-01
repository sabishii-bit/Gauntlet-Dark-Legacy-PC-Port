#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

struct LegendsMapEntry {
    std::string name;
    std::filesystem::path directory;
};

/** Finds the first usable standard extraction, without recursively searching the disk.
 * Configured assets take precedence over the executable and working directories. */
std::vector<LegendsMapEntry> legendsMaps(LegendsPlatform platform,
                                         const std::filesystem::path& assetRoot,
                                         const std::filesystem::path& executableDirectory,
                                         const std::filesystem::path& workingDirectory);

/** Resolves a short map ID without changing existing explicit-path command arguments. */
std::optional<std::filesystem::path> legendsMapPath(std::span<const LegendsMapEntry> maps,
                                                    std::string_view value);

/** The current extraction's own native Tower, only when its world and model
 * files exist. Full decode still precedes replacing the current scene. */
std::optional<std::filesystem::path>
legendsTowerPath(LegendsPlatform platform, const std::filesystem::path& currentDirectory);

} // namespace gdl::game
