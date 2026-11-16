#pragma once

namespace gdl::game {
class ClassUnlocks;
class Haptics;
class HudStyles;
class PartyStore;

/** The application's owners an online game answers through: this machine's controllers'
 * rumble, the classes a won challenge grants, the local characters' saves and the looks this
 * machine's HUD may be drawn in. Any may be missing; each must outlive whoever holds it. */
struct OnlineLinks {
    Haptics* haptics = nullptr;
    ClassUnlocks* unlocks = nullptr;
    PartyStore* store = nullptr;
    HudStyles* hudStyles = nullptr; ///< without them the HUD is Dark Legacy's
};
} // namespace gdl::game
