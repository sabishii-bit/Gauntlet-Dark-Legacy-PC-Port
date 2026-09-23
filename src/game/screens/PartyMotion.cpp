#include "game/screens/PartyMotion.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"
namespace gdl::game {
namespace {
constexpr f32 kChargeStick = 0.25f;
}
PlayerDeed PartyMotion::turboDeed(const PlayerRuntime& runtime, const PlayInput& in) {
    if (runtime.figure == nullptr) {
        return PlayerDeed::None;
    }
    const TurboMeter& meter = runtime.turbo;
    PlayerDeed deed = PlayerDeed::None;
    if (in.turboAttackPressed) {
        if (meter.held() >= TurboMeter::kFullCost) {
            deed = PlayerDeed::TurboFull;
        } else if (meter.held() >= TurboMeter::kStrongCost) {
            deed = PlayerDeed::TurboStrong;
        }
    } else if (in.chargePressed && meter.held() >= TurboMeter::kShoveFrom) {
        deed = PlayerDeed::Shove;
    }
    return runtime.figure->animator().canBegin(deed) ? deed : PlayerDeed::None;
}

MoveInput PartyMotion::chargeInput(const PlayerActor& actor, const MoveInput& stick,
                                   f32 cameraYaw) {
    MoveInput rush;
    rush.magnitude = 1.0f;
    if (stick.magnitude >= kChargeStick) {
        rush.direction = stick.direction;
        return rush;
    }
    const Vec3 facing = actor.facing();
    const f32 ahead = std::atan2(facing.x, facing.z) - cameraYaw;
    rush.direction = Vec2{std::sin(ahead), std::cos(ahead)};
    return rush;
}

StrafeWay PartyMotion::strafeWayOf(f32 heading, f32 facing) {
    constexpr f32 kEighth = 0.7853982f;
    const f32 off = std::remainder(heading - facing, 8.0f * kEighth);
    if (std::abs(off) > 3.0f * kEighth) {
        return StrafeWay::Back;
    }
    if (off > kEighth) {
        return StrafeWay::Right; // the original's sense of a positive turn
    }
    return off < -kEighth ? StrafeWay::Left : StrafeWay::Forward;
}

std::vector<CameraSubject> PartyMotion::step(std::span<PlayerRuntime> players,
                                             std::span<const PlayInput> inputs, bool held,
                                             f32 cameraYaw, s32 ticks, f32 seconds,
                                             const WorldCollision& collision,
                                             const Events& events) {
    // Snapshot after movement, before fixture collision, preserving the camera's frame phase.
    std::vector<CameraSubject> subjects;
    subjects.reserve(players.size());
    for (usize i = 0; i < players.size(); ++i) {
        PlayerActor& actor = players[i].actor;
        const auto player = static_cast<usize>(actor.player());
        const bool down = players[i].life != PlayerLife::Standing;
        if (down) {
            players[i].capture.clear();
        } else if (players[i].capture.active()) {
            PlayerCapture& capture = players[i].capture;
            const PlayerDeed deed = capture.held() ? PlayerDeed::Grabbed : PlayerDeed::Thrown;
            if (const auto impact = capture.update(seconds, actor, collision);
                impact.has_value() && events.thrownImpact) {
                events.thrownImpact(i, *impact);
            }
            players[i].rammed.clear();
            if (players[i].figure != nullptr) {
                players[i].figure->animate(
                    0, ticks, seconds,
                    players[i].life == PlayerLife::Standing ? deed : PlayerDeed::Die);
            }
            subjects.push_back({actor.position(), actor.followPoint()});
            continue;
        }
        // Reeling from a hit, a character neither moves nor does anything.
        const bool reeling =
            players[i].reaction != PlayerDeed::None ||
            (players[i].figure != nullptr && players[i].figure->animator().reacting());
        const bool entering =
            players[i].figure != nullptr && players[i].figure->animator().entering();
        const MoveInput& move = !held && !down && !reeling && !entering && player < inputs.size()
                                    ? inputs[player].move
                                    : MoveInput{};
        // What the buttons ask: a potion first, when one is carried, then the attack.
        PlayerDeed deed = down ? PlayerDeed::Die : PlayerDeed::None;
        if (!down && players[i].reaction != PlayerDeed::None) {
            deed = players[i].reaction;
        }
        players[i].reaction = PlayerDeed::None;
        if (!held && !down && !reeling && !entering && player < inputs.size()) {
            const PlayInput& in = inputs[player];
            const bool carrying = !actor.save().progress().inventory.potions.empty();
            if ((in.usePotion || in.throwPotion) && !carrying) {
                events.perform(i, Action::NoPotion);
            }
            if ((in.shieldPotion) && !carrying) {
                events.perform(i, Action::NoPotion);
            }
            if (const PlayerDeed turbo = turboDeed(players[i], in); turbo != PlayerDeed::None) {
                deed = turbo;
            } else if (in.shieldPotion && carrying) {
                deed = PlayerDeed::ShieldPotion;
            } else if (in.usePotion && carrying) {
                deed = PlayerDeed::UsePotion;
            } else if (in.throwPotion && carrying) {
                deed = PlayerDeed::ThrowPotion;
            } else if (in.strongAttack && players[i].figure != nullptr &&
                       players[i].figure->animator().canBegin(PlayerDeed::StrongAttack)) {
                deed = PlayerDeed::StrongAttack; // nothing is ever in reach yet: the strong throw
            } else if (in.turbo) {
                deed = PlayerDeed::Defend; // held by itself, the turbo button is the guard
            } else if (in.attack) {
                deed = PlayerDeed::Attack;
            }
            events.select(i, in.selector, ticks);
        }
        actor.setPaceBonus(PowerupEffects::of(actor.save().progress().inventory).paceAdd);
        // A body in a throw keeps its feet where they are, turning to the stick.
        const f32 pace =
            players[i].figure != nullptr ? players[i].figure->animator().moveScale() : 1.0f;
        const bool charging =
            players[i].figure != nullptr && players[i].figure->animator().shoving();
        // Strafing, the character steps the way the stick is pushed without turning to it.
        const bool strafes = !held && !down && !charging && player < inputs.size() &&
                             inputs[player].strafe && move.any();
        if (players[i].figure != nullptr) {
            players[i].figure->setStrafe(
                strafes ? strafeWayOf(PlayerActor::headingOf(move, cameraYaw), actor.yaw())
                        : StrafeWay::None);
        }
        const Vec3 before = actor.position();
        actor.update(charging ? chargeInput(actor, move, cameraYaw) : move, cameraYaw, seconds,
                     &collision, pace, strafes);
        if (events.allowMovement && !events.allowMovement(before, actor.position())) {
            actor.place(Vec3{before.x, actor.position().y, before.z});
        }
        // Stationary normal attacks face the assisted target. The stick, strafe,
        // charging and authored turbo movement retain control of their heading.
        if (!move.any() && !charging && player < inputs.size() && !inputs[player].strafe &&
            (deed == PlayerDeed::Attack || deed == PlayerDeed::StrongAttack) && events.aim) {
            if (const auto target = events.aim(i)) {
                actor.faceToward(*target);
            }
        }
        if (charging) {
            events.perform(i, Action::Ram);
        } else {
            players[i].rammed.clear();
        }
        if (players[i].figure != nullptr) {
            players[i].figure->animate(move.magnitude, ticks, seconds, deed);
            events.advanceTurbo(i, ticks, seconds);
            if (players[i].life == PlayerLife::Dying && players[i].figure->animator().dead()) {
                players[i].life = PlayerLife::InTower; // the body goes; its box says where
            }
            if (players[i].figure->animator().released()) {
                events.perform(i, Action::ThrowWeapon);
            }
            if (players[i].figure->animator().strongReleased()) {
                events.perform(i, Action::StrongThrow);
            }
            players[i].blockLeft = std::max(players[i].blockLeft - seconds, 0.0f);
            if (players[i].figure->animator().potionShielded()) {
                events.perform(i, Action::ShieldPotion);
            }
            if (players[i].figure->animator().potionUsed()) {
                events.perform(i, Action::UsePotion);
            } else if (players[i].figure->animator().potionThrown()) {
                events.perform(i, Action::ThrowPotion);
            }
            if (const PlayerAnimator::Foot foot = players[i].figure->animator().footfall();
                foot != PlayerAnimator::Foot::None) {
                events.perform(i, foot == PlayerAnimator::Foot::Second ? Action::SecondFoot
                                                                       : Action::FirstFoot);
            }
        }
        if (players[i].figure == nullptr && players[i].life == PlayerLife::Dying) {
            players[i].life = PlayerLife::InTower; // nothing to play: gone at once
        }
        subjects.push_back(CameraSubject{actor.position(), actor.followPoint()});
    }
    return subjects;
}
} // namespace gdl::game
