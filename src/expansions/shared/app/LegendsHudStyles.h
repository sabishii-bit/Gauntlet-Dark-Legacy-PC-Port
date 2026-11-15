#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/render/RenderDevice.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"

namespace gdl::game {

/** Where the Legends HUD looks' art is looked for, as legendsMaps looks for the campaigns' maps:
 * the configured assets first, then the executable's folder and the working folder, each with
 * its ancestors. */
struct LegendsHudRoots {
    std::filesystem::path assetRoot;
    std::filesystem::path executableDirectory;
    std::filesystem::path workingDirectory;
};

/** The folder a Legends campaign's HUD art is read from: the extraction holding the first maps
 * legendsMaps finds, `n64/extracted` or `dreamcast/extracted/native`; none without one. */
std::optional<std::filesystem::path> legendsHudRoot(LegendsPlatform platform,
                                                    const LegendsHudRoots& roots);

/** The files under its campaign's HUD root that Legends look `id` cannot be drawn without; none
 * for an id no Legends look has. */
std::span<const std::string_view> legendsHudRequiredFiles(std::string_view id);

/** Whether Legends look `id`'s campaign is installed with every file the look cannot be drawn
 * without, none of them empty; false for an id no Legends look has, and, with a warning, when
 * its files cannot be looked for. */
bool legendsHudFilesFound(std::string_view id, const LegendsHudRoots& roots);

/** The Legends HUD looks this build has painters for, in the order the HUD page lists them, each
 * usable while its files are found; looked for afresh at each ask. One look's fault hides no
 * other. */
std::vector<HudStyleOffer> legendsHudStyles(const LegendsHudRoots& roots);

/** Opens Legends look `id`'s painter for `party`, which takes its own art and the art it borrows
 * from Dark Legacy's now; null, with one warning saying why, when the look's files are not found,
 * this build has no painter for it, or its painter does not load. */
std::unique_ptr<HudPainter> openLegendsHud(std::string_view id, RenderDevice& device,
                                           DarkLegacyHudArt& darkLegacy,
                                           std::span<const HudMember> party,
                                           const LegendsHudRoots& roots);

} // namespace gdl::game
