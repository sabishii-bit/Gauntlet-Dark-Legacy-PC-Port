#pragma once
#include <memory>
#include <optional>
#include <vector>

#include "game/players/PlayerActor.h"
#include "game/players/TurboMeter.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
enum class PlayerLife : u8 { Standing, Dying, InTower };

/** A turbo move under way: the strikes it has yet to make and what it has yet to pay. */
struct MoveProgress {
    std::vector<s32> pending;
    std::vector<s32> all; ///< every strike of it, which may keep the level dark
    f32 owed = 0.0f;
    bool named = false;           ///< its name has been announced
    bool weaponHidden = false;    ///< one of its strikes empties the hand for now
    std::vector<s32> volleysShot; ///< per strike of `all`, how many shots it has let fly
};
/** Everything that belongs to one participant for this level. The player id may
 * differ from this record's position in the party; figures may be unavailable. */
struct PlayerRuntime {
    PlayerActor actor;
    std::unique_ptr<PlayerFigure> figure; ///< null when character assets are unavailable
    std::optional<usize> slot;            ///< persistent save slot, not the input player id
    CharacterSave entrySave;              ///< restored when a fallen character leaves the level
    PlayerLife life = PlayerLife::Standing;
    f32 painOwed = 0.0f;                    ///< accumulated damage not yet answered by a cry
    s32 hitSoundGap = 0;                    ///< ticks before another impact sound
    PlayerDeed reaction = PlayerDeed::None; ///< hit or gesture requested for the next update
    TurboMeter turbo;
    std::vector<s32> helpHeard; ///< since the character was loaded, distinct from saved help
    MoveProgress move;
    std::vector<usize> rammed; ///< barrels already hit by the current charge
    f32 blockLeft = 0.0f;      ///< seconds before another block effect
    f32 cloudGap = 0.0f;       ///< seconds before gas can harm this participant again
};

} // namespace gdl::game
