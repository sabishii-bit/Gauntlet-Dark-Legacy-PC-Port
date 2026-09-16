#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace gdl {

/**
 * Resolves game-relative asset names ("VQMOVIES/opening.avi") to files under the asset
 * root, ignoring case in every path component so the original lowercase names work on
 * case-sensitive filesystems.
 */
class AssetLocator {
public:
    explicit AssetLocator(std::filesystem::path root);

    const std::filesystem::path& root() const { return m_root; }

    /** Returns the matching file or directory, or nothing when no component matches. */
    std::optional<std::filesystem::path> find(std::string_view relative) const;

    /** Like find(), but throws FileError when the asset is missing. */
    std::filesystem::path require(std::string_view relative) const;

private:
    std::filesystem::path m_root;
};

} // namespace gdl
