#pragma once
#include <memory>
#include <optional>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/Knockback.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerCapture.h"
#include "game/players/PlayerTransport.h"
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
    PlayerTransport transport;
    std::unique_ptr<PlayerFigure> figure; ///< null when character assets are unavailable
    std::optional<usize> slot;            ///< persistent save slot, not the input player id
    CharacterSave entrySave;              ///< restored when a fallen character leaves the level
    s32 levelKills = 0;                   ///< creatures and generators credited during this level
    PlayerLife life = PlayerLife::Standing;
    bool towerPrompt = false;        ///< fallen outside the tower, asked to wait there or quit
    bool departed = false;           ///< quit the game from that prompt: out of the party
    f32 painOwed = 0.0f;             ///< accumulated damage not yet answered by a cry
    s32 hitSoundGap = 0;             ///< ticks before another impact sound
    s32 heartbeatTicks = 0;          ///< ticks before the low-health heart beats again
    std::optional<Vec3> fixtureSpot; ///< where the fixtures last saw it, for its step
    f32 surfaceGap = 0.0f;           ///< seconds before a harmful surface can hurt it again
    s32 hitFlashTicks = 0;           ///< two 30 Hz frames of the white damage skin
    s32 itTicks = 0;                 ///< since IT tagged this player; none when not it
    PlayerDeed gesture = PlayerDeed::None; ///< a pickup's or bad food's, played when free
    f32 gagSeconds = 0.0f; ///< retching at the stick let go for this long (field_898)
    PlayerDeed reaction = PlayerDeed::None; ///< hit or gesture requested for the next update
    Knockback knockback;                    ///< the pushes of this frame's hits, and the slide
    TurboMeter turbo;
    std::vector<s32> helpHeard; ///< since the character was loaded, distinct from saved help
    TurboMove move;
    std::vector<usize> rammed; ///< barrels already hit by the current charge
    f32 blockLeft = 0.0f;      ///< seconds before another block effect
    bool blocked = false;      ///< its guard has shown a block this level (hud_flags 0x2000)
    f32 cloudGap = 0.0f;       ///< seconds before gas can harm this participant again
    f32 breathGap = 0.0f; ///< the great ones' shared hit gap (fxhittime): breath and blows alike
    f32 effectGap = 0.0f; ///< shared attached-area damage gate, independent of breath
};

} // namespace gdl::game
