#pragma once

#include <array>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

/** Shared N64 Tower presentation derived from the attending party's N64
 * collection. No GDL relics, Dreamcast records or completed-map guesses enter
 * these predicates. Door order is Town, Castle, Ice, Battlefield. */
struct LegendsTowerProgress {
    /** The obelisks each door wants, native obelisk N at bit N-1 (e0054fdc). */
    static constexpr std::array<u8, 4> kDoorObelisks{0x60, 0x07, 0x18, 0x80};

    u16 runestones = 0;
    u8 shards = 0;
    std::array<bool, 4> doors{};
    bool cathedralBeam = false;
    bool runestonePortal = false;

    static LegendsTowerProgress of(const LegendsProgress& progress);
    /** Native drawable transparency208 until the complete four-piece window. */
    f32 shardOpacity() const;
    bool operator==(const LegendsTowerProgress&) const = default;
};

} // namespace gdl::game
