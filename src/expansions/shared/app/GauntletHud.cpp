#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/platform/Paths.h"
#include "engine/render/RenderDevice.h"

#include "expansions/shared/app/LegendsHudStyles.h"
#include "game/app/Gauntlet.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"

namespace gdl::game {
namespace {
/** Where the HUD looks' art is looked for: from the game's own data, as Sumner's gateways look
 * for their towers. */
LegendsHudRoots hudRoots(const std::filesystem::path& assetRoot) {
    std::error_code error;
    auto working = std::filesystem::current_path(error); // none when it cannot be read
    return {.assetRoot = assetRoot,
            .executableDirectory = paths::executableDirectory(),
            .workingDirectory = std::move(working)};
}
} // namespace

std::span<const HudStyleOffer> Gauntlet::HudRouting::offers() {
    m_offers = legendsHudStyles(hudRoots(m_game.m_options.unpackedDirectory));
    return m_offers;
}

std::unique_ptr<HudPainter> Gauntlet::HudRouting::open(std::string_view id, RenderDevice& device,
                                                       DarkLegacyHudArt& darkLegacy,
                                                       std::span<const HudMember> party) {
    return openLegendsHud(id, device, darkLegacy, party,
                          hudRoots(m_game.m_options.unpackedDirectory));
}

std::vector<HudStyleOffer> Gauntlet::SettingsRouting::hudStyles() {
    const auto offers = m_game.m_hudStyles.offers();
    return {offers.begin(), offers.end()};
}

} // namespace gdl::game
