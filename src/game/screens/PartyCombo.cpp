#include "game/screens/PartyCombo.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PartyCollision.h"

namespace gdl::game {

namespace {

constexpr std::string_view kSeatNode = "DUMMY"; ///< the costume's marker the partner hangs on

/** Whether a member may be taken hold of, or take hold: standing, free and carrying no one
 * (fn_80088EF4's tests of the other player). */
bool free(const PlayerRuntime& runtime) {
    if (runtime.life != PlayerLife::Standing || runtime.departed || runtime.combo.active() ||
        runtime.capture.active() || runtime.transport.active() || runtime.figure == nullptr ||
        runtime.floor.object < 0 || (runtime.floor.flags & FloorRiding::kMoving) != 0 ||
        !runtime.figure->animator().bound() || !runtime.figure->animator().comboTakeable()) {
        return false;
    }
    const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
    return (worn.special & powerup::kPojo) == 0;
}

/** Heading `wanted` taken part of the way from `yaw` at `turn` a 30 Hz frame. */
f32 turnToward(f32 yaw, f32 wanted, f32 turn, f32 seconds) {
    const f32 share = 1.0f - std::pow(1.0f - turn, seconds * PartyCombo::kTurnFrames);
    return yaw + std::remainder(wanted - yaw, 2.0f * std::numbers::pi_v<f32>) * share;
}

} // namespace

std::optional<usize> PartyCombo::partnerFor(std::span<const PlayerRuntime> players, usize index) {
    if (index >= players.size() || !free(players[index])) {
        return std::nullopt;
    }
    std::vector<ComboMove::Candidate> candidates;
    candidates.reserve(players.size());
    for (const PlayerRuntime& runtime : players) {
        candidates.push_back({runtime.actor.position(), free(runtime)});
    }
    const PlayerActor& actor = players[index].actor;
    return ComboMove::findPartner(index, actor.position(), actor.facing(), candidates);
}

void PartyCombo::begin(std::span<PlayerRuntime> players, usize grabber, usize partner) {
    if (grabber >= players.size() || partner >= players.size() || grabber == partner) {
        return;
    }
    const s32 character = players[grabber].actor.save().character;
    const s32 grabberClass =
        character == kSumnerClass ? ComboMove::kWizard : character % kStartingClassCount;
    ComboMove::link(players[grabber].combo, grabber, players[partner].combo, partner, grabberClass);
    if (players[partner].figure != nullptr) {
        players[partner].figure->setCombo(grabberClass, false);
    }
}

void PartyCombo::cancelInvalid(std::span<PlayerRuntime> players) {
    const auto release = [](PlayerRuntime& player) {
        if (player.combo.riding) {
            player.actor.place(player.combo.saved);
        }
        ComboMove::clear(player.combo);
        if (player.figure != nullptr) {
            player.figure->setCombo(-1, false);
        }
    };
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        if (!player.combo.active()) {
            continue;
        }
        const s32 index = player.combo.partner;
        PlayerRuntime* partner = index >= 0 && static_cast<usize>(index) < players.size() &&
                                         static_cast<usize>(index) != i
                                     ? &players[static_cast<usize>(index)]
                                     : nullptr;
        const bool paired = partner != nullptr && partner->combo.active() &&
                            partner->combo.partner == static_cast<s32>(i);
        const bool roles = paired && ((player.combo.role == ComboRole::Grabber) !=
                                      (partner->combo.role == ComboRole::Grabber));
        if (roles && player.life == PlayerLife::Standing && !player.departed &&
            partner->life == PlayerLife::Standing && !partner->departed) {
            continue;
        }
        release(player);
        // A reused slot may now belong to another pair; never tear down that pair.
        if (paired) {
            release(*partner);
        }
    }
}

bool PartyCombo::aside(const PlayerRuntime& runtime) {
    return runtime.combo.riding || runtime.combo.role == ComboRole::Held ||
           runtime.combo.role == ComboRole::Thrown;
}

PlayerDeed PartyCombo::deedOf(const PlayerRuntime& runtime) {
    switch (runtime.combo.role) {
    case ComboRole::Held: return PlayerDeed::ComboHeld;
    case ComboRole::Thrown: return PlayerDeed::ComboThrown;
    default: return PlayerDeed::None;
    }
}

void PartyCombo::animate(std::span<PlayerRuntime> players, usize index, s32 ticks, f32 seconds,
                         const Events& events) {
    if (index >= players.size()) {
        return;
    }
    PlayerRuntime& runtime = players[index];
    if (runtime.combo.role == ComboRole::Held) {
        ComboMove::tick(runtime.combo, ticks, seconds);
    }
    if (runtime.figure != nullptr) {
        runtime.figure->setCombo(runtime.combo.grabberClass, runtime.combo.rideAsked);
        runtime.figure->animate(0, ticks, seconds, deedOf(runtime));
        runtime.figure->updateTrail(
            PlayerFigure::bodyPlacement(
                runtime.actor.transform(), runtime.actor.save(),
                PowerupEffects::of(runtime.actor.save().progress().inventory)),
            ticks);
        if (events.advanceTurbo) {
            events.advanceTurbo(index, ticks, seconds);
        }
    }
    if (runtime.combo.role == ComboRole::Grabber) {
        advance(players, index, ticks);
    }
}

void PartyCombo::advance(std::span<PlayerRuntime> players, usize grabber, s32 ticks) {
    cancelInvalid(players);
    if (grabber >= players.size() || players[grabber].combo.role != ComboRole::Grabber) {
        return;
    }
    PlayerRuntime& g = players[grabber];
    const s32 partnerIndex = g.combo.partner;
    PlayerRuntime& p = players[static_cast<usize>(partnerIndex)];
    ComboPhase phase;
    if (g.figure != nullptr) {
        const PlayerAnimator& animator = g.figure->animator();
        phase.act1 = animator.action() == PlayerAnimator::Action::ComboAct1;
        phase.act2 = animator.action() == PlayerAnimator::Action::ComboAct2;
        phase.frame = animator.player().frame();
    }
    const Vec3 grabberSaved = g.combo.saved;
    const Vec3 partnerSaved = p.combo.saved;
    const bool wasRiding = p.combo.riding;
    const bool wasCarrying = g.combo.riding;
    const ComboOrders orders = ComboMove::advance(g.combo, p.combo, phase, ticks);
    switch (orders.attach) {
    case ComboOrders::Attach::PartnerOnGrabber:
        p.combo.saved = p.actor.position();
        p.knockback.clear();
        break;
    case ComboOrders::Attach::GrabberOnPartner:
        g.combo.saved = g.actor.position();
        g.knockback.clear();
        break;
    case ComboOrders::Attach::None: break;
    }
    if (orders.turnPartnerAway) {
        // The partner turns its back to the grabber (pmotion.c 3005).
        const Vec3 gap = g.actor.position() - p.actor.position();
        p.actor.turnTo(std::atan2(gap.x, gap.z) + std::numbers::pi_v<f32>);
    }
    if (orders.restorePartner && wasRiding) {
        p.actor.place(partnerSaved);
    }
    if (orders.restoreGrabber && wasCarrying) {
        g.actor.place(grabberSaved);
    }
    if (orders.releasePartner) {
        p.rammed.clear(); // what the flight strikes, once each
    }
    if (orders.unlink) {
        if (g.figure != nullptr) {
            g.figure->setCombo(-1, false);
        }
        if (p.figure != nullptr) {
            p.figure->setCombo(-1, false);
        }
    }
}

void PartyCombo::fly(std::span<PlayerRuntime> players, usize flier, s32 ticks, f32 seconds,
                     const WorldCollision& collision, const Events& events) {
    if (flier >= players.size()) {
        return;
    }
    PlayerRuntime& runtime = players[flier];
    ComboState& state = runtime.combo;
    ComboMove::tick(state, ticks, seconds);
    PlayerActor& actor = runtime.actor;
    const Vec3 before = actor.position();
    const Vec3 meant = before + actor.facing() * (ComboMove::kFlightSpeed * seconds);
    Vec3 to = collision.loaded() ? collision.resolveWalls(meant, actor.radius(),
                                                          meant.y + PlayerActor::kFootClearance,
                                                          meant.y + actor.height())
                                 : meant;
    const Vec3 pushed{to.x - meant.x, 0.0f, to.z - meant.z};
    const bool wall = std::hypot(pushed.x, pushed.z) > PartyCollision::kCoincident;
    const Vec3 worldResolved = to;
    std::optional<usize> other = PartyCollision::resolve(players, flier, before, to);
    const auto thrower = static_cast<usize>(std::max(state.partner, 0));
    if (other == thrower && state.graceSeconds > 0.0f) {
        to = worldResolved;
        other.reset();
    }
    if (other.has_value()) {
        players[*other].knockback.shove(meant - before, seconds);
    }
    actor.place(to);
    // What it strikes turns it: an item or another member three eighths round, a wall by
    // reflection; not its thrower until the grace is over (pmotion.c 1157).
    std::optional<f32> turned;
    if (events.impact && events.impact(flier, thrower, ComboMove::kWarriorBlow)) {
        turned = ComboMove::bounceYaw(actor.yaw());
    }
    if (other.has_value() && (*other != thrower || state.graceSeconds <= 0.0f)) {
        turned = ComboMove::bounceYaw(actor.yaw());
    }
    if (wall) {
        turned = ComboMove::reflectedYaw(actor.yaw(), pushed);
    }
    if (turned.has_value() && ComboMove::takeTurn(state)) {
        actor.turnTo(*turned);
    }
    if (collision.loaded()) {
        actor.fall(seconds, collision);
    }
}

void PartyCombo::ride(std::span<PlayerRuntime> players, usize charger, const MoveInput& stick,
                      f32 cameraYaw, s32 ticks, f32 seconds, const WorldCollision& collision,
                      const Events& events) {
    if (charger >= players.size()) {
        return;
    }
    PlayerRuntime& runtime = players[charger];
    ComboState& state = runtime.combo;
    ComboMove::tick(state, ticks, seconds);
    PlayerActor& actor = runtime.actor;
    const Vec3 before = actor.position();
    // The dwarf's stick steers it, half way round a frame (COMBODWF2's turn scale).
    MoveInput steer = stick;
    if (stick.any()) {
        const f32 heading = turnToward(actor.yaw(), PlayerActor::headingOf(stick, cameraYaw),
                                       ComboMove::kRideTurn, seconds);
        steer.direction = Vec2{std::sin(heading - cameraYaw), std::cos(heading - cameraYaw)};
    }
    actor.update(steer, cameraYaw, seconds, collision.loaded() ? &collision : nullptr,
                 ComboMove::kRidePace);
    PartyCollision::step(players, charger, before, seconds);
    const auto dwarf = static_cast<usize>(std::max(state.partner, 0));
    if (events.impact && events.impact(charger, dwarf, ComboMove::kDwarfBlow) &&
        ComboMove::takeTurn(state)) {
        actor.turnTo(ComboMove::bounceYaw(actor.yaw()));
    }
    if (collision.loaded()) {
        actor.fall(seconds, collision);
    }
}

Mat4 PartyCombo::seatOf(const PlayerRuntime& carrier) {
    const Mat4 body =
        PlayerFigure::bodyPlacement(carrier.actor.transform(), carrier.actor.save(),
                                    PowerupEffects::of(carrier.actor.save().progress().inventory));
    if (carrier.figure != nullptr) {
        if (const auto seat = carrier.figure->attachment(body, kSeatNode)) {
            return *seat;
        }
    }
    return body;
}

void PartyCombo::carry(std::span<PlayerRuntime> players) {
    for (PlayerRuntime& rider : players) {
        if (!rider.combo.riding || rider.combo.partner < 0 ||
            static_cast<usize>(rider.combo.partner) >= players.size()) {
            continue;
        }
        const Mat4 seat = seatOf(players[static_cast<usize>(rider.combo.partner)]);
        rider.actor.place(Vec3{seat[3]});
        rider.actor.turnTo(std::atan2(seat[2].x, seat[2].z));
    }
}

} // namespace gdl::game
