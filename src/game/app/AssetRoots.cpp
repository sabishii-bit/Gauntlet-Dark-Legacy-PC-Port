#include "game/app/AssetRoots.h"

#include <string_view>
#include <system_error>

#include "engine/io/AssetLocator.h"

namespace gdl::game {

namespace {

std::filesystem::path adjacentOrFallback(const std::filesystem::path& directory,
                                         std::string_view name,
                                         const std::filesystem::path& fallback) {
    const auto adjacent = AssetLocator(directory).find(name);
    std::error_code error;
    if (adjacent && std::filesystem::is_directory(*adjacent, error)) {
        return *adjacent;
    }
    return fallback.empty() ? directory / name : fallback;
}

} // namespace

AssetRoots resolveAssetRoots(const std::filesystem::path& executableDirectory,
                             const std::filesystem::path& developerAssets,
                             const std::filesystem::path& developerData) {
    return {adjacentOrFallback(executableDirectory, "Gauntlet", developerAssets),
            adjacentOrFallback(executableDirectory, "data", developerData)};
}

} // namespace gdl::game
