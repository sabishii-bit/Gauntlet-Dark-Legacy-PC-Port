#include "expansions/shared/scenes/LegendsMapCatalog.h"

#include <algorithm>
#include <array>
#include <set>
#include <system_error>

#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/AssetLocator.h"
#include "engine/platform/Paths.h"

namespace gdl::game {
namespace {
bool nonemptyFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    std::error_code error;
    if (!path || !std::filesystem::is_regular_file(*path, error)) {
        return false;
    }
    const auto size = std::filesystem::file_size(*path, error);
    return !error && size != 0;
}

std::vector<LegendsMapEntry> catalog(const std::filesystem::path& directory,
                                     LegendsPlatform platform) {
    std::vector<LegendsMapEntry> result;
    std::error_code error;
    std::filesystem::directory_iterator current(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && current != end) {
        const auto& entry = *current;
        std::error_code entryError;
        if (entry.is_directory(entryError)) {
            const AssetLocator files{entry.path()};
            if (nonemptyFile(files,
                             platform == LegendsPlatform::N64 ? "worlds.wad" : "worlds.rom") &&
                nonemptyFile(files,
                             platform == LegendsPlatform::N64 ? "objects.wad" : "objects.rom")) {
                result.push_back({paths::utf8(entry.path().filename()), entry.path()});
            }
        }
        current.increment(error);
    }
    if (error) {
        return {}; // Do not publish a partial listing after an interrupted directory read.
    }
    std::ranges::sort(result, [](const LegendsMapEntry& a, const LegendsMapEntry& b) {
        const auto left = toLowerAscii(a.name);
        const auto right = toLowerAscii(b.name);
        return left != right ? left < right : a.name < b.name;
    });
    return result;
}

std::string pathKey(const std::filesystem::path& path) {
    const std::string key = paths::genericUtf8(path);
#ifdef _WIN32
    return toLowerAscii(key);
#else
    return key;
#endif
}
} // namespace

std::vector<LegendsMapEntry> legendsMaps(LegendsPlatform platform,
                                         const std::filesystem::path& assetRoot,
                                         const std::filesystem::path& executableDirectory,
                                         const std::filesystem::path& workingDirectory) {
    std::error_code error;
    const auto working = std::filesystem::absolute(
        workingDirectory.empty() ? std::filesystem::current_path(error) : workingDirectory, error);
    if (error) {
        return {};
    }
    const std::string suffix = platform == LegendsPlatform::N64
                                   ? "n64/extracted/levels"
                                   : "dreamcast/extracted/native/levels";
    const std::array<std::string, 2> candidates{"assets/" + suffix, suffix};
    std::set<std::string> visited;
    for (const auto& location : {assetRoot, executableDirectory, workingDirectory}) {
        if (location.empty()) {
            continue;
        }
        auto ancestor = (location.is_absolute() ? location : working / location).lexically_normal();
        while (!ancestor.empty()) {
            try {
                const AssetLocator parent{ancestor};
                for (const auto& relative : candidates) {
                    const auto directory = parent.find(relative);
                    if (!directory || !std::filesystem::is_directory(*directory, error)) {
                        continue;
                    }
                    const auto canonical = std::filesystem::weakly_canonical(*directory, error);
                    if (error || !visited.insert(pathKey(canonical)).second) {
                        continue;
                    }
                    auto entries = catalog(canonical, platform);
                    if (!entries.empty()) {
                        return entries;
                    }
                }
            } catch (const std::filesystem::filesystem_error& error) {
                // An unavailable extraction must not hide another valid configured root.
                log::trace("Legends maps: {} is not readable: {}", paths::utf8(ancestor),
                           error.what());
            }
            const auto parent = ancestor.parent_path();
            if (parent == ancestor) {
                break;
            }
            ancestor = parent;
        }
    }
    return {};
}

std::optional<std::filesystem::path> legendsMapPath(std::span<const LegendsMapEntry> maps,
                                                    std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::filesystem::path path{value};
    if (value.find_first_of("/\\") != std::string_view::npos || path.has_root_name()) {
        return path;
    }
    // Exact names win if a case-sensitive filesystem contains both spellings.
    for (const auto& entry : maps) {
        if (entry.name == value) {
            return entry.directory;
        }
    }
    const auto name = toLowerAscii(value);
    for (const auto& entry : maps) {
        if (toLowerAscii(entry.name) == name) {
            return entry.directory;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
legendsTowerPath(LegendsPlatform platform, const std::filesystem::path& currentDirectory) {
    if (currentDirectory.empty() ||
        (platform != LegendsPlatform::N64 && platform != LegendsPlatform::Dreamcast)) {
        return {};
    }
    auto current = currentDirectory.lexically_normal();
    if (current.filename().empty())
        current = current.parent_path();
    try {
        // N64's native stage table names tower2. Dreamcast worlddata/tower.wad
        // has the single LEVL code T1/name Tower, loaded as levels/levelt1.
        const AssetLocator siblings{current.parent_path()};
        const auto tower = siblings.find(platform == LegendsPlatform::N64 ? "tower2" : "levelt1");
        if (!tower)
            return {};
        const AssetLocator files{*tower};
        const bool n64 = platform == LegendsPlatform::N64;
        if (nonemptyFile(files, n64 ? "worlds.wad" : "worlds.rom") &&
            nonemptyFile(files, n64 ? "objects.wad" : "objects.rom")) {
            return tower;
        }
    } catch (const std::filesystem::filesystem_error&) {
        // A missing/unreadable native Tower never redirects to another install.
    }
    return {};
}

} // namespace gdl::game
