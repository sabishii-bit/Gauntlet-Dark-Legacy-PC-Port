#pragma once

#include <optional>

#include "engine/core/Types.h"

#include "game/bosses/BossVictory.h"
#include "game/tower/TowerGateways.h"

namespace gdl::game {

/**
 * A party's way out through Sumner's gateways, seen off as the good wizard sees a party off
 * after a boss: once Sumner's scroll has gone the teleport's sparkle comes on the party, and
 * BossVictory::kExitSparkleTicks later (DoGoodWizard 0x80019090 sparkles the party when 35
 * ticks of its exit timer are left, 0x80343B60) it leaves, the transition coming up over the
 * view. Where it goes once that covers is the application's to make.
 */
class GatewayDeparture {
public:
    enum class Stage : u8 {
        None,      ///< nobody is going
        Asked,     ///< a place is chosen; the party waits for Sumner's scroll to go
        Sparkling, ///< the teleport is on the party, which waits for it
        Gone,      ///< the party has left, and the transition comes up over the view
    };
    /** How long the sparkle shows before the party leaves. */
    static constexpr s32 kSparkleTicks = BossVictory::kExitSparkleTicks;

    /** What an update brings about. */
    struct Step {
        bool sparkle = false; ///< the teleport comes on the party
        bool leave = false;   ///< the party leaves
    };

    /** The party chooses to go to `destination`. */
    void begin(GatewayDestination destination);
    /** Runs `ticks` (60 a second) of the departure, once Sumner's scroll has gone. */
    Step update(s32 ticks);
    /** Calls the journey off: the party stays where it stood. */
    void cancel();

    Stage stage() const { return m_stage; }
    bool active() const { return m_stage != Stage::None; }
    /** Whether the party waits, held still, to be taken: from its choice until it leaves. */
    bool holding() const { return m_stage == Stage::Asked || m_stage == Stage::Sparkling; }
    /** Where the party chose to go, until the journey is called off. */
    const std::optional<GatewayDestination>& destination() const { return m_destination; }
    /** Where the party has left for, once it has gone. */
    std::optional<GatewayDestination> departed() const {
        return m_stage == Stage::Gone ? m_destination : std::nullopt;
    }

private:
    Stage m_stage = Stage::None;
    std::optional<GatewayDestination> m_destination;
    s32 m_ticksLeft = 0;
};

} // namespace gdl::game
