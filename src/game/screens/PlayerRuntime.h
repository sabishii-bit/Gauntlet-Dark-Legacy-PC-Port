#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "game/players/PlayerActor.h"
#include "game/players/TurboMeter.h"
#include "game/players/TurboMove.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
enum class PlayerLife : std::uint8_t { Standing, Dying, InTower };

/** Everything that belongs to one participant for this level. The player id may
 * differ from this record's position in the party; figures may be unavailable. */
struct PlayerRuntime {
    PlayerActor actor;
    std::unique_ptr<PlayerFigure> figure; ///< null when character assets are unavailable
    std::optional<std::size_t> slot;      ///< persistent save slot, not the input player id
    CharacterSave entrySave;              ///< restored when a fallen character leaves the level
    PlayerLife life = PlayerLife::Standing;
    float painOwed = 0.0f;                  ///< accumulated damage not yet answered by a cry
    std::int32_t hitSoundGap = 0;           ///< ticks before another impact sound
    PlayerDeed reaction = PlayerDeed::None; ///< hit or gesture requested for the next update
    TurboMeter turbo;
    std::vector<std::int32_t>
        helpHeard; ///< since the character was loaded, distinct from saved help
    TurboMove move;
    std::vector<std::size_t> rammed; ///< barrels already hit by the current charge
    float blockLeft = 0.0f;          ///< seconds before another block effect
    float cloudGap = 0.0f;           ///< seconds before gas can harm this participant again
};

} // namespace gdl::game
