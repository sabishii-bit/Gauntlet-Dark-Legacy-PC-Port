#include "expansions/shared/app/LegendsHudStyles.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Expected.h"
#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/platform/Paths.h"
#include "engine/render/RenderDevice.h"

#include "expansions/shared/app/LegendsFiles.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "expansions/shared/scenes/LegendsMapCatalog.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"

namespace gdl::game {
namespace {

/** The N64 look's HUD bank and the three fonts it writes in: 4x6font's counts and words, glfm3's
 * names and help, numfont's health. */
constexpr std::array<std::string_view, 4> kN64Files{"hud/objects.wad", "Font/4x6font.fnt",
                                                    "Font/glfm3.fnt", "Font/numfont.FNT"};
/** The Dreamcast look's static set, which holds its boxes' art, and the three fonts the boxes are
 * written in. */
constexpr std::array<std::string_view, 5> kDreamcastFiles{
    "static/objects.rom", "static/textures.rom", "fonts/8hi_fonts5.fnt", "fonts/score.fnt",
    "fonts/font32.fnt"};

/** A Legends HUD look: the campaign whose art it draws, and the files under that campaign's HUD
 * root it cannot be drawn without; any other art it reads is optional. */
struct LegendsHudLook {
    std::string_view id;
    std::string_view label; ///< the text id of its name on the HUD page
    LegendsPlatform platform;
    std::span<const std::string_view> required;
};

/** Every Legends look, in the order the HUD page lists them. */
constexpr std::array kLooks{
    LegendsHudLook{kLegendsN64HudStyle, kLegendsN64HudLabel, LegendsPlatform::N64, kN64Files},
    LegendsHudLook{kLegendsDreamcastHudStyle, kLegendsDreamcastHudLabel, LegendsPlatform::Dreamcast,
                   kDreamcastFiles}};

/** Opens a look's painter from its campaign's HUD root, taking all its art at once; null when
 * that art does not load. */
using PainterOpener = std::unique_ptr<HudPainter> (*)(RenderDevice& device,
                                                      const std::filesystem::path& root,
                                                      DarkLegacyHudArt& darkLegacy,
                                                      std::span<const HudMember> party);

/** A look this build has a painter for. */
struct BuiltLook {
    std::string_view id;
    PainterOpener open;
};

/** The looks this build has painters for; a look is offered only once it is listed here. */
constexpr std::array<BuiltLook, 0> kBuiltLooks{};

const LegendsHudLook* lookOf(std::string_view id) {
    const auto found = std::ranges::find(kLooks, id, &LegendsHudLook::id);
    return found == kLooks.end() ? nullptr : &*found;
}

const BuiltLook* builtLookOf(std::string_view id) {
    const auto found = std::ranges::find(kBuiltLooks, id, &BuiltLook::id);
    return found == kBuiltLooks.end() ? nullptr : &*found;
}

/** Where `look`'s art is read from, when its campaign is installed with every file the look
 * cannot be drawn without; else what is missing. */
Expected<std::filesystem::path, std::string> foundRoot(const LegendsHudLook& look,
                                                       const LegendsHudRoots& roots) {
    auto root = legendsHudRoot(look.platform, roots);
    if (!root) {
        return gdl::Unexpected(std::string("no extraction of its campaign was found"));
    }
    const AssetLocator files{*root};
    for (const std::string_view name : look.required) {
        if (!nonemptyFile(files, name)) {
            return gdl::Unexpected(
                std::format("{} is missing or empty under {}", name, paths::utf8(*root)));
        }
    }
    return std::move(*root);
}

/** Look `id`'s painter as its opener gives it, null when its art did not load; else why no opener
 * was asked. */
Expected<std::unique_ptr<HudPainter>, std::string>
openedLook(std::string_view id, RenderDevice& device, DarkLegacyHudArt& darkLegacy,
           std::span<const HudMember> party, const LegendsHudRoots& roots) {
    const LegendsHudLook* look = lookOf(id);
    if (look == nullptr) {
        return gdl::Unexpected(std::string("no Legends look has that id"));
    }
    const auto root = foundRoot(*look, roots);
    if (!root) {
        return gdl::Unexpected(root.error());
    }
    const BuiltLook* built = builtLookOf(id);
    if (built == nullptr) {
        return gdl::Unexpected(std::string("this build has no painter for it"));
    }
    return built->open(device, *root, darkLegacy, party);
}

} // namespace

std::optional<std::filesystem::path> legendsHudRoot(LegendsPlatform platform,
                                                    const LegendsHudRoots& roots) {
    const auto maps =
        legendsMaps(platform, roots.assetRoot, roots.executableDirectory, roots.workingDirectory);
    if (maps.empty()) {
        return std::nullopt;
    }
    // Each map's folder is levels/<map> in the extraction.
    return maps.front().directory.parent_path().parent_path();
}

std::span<const std::string_view> legendsHudRequiredFiles(std::string_view id) {
    const LegendsHudLook* look = lookOf(id);
    return look != nullptr ? look->required : std::span<const std::string_view>{};
}

bool legendsHudFilesFound(std::string_view id, const LegendsHudRoots& roots) {
    const LegendsHudLook* look = lookOf(id);
    if (look == nullptr) {
        return false;
    }
    try {
        return foundRoot(*look, roots).has_value();
    } catch (const std::exception& error) {
        log::warn("HUD: the {} look's files could not be looked for: {}", id, error.what());
        return false;
    }
}

std::vector<HudStyleOffer> legendsHudStyles(const LegendsHudRoots& roots) {
    std::vector<HudStyleOffer> offers;
    offers.reserve(kLooks.size());
    for (const LegendsHudLook& look : kLooks) {
        if (builtLookOf(look.id) != nullptr) {
            offers.push_back({.id = std::string(look.id),
                              .label = std::string(look.label),
                              .usable = legendsHudFilesFound(look.id, roots)});
        }
    }
    return offers;
}

std::unique_ptr<HudPainter> openLegendsHud(std::string_view id, RenderDevice& device,
                                           DarkLegacyHudArt& darkLegacy,
                                           std::span<const HudMember> party,
                                           const LegendsHudRoots& roots) {
    std::string cause;
    try {
        auto opened = openedLook(id, device, darkLegacy, party, roots);
        if (opened && *opened) {
            return std::move(*opened);
        }
        cause = opened ? std::string("its art did not load") : opened.error();
    } catch (const std::exception& error) {
        cause = error.what();
    }
    log::warn("HUD: the {} look could not be opened ({}); drawing Dark Legacy's", id, cause);
    return nullptr;
}

} // namespace gdl::game
