#pragma once
#include <memory>
#include <optional>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/PlayerActor.h"
#include "game/players/PlayerCapture.h"
#include "game/players/TurboMeter.h"
#include "game/players/TurboMove.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
enum class PlayerLife : u8 { Standing, Dying, InTower };

/** Everything that belongs to one participant for this level. The player id may
 * differ from this record's position in the party; figures may be unavailable. */
struct PlayerRuntime {
    PlayerActor actor;
    PlayerCapture capture;
    std::unique_ptr<PlayerFigure> figure; ///< null when character assets are unavailable
    std::optional<usize> slot;            ///< persistent save slot, not the input player id
    CharacterSave entrySave;              ///< restored when a fallen character leaves the level
    PlayerLife life = PlayerLife::Standing;
    f32 painOwed = 0.0f;                    ///< accumulated damage not yet answered by a cry
    s32 hitSoundGap = 0;                    ///< ticks before another impact sound
    PlayerDeed reaction = PlayerDeed::None; ///< hit or gesture requested for the next update
    TurboMeter turbo;
    std::vector<s32> helpHeard; ///< since the character was loaded, distinct from saved help
    TurboMove move;
    std::vector<usize> rammed; ///< barrels already hit by the current charge
    f32 blockLeft = 0.0f;      ///< seconds before another block effect
    f32 cloudGap = 0.0f;       ///< seconds before gas can harm this participant again
    f32 breathGap = 0.0f;      ///< shared across all creatures' breath, not reset by a new move
    f32 effectGap = 0.0f;      ///< shared attached-area damage gate, independent of breath
};

} // namespace gdl::game
