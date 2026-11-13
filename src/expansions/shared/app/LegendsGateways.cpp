#include "expansions/shared/app/LegendsGateways.h"

#include <array>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/scenes/LegendsMapCatalog.h"

namespace gdl::game {
namespace {

/** A campaign's tower, as Sumner's gateway leads to it. */
struct LegendsGateway {
    LegendsPlatform platform;
    std::string_view label; ///< the text id of its name among Sumner's choices
    std::string_view tower; ///< its folder among the campaign's maps
};

/** The N64's stage table names its tower tower2; the Dreamcast's worlddata/tower.wad holds the
 * one level T1, extracted as levelt1 (legendsTowerPath). */
constexpr std::array kGateways{
    LegendsGateway{LegendsPlatform::N64, "legends.n64.gateway.label", "tower2"},
    LegendsGateway{LegendsPlatform::Dreamcast, "legends.dreamcast.gateway.label", "levelt1"}};

bool nonemptyFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    std::error_code error;
    if (!path || !std::filesystem::is_regular_file(*path, error)) {
        return false;
    }
    const auto size = std::filesystem::file_size(*path, error);
    return !error && size != 0;
}

/** Whether the campaign's extraction holds its tower whole. */
bool towerUsable(const LegendsGateway& gateway, const std::filesystem::path& assetRoot,
                 const std::filesystem::path& executableDirectory,
                 const std::filesystem::path& workingDirectory) {
    const auto maps =
        legendsMaps(gateway.platform, assetRoot, executableDirectory, workingDirectory);
    const auto tower = legendsMapPath(maps, gateway.tower);
    if (!tower) {
        return false;
    }
    if (gateway.platform == LegendsPlatform::Dreamcast) {
        // The scene adapter will not open a Dreamcast map without its textures.
        return nonemptyFile(AssetLocator{*tower}, "textures.rom");
    }
    // Without the program tables the N64 tower opens as a plain map, with no gates, ceremonies
    // or Sumner (LegendsSceneAdapter::loadN64 and LegendsLevel's Sumner).
    const AssetLocator root{tower->parent_path().parent_path()};
    return nonemptyFile(root, "_program/game.bin") && nonemptyFile(root, "_program/Tower.bin");
}

} // namespace

std::vector<GatewayWorld> legendsGatewayWorlds(const std::filesystem::path& assetRoot,
                                               const std::filesystem::path& executableDirectory,
                                               const std::filesystem::path& workingDirectory) {
    std::vector<GatewayWorld> worlds;
    worlds.reserve(kGateways.size());
    for (const LegendsGateway& gateway : kGateways) {
        bool usable = false;
        try {
            usable = towerUsable(gateway, assetRoot, executableDirectory, workingDirectory);
        } catch (const std::exception& error) {
            log::warn("Legends gateway to {}: {}", gateway.tower, error.what());
        }
        worlds.push_back({static_cast<u8>(gateway.platform), std::string(gateway.label), usable});
    }
    return worlds;
}

} // namespace gdl::game
