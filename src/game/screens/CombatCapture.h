#pragma once

#include "game/enemies/Enemies.h"
#include "game/netplay/CombatSnapshot.h"
#include "game/screens/PlayerRuntime.h"

namespace gdl::game {
class PortalDeparture;
class LevelWorld;

/** Capture after an authoritative simulation tick. Does not drain enemy feedback,
 * invoke damage/AI, alter character saves or advance animation. motion must be
 * from this same party/tick. Epoch boundaries scope all actor identities. */
class CombatCapture {
public:
    static std::optional<CombatSnapshot> capture(const MotionSnapshot& motion,
                                                 std::span<const PlayerRuntime> players,
                                                 const Enemies& enemies,
                                                 const PortalDeparture* departure = nullptr,
                                                 const LevelWorld* world = nullptr);
};

} // namespace gdl::game
