#pragma once
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/party/PlayerRuntime.h"
#include "game/players/ComboMove.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

/**
 * How the party's two-player combos are carried out each step (PlayerMotion's grab and
 * combo passes): who may take hold of whom, where a held body rides (its partner's DUMMY
 * node), how a warrior's pinball flies and a dwarf's charger is steered, and when the pair
 * are let go. `ComboMove` decides; this applies its orders to the actors and figures. It
 * keeps nothing and knows no scene: what a flier strikes is asked of the level.
 */
class PartyCombo {
public:
    /** What a combo's flier or charger asks of the level as it goes; answered at once. */
    class Level {
    public:
        virtual ~Level() = default;
        /** A flier or charger (its index, its thrower's, the blow it deals) against the
         * level's items: true when it struck one, which turns it. */
        virtual bool comboImpact(usize body, usize thrower, f32 blow) = 0;
        /** Runs member `index`'s turbo move on through the step. */
        virtual void advanceTurbo(usize index, s32 ticks, f32 seconds) = 0;
        /** The shared view's limit on a step (camera_debug_supervisor): where `after` may
         * end, given `before`; a pinball takes the view's edge for a hit. */
        virtual Vec3 limitMovement(usize index, const Vec3& before, const Vec3& after) const = 0;
    };

    /** Whom `index` may take hold of now: the nearest other standing member ahead who may be
     * taken (fn_80088EF4); none when nobody may, or `index` is not clear to take hold. Its
     * own action is not asked: whether its move may begin is for its animator to say. */
    static std::optional<usize> partnerFor(std::span<const PlayerRuntime> players, usize index);
    /** Ties the pair together as the grabber's move begins. */
    static void begin(std::span<PlayerRuntime> players, usize grabber, usize partner);
    /** Releases both sides of an invalid pair, restoring attached bodies before normal motion. */
    static void cancelInvalid(std::span<PlayerRuntime> players);
    /** Whether the combo moves this body instead of its own walking: it rides, is held,
     * flies or is steered. */
    static bool aside(const PlayerRuntime& runtime);
    /** What the body plays this step: held, thrown, or nothing of the combo's. */
    static PlayerDeed deedOf(const PlayerRuntime& runtime);
    /** Animates a body the combo has aside: its figure with the combo's deed, its turbo rows,
     * and for a grabber the pair's orders. */
    static void animate(std::span<PlayerRuntime> players, usize index, s32 ticks, f32 seconds,
                        Level& level);
    /** One tick of a grabber's move once its figure has animated: attachments, releases and
     * the end of the combo, applied to both bodies. */
    static void advance(std::span<PlayerRuntime> players, usize grabber, s32 ticks);
    /** A warrior's pinball for the frame: thirty a second along its facing, turned three
     * eighths round by an item or another member (not its thrower for a while), reflected
     * off walls, once in ten ticks. */
    static void fly(std::span<PlayerRuntime> players, usize flier, s32 ticks, f32 seconds,
                    const WorldCollision& collision, Level& level);
    /** A dwarf's charger for the frame: the dwarf's stick at half again the pace, turning
     * half way a frame, turned round by an item it strikes. */
    static void ride(std::span<PlayerRuntime> players, usize charger, const MoveInput& stick,
                     f32 cameraYaw, s32 ticks, f32 seconds, const WorldCollision& collision,
                     Level& level);
    /** Sets every rider on its carrier's DUMMY node, once every figure has animated. */
    static void carry(std::span<PlayerRuntime> players);
    /** Where a rider hangs: its carrier's DUMMY node, else the carrier itself. */
    static Mat4 seatOf(const PlayerRuntime& carrier);
    static constexpr f32 kTurnFrames = 30.0f; ///< the rate a partial turn is taken at
};

} // namespace gdl::game
