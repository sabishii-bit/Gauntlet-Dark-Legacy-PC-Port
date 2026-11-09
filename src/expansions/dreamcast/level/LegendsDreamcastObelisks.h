#pragma once

#include <optional>
#include <string>

#include "expansions/shared/level/LegendsItems.h"
#include "formats/WorldFile.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** Dreamcast's persistent, solid pickup. Its glow, recolouring and rising
 * sprites belong to OBELISK/ONA, not the N64 procedural particle system. */
struct LegendsDreamcastObelisk {
    LegendsPickup pickup;
    Obstacle body;
};

std::optional<LegendsDreamcastObelisk>
legendsDreamcastObelisk(const formats::ItemInfoRecord& info,
                        const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                        std::string* diagnostic = nullptr);

/** do_got_it (8c0aee80) -> AudioLegend (8c0e1180), type 10: S_OBELISKPKUP. */
inline constexpr u32 kDreamcastObeliskSound = 0x5f;

} // namespace gdl::game
