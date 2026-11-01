#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "game/level/ImportedGenerators.h"

namespace gdl::game {

/** Native placement/art with explicitly translated GDL brood mechanics. State zero
 * is the authored ruin, or empty when the native generator disappears entirely. */
struct LegendsGenerator {
    ImportedGenerator gameplay;
    std::array<std::string, 4> sequences;
};

/** Stage species banks whose GENERATOR groups are part of the native fixture
 * scope. Mountain/Ice generator art lives in items/<realm> instead. */
std::vector<std::string> legendsN64GeneratorArchives(std::string_view levelDirectory);

/** N64's level roster remaps generic small/medium generators before their PGEn
 * template is selected. Unknown kinds are reported, never replaced by grunts.
 * Non-generator fixtures return nothing without a warning. */
std::optional<LegendsGenerator>
legendsN64Generator(const formats::LegendsN64Fixture& fixture,
                    std::span<const u8> generatorTemplates, std::string_view levelDirectory,
                    std::span<const formats::LegendsMapCollisionTriangle> collision,
                    std::string* unsupported = nullptr);

} // namespace gdl::game
