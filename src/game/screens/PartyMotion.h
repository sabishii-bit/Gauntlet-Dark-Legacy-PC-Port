#pragma once
#include <functional>
#include <span>
#include <vector>

#include "game/menu/MenuInput.h"
#include "game/screens/PlayerRuntime.h"
#include "game/screens/PowerupSelector.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {
/** One player's input for a frame of play. */
struct PlayInput {
    MoveInput move;
    MenuInput menu;
    bool attack = false; ///< the attack button is held
    bool usePotion = false;
    bool throwPotion = false;
    bool shieldPotion = false;  ///< the shield potion button is held
    bool strafe = false;        ///< the strafe button is held
    bool strongAttack = false;  ///< the slow attack button is held
    bool turbo = false;         ///< the turbo button is held
    bool chargePressed = false; ///< the charge button went down this frame
    bool attackPressed = false; ///< the attack button went down this frame
    SelectorInput selector;     ///< this frame's presses for the powerup selector
};

/** Advances player input, locomotion and animation, independently of scene/world orchestration.
 * Events are delivered synchronously at their animation phase, before each camera snapshot.
 * Callbacks must not resize or replace the supplied party. No scene or callbacks are retained. */
class PartyMotion {
public:
    enum class Action : u8 {
        NoPotion,
        Ram,
        ThrowWeapon,
        StrongThrow,
        ShieldPotion,
        UsePotion,
        ThrowPotion,
        FirstFoot,
        SecondFoot
    };
    struct Events {
        std::function<void(usize, Action)> perform;
        std::function<void(usize, const SelectorInput&, s32)> select;
        std::function<void(usize, s32, f32)> advanceTurbo;
    };
    static std::vector<CameraSubject> step(std::span<PlayerRuntime> players,
                                           std::span<const PlayInput> inputs, bool held,
                                           f32 cameraYaw, s32 ticks, f32 seconds,
                                           const WorldCollision& collision, const Events& events);
    static StrafeWay strafeWayOf(f32 heading, f32 facing);
    static MoveInput chargeInput(const PlayerActor& actor, const MoveInput& stick, f32 cameraYaw);
    static PlayerDeed turboDeed(const PlayerRuntime& runtime, const PlayInput& input);
};
} // namespace gdl::game
