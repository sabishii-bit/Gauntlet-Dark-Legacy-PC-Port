#pragma once
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/camera/TowerCamera.h"
#include "game/party/PartyCombo.h"
#include "game/party/PlayInput.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::game {
/** Advances player input, locomotion and animation, independently of scene/world orchestration.
 * What it asks of the level is asked synchronously at its animation phase, before each camera
 * snapshot. The level must not resize or replace the supplied party, and is not retained. */
class PartyMotion {
public:
    enum class Action : u8 {
        NoPotion,
        Ram,
        ThrowWeapon,
        FamiliarShot,
        StrongThrow,
        SuperShot,
        ItemAttack,
        ShieldPotion,
        UsePotion,
        ThrowPotion,
        FirstFoot,
        SecondFoot,
        Melee,
        Fallen,    ///< its death played out, it is gone from the level
        Tagged,    ///< it is now it, touched by the one who was
        ComboStart ///< it has taken hold of its combo partner (StartComboFX's sphere and burst)
    };
    /** What the party's movement asks of the level, each at its phase of the step and before
     * the camera snapshot; every answer is used at once. Must not resize or replace the
     * party. */
    class Level : public PartyCombo::Level {
    public:
        /** Carries out what member `index`'s movement set off: a throw, a swing, a potion, a
         * footstep. */
        virtual void perform(usize index, Action action) = 0;
        /** Steps member `index`'s powerup selector by the input meant for it. */
        virtual void stepSelector(usize index, const SelectorInput& input, s32 ticks) = 0;
        /** The harm a thrown body takes as it lands. */
        virtual void thrownImpact(usize index, f32 damage) = 0;
        /** What a standing attack turns to face, when anything. */
        virtual std::optional<Vec3> aim(usize index) const = 0;
        /** What the attack buttons ask: strong or not, and whether the stick moves. */
        virtual PlayerDeed attackDeed(usize index, bool strong, bool moved,
                                      const Vec3& facing) const = 0;
        /** A walking/running contact without pressing attack, along the requested heading. */
        virtual PlayerDeed automaticMeleeDeed(usize index, const Vec3& facing) const = 0;
        /** Where the nearest thing to strike lies, an attack button held or not; none leaves
         * the figure's sense as it was. */
        virtual std::optional<MeleeSense> meleeSense(usize index, bool held,
                                                     const Vec3& facing) const = 0;
        /** Pure contact query before movement; a current hold suppresses attack input. */
        virtual std::optional<Vec3> deathContact(usize index) const = 0;
        /** Where the Death a halo wearer holds stands, when one is held this step (ticks, and
         * whether a hold may be made at all): holds, drains or lets go of him. Asked once,
         * after resolved movement. */
        virtual std::optional<Vec3> grabDeath(usize index, s32 ticks, bool allowed) = 0;
        /** Whether automatic melee counts held close swings toward heavy finishers. */
        virtual bool chainsHeldMelee() const = 0;
        /** Dynamic creature collision, before the camera limit and action events. */
        virtual Vec3 resolveMovement(usize index, const Vec3& from, const Vec3& to) const = 0;
        /** Where a body fallen out of the world with nobody to stand beside goes: the
         * level's start, when there is one. */
        virtual std::optional<Vec3> startPoint() const = 0;
        /** Whether the developer console holds a player's body aloft (noclip): it then
         * rides no floor, collides with nothing and never falls. */
        virtual bool flying(usize index) const = 0;
    };
    static constexpr f32 kLostDepth = 4.5f;   ///< under the world's lowest point a body is lost
    static constexpr f32 kTurnFrames = 30.0f; ///< the rate a partial turn is taken at, a second
    static constexpr s32 kItHold = 60; ///< ticks it stays with a player before a touch passes it
    static constexpr s32 kRescueSpots = 16;
    static constexpr f32 kRescueGap = 0.5f;  ///< how far from the rescuer's side it stands
    static constexpr f32 kRescueRise = 6.0f; ///< how far above or below the rescuer's floor
    /** Where a body lost out of the world stands again (get_player_pos): on one of sixteen
     * spots round another standing player, just clear of them, where there is floor near
     * theirs and no wall; nullopt with none. */
    static std::optional<Vec3> rescueSpot(std::span<const PlayerRuntime> players, usize lost,
                                          const WorldCollision& collision);
    /** Moves the party for the tick. `cameraYaws` holds, per player in `players`, the yaw
     * of the camera their stick is read against: one shared yaw for every player, or each
     * one's own camera's under solo cameras. */
    static std::vector<CameraSubject> step(std::span<PlayerRuntime> players,
                                           std::span<const PlayInput> inputs, bool held,
                                           std::span<const f32> cameraYaws, s32 ticks, f32 seconds,
                                           const WorldCollision& collision, Level& level);
    /** The same with one camera yaw for the whole party. */
    static std::vector<CameraSubject> step(std::span<PlayerRuntime> players,
                                           std::span<const PlayInput> inputs, bool held,
                                           f32 cameraYaw, s32 ticks, f32 seconds,
                                           const WorldCollision& collision, Level& level);
    static StrafeWay strafeWayOf(f32 heading, f32 facing);
    /** Native action pacing before the current tick changes its animation. */
    static f32 actionPace(const PlayerAnimator* animator, PlayerDeed deed, bool webbed = false);
    /** Native partial turning and stick-released lunge intent. Pure planning:
     * callers still resolve the resulting step against their own world state. */
    static MoveInput attackMove(const PlayerActor& actor, const PlayerAnimator* animator,
                                const MoveInput& move, PlayerDeed deed, f32 cameraYaw, f32 seconds,
                                bool mayLunge, bool steeringTurbo = false);
    static MoveInput chargeInput(const PlayerActor& actor, const MoveInput& stick, f32 cameraYaw);
    static PlayerDeed turboDeed(const PlayerRuntime& runtime, const PlayInput& input);
    /** Native meter/button priority without inventory or scene access. */
    static PlayerDeed turboDeed(const PlayerAnimator& animator, f32 meter, const PlayInput& input,
                                bool pojo = false);
    /** Turbo turning uses an angular-speed limit, including cursor aiming. */
    static f32 turboHeading(f32 current, f32 wanted, f32 seconds, f32 turnScale);
    /** Passes IT at the step's resolved player contacts after a second of possession;
     * the fallen are it no more. Contacts are indexed by party member, not input lane. */
    static void passIt(std::span<PlayerRuntime> players, s32 ticks, Level& level,
                       std::span<const std::optional<usize>> contacts);
};
} // namespace gdl::game
