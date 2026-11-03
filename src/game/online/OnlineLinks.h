#pragma once

namespace gdl::game {
class ClassUnlocks;
class Haptics;
class PartyStore;

/** The application's owners an online game answers through: this machine's controllers'
 * rumble, the classes a won challenge grants and the local characters' saves. Any may be
 * missing; each must outlive whoever holds it. */
struct OnlineLinks {
    Haptics* haptics = nullptr;
    ClassUnlocks* unlocks = nullptr;
    PartyStore* store = nullptr;
};
} // namespace gdl::game
