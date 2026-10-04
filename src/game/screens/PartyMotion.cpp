#include "game/screens/PartyMotion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

#include "game/players/ComboMove.h"
#include "game/players/CursorAim.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PartyCollision.h"
#include "game/screens/PartyCombo.h"
namespace gdl::game {
namespace {
constexpr f32 kChargeStick = 0.25f;
/** A shoved body moving less than this a second shows no push (0.01 a 30 Hz frame,
 * pmotion.c 2042). */
constexpr f32 kPushedStep = 0.3f;
} // namespace
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

std::optional<Vec3> PartyMotion::rescueSpot(std::span<const PlayerRuntime> players, usize lost,
                                            const WorldCollision& collision) {
    constexpr f32 kTurn = 6.2831853f;
    if (lost >= players.size()) {
        return std::nullopt;
    }
    const PlayerActor& body = players[lost].actor;
    for (usize other = 0; other < players.size(); ++other) {
        if (other == lost || players[other].life != PlayerLife::Standing) {
            continue;
        }
        const PlayerActor& rescuer = players[other].actor;
        if (rescuer.position().y <= collision.lowest() - kLostDepth) {
            continue; // lost too
        }
        const f32 out = kRescueGap + rescuer.radius() + body.radius();
        for (s32 k = 0; k < kRescueSpots; ++k) {
            const f32 angle = kTurn * static_cast<f32>(k) / static_cast<f32>(kRescueSpots);
            Vec3 spot = rescuer.position() + Vec3{std::sin(angle), 0.0f, std::cos(angle)} * out;
            const auto floor = collision.floorAt(spot, kRescueRise, kRescueRise);
            if (!floor.has_value()) {
                continue;
            }
            spot.y = floor->y;
            const Vec3 clear =
                collision.resolveWalls(spot, body.radius(), spot.y + PlayerActor::kFootClearance,
                                       spot.y + body.height() - PlayerActor::kFootClearance);
            if (glm::distance(clear, spot) < 1e-3f) {
                return spot;
            }
        }
    }
    return std::nullopt;
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

void PartyMotion::passIt(std::span<PlayerRuntime> players, s32 ticks, const Events& events,
                         std::span<const std::optional<usize>> contacts) {
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& it = players[i];
        if (it.itTicks <= 0) {
            continue;
        }
        if (it.life != PlayerLife::Standing || it.departed) {
            it.itTicks = 0;
            continue;
        }
        if (it.itTicks > kItHold && i < contacts.size() && contacts[i].has_value()) {
            const usize j = *contacts[i];
            if (j < players.size() && j != i && players[j].life == PlayerLife::Standing &&
                !players[j].departed) {
                players[j].itTicks = 1;
                it.itTicks = 0;
                if (events.perform) {
                    events.perform(j, Action::Tagged);
                }
            }
        }
        if (it.itTicks > 0) {
            it.itTicks += ticks;
        }
    }
}

std::vector<CameraSubject> PartyMotion::step(std::span<PlayerRuntime> players,
                                             std::span<const PlayInput> inputs, bool held,
                                             f32 cameraYaw, s32 ticks, f32 seconds,
                                             const WorldCollision& collision,
                                             const Events& events) {
    // Snapshot after movement, before fixture collision, preserving the camera's frame phase.
    PartyCombo::cancelInvalid(players);
    std::vector<CameraSubject> subjects;
    subjects.reserve(players.size());
    std::vector<std::optional<usize>> contacts(players.size());
    for (usize i = 0; i < players.size(); ++i) {
        players[i].hitFlashTicks = std::max(0, players[i].hitFlashTicks - ticks);
        PlayerActor& actor = players[i].actor;
        players[i].cursorAiming = false;
        actor.clearWallContacts();
        const auto player = static_cast<usize>(actor.player());
        const bool down = players[i].life != PlayerLife::Standing;
        if (down) {
            players[i].capture.clear();
            players[i].transport.clear();
            players[i].knockback.clear();
        } else if (players[i].transport.active()) {
            players[i].knockback.clear();
            actor.update({}, cameraYaw, seconds, nullptr);
            if (players[i].figure != nullptr) {
                players[i].figure->animate(0, ticks, seconds, PlayerDeed::None);
            }
            subjects.push_back({actor.position(), actor.followPoint()});
            continue;
        } else if (players[i].capture.active()) {
            players[i].knockback.clear();
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
        // A body its partner's combo has aside goes where the combo takes it and plays what
        // it asks: hung on the partner, flying as a pinball or steered as a charger.
        if (!down && PartyCombo::aside(players[i])) {
            players[i].knockback.clear();
            const PartyCombo::Events comboEvents{events.comboImpact, events.advanceTurbo};
            if (ComboMove::flies(players[i].combo)) {
                PartyCombo::fly(players, i, ticks, seconds, collision, comboEvents);
            } else if (ComboMove::ridden(players[i].combo)) {
                const auto dwarf = static_cast<usize>(std::max(players[i].combo.partner, 0));
                const auto steering = dwarf < players.size()
                                          ? static_cast<usize>(players[dwarf].actor.player())
                                          : inputs.size();
                PartyCombo::ride(players, i,
                                 !held && steering < inputs.size() ? inputs[steering].move
                                                                   : MoveInput{},
                                 cameraYaw, ticks, seconds, collision, comboEvents);
            }
            PartyCombo::animate(players, i, ticks, seconds, comboEvents);
            subjects.push_back({actor.position(), actor.followPoint()});
            continue;
        }
        // Webs own the reaction animation and buttons, but allow a slow escape walk.
        const PlayerAnimator* animator =
            players[i].figure != nullptr ? &players[i].figure->animator() : nullptr;
        const bool webbed = players[i].reaction == PlayerDeed::Webbed ||
                            (animator != nullptr && animator->webbed());
        const bool reeling = players[i].reaction != PlayerDeed::None ||
                             (animator != nullptr && animator->reacting());
        const bool immobilized =
            (players[i].reaction != PlayerDeed::None &&
             players[i].reaction != PlayerDeed::Webbed) ||
            (animator != nullptr && animator->reacting() && !animator->webbed());
        const bool entering =
            players[i].figure != nullptr && players[i].figure->animator().entering();
        // A halo wearer holding Death stands facing him, heeding no button (PlayerMotion,
        // pmotion.c 1621).
        std::optional<Vec3> deathHeld;
        if (events.grabDeath) {
            deathHeld = events.grabDeath(i, ticks, !held && !down && !reeling && !entering);
        }
        MoveInput move =
            !held && !down && !immobilized && !entering && !deathHeld && player < inputs.size()
                ? inputs[player].move
                : MoveInput{};
        const auto aim =
            !held && !down && !reeling && !entering && !deathHeld && player < inputs.size()
                ? inputs[player].aimPoint
                : std::nullopt;
        if (aim) {
            players[i].cursorAiming = true;
            move = cursorRelativeMove(move, actor.position(), *aim, cameraYaw);
            if (animator == nullptr || (animator->turnScale() >= 1.0f && !animator->shoving())) {
                actor.faceToward(*aim);
            }
        }
        // What the buttons ask: a potion first, when one is carried, then the attack.
        PlayerDeed deed = down ? PlayerDeed::Die : PlayerDeed::None;
        if (!down && players[i].reaction != PlayerDeed::None) {
            deed = players[i].reaction;
        }
        // Still, a body retches while gas or Death's touch lasts, heeding no button
        // (PlayerMotion's reaction 100, pmotion.c 1485).
        players[i].gagSeconds = std::max(players[i].gagSeconds - seconds, 0.0f);
        const bool gagging =
            !down && deed == PlayerDeed::None && players[i].gagSeconds > 0.0f && !move.any();
        if (gagging) {
            deed = PlayerDeed::Gag;
        }
        if (deathHeld) {
            deed = PlayerDeed::DeathGrab;
        }
        players[i].reaction = PlayerDeed::None;
        if (!held && !down && !reeling && !entering && !gagging && !deathHeld &&
            player < inputs.size()) {
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
            } else if (in.strongAttack && players[i].figure != nullptr) {
                deed = events.attackDeed ? events.attackDeed(i, true, move.any())
                                         : PlayerDeed::StrongAttack;
            } else if (in.turbo) {
                deed = PlayerDeed::Defend; // held by itself, the turbo button is the guard
            } else if (in.attack) {
                deed = events.attackDeed ? events.attackDeed(i, false, move.any())
                                         : PlayerDeed::Attack;
            }
            // The combo button with half the meter takes hold of a partner ahead, over
            // anything else the buttons ask (fn_80088938, pmotion.c 4656).
            if (in.combo && players[i].turbo.held() >= ComboMove::kMeterNeeded &&
                animator != nullptr && animator->canBegin(PlayerDeed::Combo)) {
                if (const auto partner = PartyCombo::partnerFor(players, i)) {
                    PartyCombo::begin(players, i, *partner);
                    deed = PlayerDeed::Combo;
                    events.perform(i, Action::ComboStart);
                }
            }
            events.select(i, in.selector, ticks);
        }
        // A pickup's gesture, or a gag at food gone bad, is made when nothing else is asked
        // (speak_kind, pmotion.c 1722).
        if (down) {
            players[i].gesture = PlayerDeed::None;
        } else if (players[i].gesture != PlayerDeed::None && deed == PlayerDeed::None && !reeling &&
                   !held) {
            deed = players[i].gesture;
            players[i].gesture = PlayerDeed::None;
        }
        const auto powerups = PowerupEffects::of(actor.save().progress().inventory);
        actor.setPaceBonus(powerups.paceAdd);
        // A body in a throw keeps its feet where they are, turning to the stick.
        const bool closeAttack = deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow ||
                                 deed == PlayerDeed::MeleeSlow || deed == PlayerDeed::MeleeSlowLow;
        f32 actionPace = animator != nullptr ? animator->moveScale() : 1.0f;
        const bool itemAttack = deed == PlayerDeed::SuperShot || deed == PlayerDeed::Hammer ||
                                deed == PlayerDeed::Breathe || deed == PlayerDeed::FireLeft ||
                                deed == PlayerDeed::FireRight;
        // A close attack under way paces the body by its own action.
        const bool swingStarts = closeAttack && (animator == nullptr || !animator->meleeing());
        if (swingStarts && deed == PlayerDeed::Melee) {
            // The first input frame moves before the animation changes. Do not
            // bypass an existing reaction/arrival lock to start a quick swing.
            actionPace = std::min(actionPace, PlayerAnimator::kQuickMeleePace);
        } else if (swingStarts || itemAttack || deed == PlayerDeed::StrongAttack) {
            actionPace = 0;
        }
        const f32 pace = webbed ? PlayerAnimator::kWebPace : actionPace;
        const bool charging =
            players[i].figure != nullptr && players[i].figure->animator().shoving();
        // Strafing, the character steps the way the stick is pushed without turning to it.
        const bool strafes = !held && !down && !charging && player < inputs.size() &&
                             (inputs[player].strafe || aim.has_value()) && move.any();
        if (players[i].figure != nullptr) {
            // Mouse aim locks facing, but forward movement uses the ordinary full-stick
            // gait. Backing up, sidestepping, and moving shots retain their strafe actions.
            const bool cursorForward = aim && !inputs[player].strafe && !inputs[player].attack &&
                                       inputs[player].move.direction.x == 0.0f &&
                                       inputs[player].move.direction.y > 0.0f;
            players[i].figure->setStrafe(
                strafes && !cursorForward
                    ? strafeWayOf(PlayerActor::headingOf(move, cameraYaw), actor.yaw())
                    : StrafeWay::None);
        }
        players[i].glow.step(seconds);
        // Riding a moving floor, the body goes where the floor took it (PlayerCheckFloor).
        if (!down) {
            FloorRiding::carry(players[i], collision);
        } else {
            players[i].floor = {};
        }
        const Vec3 before = actor.position();
        // A knock slides the body on, then what hit it last frame kicks it, turning it to
        // face along the push or against it (PlayerMotion, PlayerKnockback).
        if (!down) {
            actor.slide(players[i].knockback.step(seconds, actor.speed()), &collision);
            if (const auto heading = players[i].knockback.kick(
                    actor.yaw(), (powerups.special & powerup::kPojo) != 0)) {
                actor.turnTo(*heading);
            }
        }
        MoveInput attackMove = move;
        // AnimAction turns a close attack to the stick only part of the way, or not at all
        // (the quick swings); a step carries on along the facing with the stick let go.
        const bool quickStarts = deed == PlayerDeed::Melee && swingStarts;
        f32 turn = animator != nullptr ? animator->turnScale() : 1.0f;
        if (quickStarts) {
            turn = 0.0f;
        }
        if (move.any() && turn < 1.0f) {
            const f32 wanted = PlayerActor::headingOf(move, cameraYaw);
            const f32 share = 1.0f - std::pow(1.0f - turn, seconds * kTurnFrames);
            const f32 heading =
                actor.yaw() +
                std::remainder(wanted - actor.yaw(), 2.0f * std::numbers::pi_v<f32>) * share;
            attackMove.direction =
                Vec2{std::sin(heading - cameraYaw), std::cos(heading - cameraYaw)};
        } else if (!move.any() && animator != nullptr && animator->lunging() && !held && !down &&
                   !immobilized) {
            const f32 heading = actor.yaw() - cameraYaw;
            attackMove = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1.0f};
        }
        actor.update(charging ? chargeInput(actor, move, cameraYaw) : attackMove, cameraYaw,
                     seconds, &collision, pace, strafes);
        if (!down && events.resolveMovement) {
            // A body/fixture push is still movement, not a teleport. Sweep the
            // correction through the same walls and floor edges as the step.
            actor.slide(events.resolveMovement(i, before, actor.position()) - actor.position(),
                        &collision);
        }
        if (!down) {
            contacts[i] = PartyCollision::step(players, i, before, seconds, &collision);
            FloorRiding::land(players, i, before, collision);
        }
        if (!down && events.limitMovement) {
            const Vec3 limited = events.limitMovement(i, before, actor.position());
            if (glm::distance(limited, actor.position()) > 1.0e-5f) {
                actor.place(before);
                actor.slide(limited - before, &collision);
                if (events.resolveMovement) {
                    actor.slide(events.resolveMovement(i, before, actor.position()) -
                                    actor.position(),
                                &collision);
                }
                Vec3 position = actor.position();
                PartyCollision::resolve(players, i, before, position);
                actor.slide(position - actor.position(), &collision);
                FloorRiding::land(players, i, before, collision);
            }
        }
        // A floor gone from under it lets it sink; lost under the world, it stands again
        // beside another, or at the start.
        if (players[i].life != PlayerLife::InTower) {
            actor.fall(seconds, collision);
        }
        if (!down && collision.loaded() && actor.position().y <= collision.lowest() - kLostDepth) {
            std::optional<Vec3> spot = rescueSpot(players, i, collision);
            if (!spot.has_value() && events.startPoint) {
                spot = events.startPoint();
            }
            if (spot.has_value()) {
                actor.place(*spot);
                players[i].knockback.clear();
                players[i].floor = {};
            }
        }
        // Stationary normal attacks face the assisted target. The stick, strafe,
        // charging and authored turbo movement retain control of their heading.
        if (!aim && !move.any() && !charging && player < inputs.size() && !inputs[player].strafe &&
            (deed == PlayerDeed::Attack || deed == PlayerDeed::StrongAttack ||
             deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow ||
             deed == PlayerDeed::MeleeSlow || deed == PlayerDeed::MeleeSlowLow ||
             deed == PlayerDeed::SuperShot || deed == PlayerDeed::Hammer ||
             deed == PlayerDeed::Breathe || deed == PlayerDeed::FireLeft ||
             deed == PlayerDeed::FireRight) &&
            events.aim) {
            if (const auto target = events.aim(i)) {
                actor.faceToward(*target);
            }
        }
        if (deathHeld) {
            actor.faceToward(*deathHeld);
        }
        if (charging) {
            events.perform(i, Action::Ram);
        } else {
            players[i].rammed.clear();
        }
        if (players[i].figure != nullptr) {
            players[i].figure->setAttackSpeed((powerups.weapon & powerup::kRapidFire) != 0,
                                              (powerups.special & powerup::kSpeedBoost) != 0);
            players[i].figure->setShielded((powerups.armor & powerup::kShields) != 0);
            // Shoved and moving, it shows being pushed, facing the way it goes.
            const Vec3 moved = actor.position() - before;
            const bool pushed = !down && players[i].knockback.pushed() &&
                                std::hypot(moved.x, moved.z) > kPushedStep * seconds;
            if (pushed) {
                actor.turnTo(std::atan2(moved.x, moved.z));
            }
            players[i].figure->setPushed(pushed);
            // The close attack sees where the nearest thing to strike lies as it decides.
            const bool attackHeld = !held && !down && player < inputs.size() &&
                                    (inputs[player].attack || inputs[player].strongAttack);
            if (events.meleeSense && (attackHeld || players[i].figure->animator().meleeing())) {
                players[i].figure->setMelee(events.meleeSense(i, attackHeld));
            }
            players[i].figure->setCombo(players[i].combo.grabberClass, players[i].combo.rideAsked);
            players[i].figure->animate(move.magnitude, ticks, seconds, deed);
            players[i].figure->updateTrail(
                PlayerFigure::bodyPlacement(players[i].capture.body().value_or(actor.transform()),
                                            actor.save(), powerups),
                ticks);
            events.advanceTurbo(i, ticks, seconds);
            // A grabber's move goes on: its partner taken up, let fly or set down.
            if (players[i].combo.role == ComboRole::Grabber) {
                PartyCombo::advance(players, i, ticks);
            }
            if (players[i].figure->familiarReleased()) {
                events.perform(i, Action::FamiliarShot);
            }
            if (players[i].figure->animator().meleeStruck()) {
                events.perform(i, Action::Melee);
            }
            if (players[i].life == PlayerLife::Dying && players[i].figure->animator().dead()) {
                players[i].life = PlayerLife::InTower; // the body goes; its box says where
                events.perform(i, Action::Fallen);
            }
            if (players[i].figure->animator().released()) {
                events.perform(i, Action::ThrowWeapon);
            }
            if (players[i].figure->animator().strongReleased()) {
                events.perform(i, Action::StrongThrow);
            }
            if (players[i].figure->animator().superReleased()) {
                events.perform(i, Action::SuperShot);
            }
            if (players[i].figure->animator().itemReleased() != PlayerDeed::None) {
                events.perform(i, Action::ItemAttack);
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
            // A levitating body's feet make no sound (pmotion.c 2931).
            if (const PlayerAnimator::Foot foot = players[i].figure->animator().footfall();
                foot != PlayerAnimator::Foot::None && !powerups.levitating()) {
                events.perform(i, foot == PlayerAnimator::Foot::Second ? Action::SecondFoot
                                                                       : Action::FirstFoot);
            }
        }
        if (players[i].figure == nullptr && players[i].life == PlayerLife::Dying) {
            players[i].life = PlayerLife::InTower; // nothing to play: gone at once
            if (events.perform) {
                events.perform(i, Action::Fallen);
            }
        }
        subjects.push_back(
            CameraSubject{actor.position(), actor.followPoint(), actor.height() * 0.5f});
    }
    // Every rider hangs on its carrier's node as posed this frame; the camera sees it there.
    PartyCombo::carry(players);
    for (usize i = 0; i < players.size() && i < subjects.size(); ++i) {
        if (players[i].combo.riding) {
            const auto& actor = players[i].actor;
            subjects[i] =
                CameraSubject{actor.position(), actor.followPoint(), actor.height() * 0.5f};
        }
    }
    for (PlayerRuntime& runtime : players) {
        runtime.knockback.endFrame();
    }
    passIt(players, ticks, events, contacts);
    return subjects;
}
} // namespace gdl::game
