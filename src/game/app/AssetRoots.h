#pragma once

#include <filesystem>

namespace gdl::game {

/** Retail and shipped-data roots, resolved independently of the working directory. */
struct AssetRoots {
    std::filesystem::path assets;
    std::filesystem::path data;
};

/** Prefer executable-adjacent trees, with configured paths for developer builds. */
AssetRoots resolveAssetRoots(const std::filesystem::path& executableDirectory,
                             const std::filesystem::path& developerAssets,
                             const std::filesystem::path& developerData);

} // namespace gdl::game
