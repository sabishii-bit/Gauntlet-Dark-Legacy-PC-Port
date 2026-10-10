#pragma once
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/screens/PlayInput.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {
/** Advances player input, locomotion and animation, independently of scene/world orchestration.
 * Events are delivered synchronously at their animation phase, before each camera snapshot.
 * Callbacks must not resize or replace the supplied party. No scene or callbacks are retained. */
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
    struct Events {
        std::function<void(usize, Action)> perform;
        std::function<void(usize, const SelectorInput&, s32)> select;
        std::function<void(usize, s32, f32)> advanceTurbo;
        std::function<void(usize, f32)> thrownImpact;
        std::function<std::optional<Vec3>(usize)> aim;
        /** Shared-view tangent projection, rechecked against world and creature collision. */
        std::function<Vec3(usize, const Vec3&, const Vec3&)> limitMovement;
        /** What the attack buttons ask: strong or not, and whether the stick moves. */
        std::function<PlayerDeed(usize, bool, bool, const Vec3&)> attackDeed;
        /** A walking/running contact without pressing attack, along the requested heading. */
        std::function<PlayerDeed(usize, const Vec3&)> automaticMeleeDeed;
        /** Where the nearest thing to strike lies, an attack button held or not. */
        std::function<MeleeSense(usize, bool, const Vec3&)> meleeSense;
        /** Pure contact query before movement; a current hold suppresses attack input. */
        std::function<std::optional<Vec3>(usize)> deathContact;
        /** Where the Death a halo wearer holds stands, when one is held this step (ticks, and
         * whether a hold may be made at all). Applied once, after resolved movement. */
        std::function<std::optional<Vec3>(usize, s32, bool)> grabDeath;
        /** Dynamic creature collision, before the camera limit and action events. */
        std::function<Vec3(usize, const Vec3&, const Vec3&)> resolveMovement;
        /** Where a body fallen out of the world with nobody to stand beside goes: the
         * level's start, when there is one. */
        std::function<std::optional<Vec3>()> startPoint;
        /** A combo's flier or charger (its index, its thrower's, the blow it deals) against
         * the level's items; true when it struck one, which turns it. */
        std::function<bool(usize, usize, f32)> comboImpact;
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
    static std::vector<CameraSubject> step(std::span<PlayerRuntime> players,
                                           std::span<const PlayInput> inputs, bool held,
                                           f32 cameraYaw, s32 ticks, f32 seconds,
                                           const WorldCollision& collision, const Events& events);
    static StrafeWay strafeWayOf(f32 heading, f32 facing);
    static MoveInput chargeInput(const PlayerActor& actor, const MoveInput& stick, f32 cameraYaw);
    static PlayerDeed turboDeed(const PlayerRuntime& runtime, const PlayInput& input);
    /** Passes IT at the step's resolved player contacts after a second of possession;
     * the fallen are it no more. Contacts are indexed by party member, not input lane. */
    static void passIt(std::span<PlayerRuntime> players, s32 ticks, const Events& events,
                       std::span<const std::optional<usize>> contacts);
};
} // namespace gdl::game
