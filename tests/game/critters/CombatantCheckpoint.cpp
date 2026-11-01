#include "game/critters/CombatantCheckpoint.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <tuple>
#include <utility>

#include "game/level/FixtureCheckpointDetail.h"

namespace gdl::game {
namespace {
using fixture_checkpoint::matrix;
using fixture_checkpoint::name;
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;

bool player(s32 value) {
    return value >= -1 && value < 4;
}
bool nonnegative(f32 value) {
    return scalar(value) && value >= 0;
}
bool vector2(const Vec2& value) {
    return scalar(value.x) && scalar(value.y);
}
bool optionalVector(const std::optional<Vec3>& value) {
    return !value || vector(*value);
}
bool pose(const std::optional<TreePose::Checkpoint>& value) {
    return !value || value->valid();
}
bool cooldown(f32 value) {
    return scalar(value) || value == -std::numeric_limits<f32>::infinity();
}
bool scales(const EnemyScales& value) {
    return nonnegative(value.health) && nonnegative(value.speed) && nonnegative(value.sight) &&
           nonnegative(value.damage) && nonnegative(value.missileRate) &&
           nonnegative(value.missileAim) && nonnegative(value.playerLevel) && value.players >= 0 &&
           value.players <= 4;
}
bool kind(CombatantKind value) {
    return value == CombatantKind::Unknown || value == CombatantKind::Golem ||
           value == CombatantKind::Boss || value == CombatantKind::Gargoyle ||
           value == CombatantKind::General;
}
bool shotValid(const CombatantCheckpoint::Shot& v) {
    return v.definition.resource != 0 && v.definition.index < CombatantCheckpoint::kMaxParts &&
           v.critter >= -1 && v.damageIndex >= 0 &&
           v.damageIndex < static_cast<s32>(CombatantCheckpoint::kMaxRows) && vector(v.origin) &&
           vector(v.forward) && optionalVector(v.target) && nonnegative(v.rate) &&
           nonnegative(v.scale) && nonnegative(v.damageScale) && scalar(v.birthLife) &&
           v.realm >= 'A' && v.realm <= 'Z';
}
template <class T, class F>
bool all(const std::vector<T>& values, F check, usize limit = CombatantCheckpoint::kMaxEvents) {
    return values.size() <= limit && std::ranges::all_of(values, check);
}
template <class T, class F> bool equal(const std::vector<T>& a, const std::vector<T>& b, F fields) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [&](const auto& x, const auto& y) { return fields(x) == fields(y); });
}
auto values(const CombatCue& v) {
    return std::tie(v.critter, v.tree, v.sound, v.attenuated, v.soundPosition, v.position, v.yaw,
                    v.scale, v.life, v.follows, v.shakes, v.arena, v.untilNextMove,
                    v.stopMoveEffect, v.rootAttachment, v.node, v.nodeOffset, v.pitchYaw, v.loop,
                    v.placement, v.playerAttachment);
}
auto values(const CombatGrab& v) {
    return std::tie(v.player, v.critter, v.attachment, v.velocity, v.damage, v.restoreSupport);
}
auto values(const CombatSpew& v) {
    return std::tie(v.critter, v.origin, v.velocity, v.halfAngle);
}
auto values(const CombatArenaActivation& v) {
    return std::tie(v.index, v.delay);
}
auto values(const CombatTrample& v) {
    return std::tie(v.enemy, v.damage, v.position);
}
auto values(const CombatantRam& v) {
    return std::tie(v.id, v.damage);
}
auto values(const CombatPush& v) {
    return std::tie(v.player, v.velocity);
}
auto values(const CombatBlow& v) {
    return std::tie(v.player, v.critter, v.damage, v.direction, v.breath, v.gated, v.flags,
                    v.origin, v.area, v.repeatGap);
}
auto values(const CombatLoss& v) {
    return std::tie(v.critter, v.kind, v.form, v.player, v.experience, v.killed, v.drop,
                    v.position);
}
auto values(const CritterArea& v) {
    return std::tie(v.local, v.node, v.worldParent, v.velocity, v.displacement, v.radius, v.minDot,
                    v.damage, v.flags, v.secondsLeft, v.lifetime, v.expanding, v.linearGrowth,
                    v.halfHeight);
}
auto values(const CombatArenaTarget& v) {
    return std::tie(v.index, v.placement, v.active);
}

/** Common value fields deliberately shared by capture and staged restoration.
 * Pointer-bearing/asset-bound clocks and differently typed rosters are handled
 * explicitly below; none of these assignments calls native gameplay methods. */
template <class To, class From> void common(To& to, const From& from) {
    to.hidden = from.hidden;
    to.forcedPattern = from.forcedPattern;
    to.childrenIntact = from.childrenIntact;
    to.smoothValid = from.smoothValid;
    to.health = from.health;
    to.burnGap = from.burnGap;
    to.maxHealth = from.maxHealth;
    to.position = from.position;
    to.yaw = from.yaw;
    to.initialYaw = from.initialYaw;
    to.homePosition = from.homePosition;
    to.initialRoot = from.initialRoot;
    to.push = from.push;
    to.target = from.target;
    to.targetDistance = from.targetDistance;
    to.targetYaw = from.targetYaw;
    to.move = from.move;
    to.moveTarget = from.moveTarget;
    to.pattern = from.pattern;
    to.finishedSeconds = from.finishedSeconds;
    to.age = from.age;
    to.moveTimes = from.moveTimes;
    to.patternTimes = from.patternTimes;
    to.struckThisMove = from.struckThisMove;
    to.areas = from.areas;
    to.arenaAnchors = from.arenaAnchors;
    to.arenaTargets = from.arenaTargets;
    to.lastArenaTarget = from.lastArenaTarget;
    to.hurtPending = from.hurtPending;
    to.hurtFlags = from.hurtFlags;
    to.hurtDirection = from.hurtDirection;
    to.roarOwed = from.roarOwed;
    to.sinceHurt = from.sinceHurt;
    to.flashTicks = from.flashTicks;
    to.skinAge = from.skinAge;
    to.alpha = from.alpha;
    to.tint = from.tint;
    to.scale = from.scale;
    to.shrink = from.shrink;
    to.frozenTicks = from.frozenTicks;
    to.blindTicks = from.blindTicks;
    to.curbSeconds = from.curbSeconds;
    to.held = from.held;
    to.roarWanted = from.roarWanted;
    to.moveEffect = from.moveEffect;
    to.moveDone = from.moveDone;
    to.soundsGiven = from.soundsGiven;
    to.arenaCollected = from.arenaCollected;
    to.shotFrame = from.shotFrame;
    to.attackTarget = from.attackTarget;
    to.stepTarget = from.stepTarget;
    to.patrolAim = from.patrolAim;
    to.grabbed = from.grabbed;
    to.grabMove = from.grabMove;
    to.grabNode = from.grabNode;
    to.grabOffset = from.grabOffset;
}
auto actorValues(const CombatantCheckpoint::Actor& v) {
    return std::tie(
        v.state, v.hidden, v.forcedPattern, v.childrenIntact, v.smoothValid, v.definition, v.parent,
        v.branch, v.health, v.burnGap, v.maxHealth, v.position, v.floor, v.yaw, v.initialYaw,
        v.homePosition, v.initialRoot, v.push, v.target, v.targetDistance, v.targetYaw, v.targets,
        v.playerDamage, v.move, v.moveTarget, v.pattern, v.finishedSeconds, v.age, v.moveTimes,
        v.patternTimes, v.patternStep, v.struckThisMove, v.arenaAnchors, v.patrolBound, v.lookout,
        v.sight, v.lastArenaTarget, v.hurtPending, v.hurtFlags, v.hurtDirection, v.roarOwed,
        v.sinceHurt, v.flashTicks, v.skin, v.skinAge, v.hitNodes, v.replacements, v.alpha, v.tint,
        v.scale, v.shrink, v.frozenTicks, v.blindTicks, v.curbSeconds, v.held, v.roarWanted,
        v.moveEffect, v.moveDone, v.soundsGiven, v.arenaCollected, v.shotFrame, v.attackTarget,
        v.stepTarget, v.patrolAim, v.grabbed, v.grabMove, v.grabNode, v.grabOffset, v.player,
        v.pose, v.smooth, v.presentation, v.attachments, v.gaze);
}
bool restorePlayer(AnimationPlayer& output, const AnimationPlayer::Checkpoint& state,
                   const TreeInfo* tree) {
    if (!state.playing) {
        return output.restore(state, nullptr);
    }
    return (tree != nullptr) && state.sequence < tree->sequences.size() &&
           output.restore(state, &tree->sequences[state.sequence]);
}
bool restorePose(TreePose& output, const std::optional<TreePose::Checkpoint>& state,
                 const TreeInfo* tree) {
    return !state || ((tree != nullptr) && output.restore(*tree, *state));
}
std::optional<u32> randomWord(const std::minstd_rand& random) {
    // linear_congruential_engine's textual representation is exactly its one
    // integer state. Convert to a bounded integer value here, never wire text.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << random;
    const auto text = stream.str();
    u32 value = 0;
    // the end pointer of a contiguous buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    // the end pointer of a contiguous buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0 &&
                   value < 2147483647U
               ? std::optional{value}
               : std::nullopt;
}
} // namespace

bool CombatantCheckpoint::Events::valid() const {
    return all(cues,
               [](const auto& v) {
                   return v.critter >= -1 && name(v.tree) && name(v.sound) &&
                          vector(v.soundPosition) && vector(v.position) && scalar(v.yaw) &&
                          nonnegative(v.scale) && scalar(v.life) && (!v.node || name(*v.node)) &&
                          vector(v.nodeOffset) && vector2(v.pitchYaw) &&
                          (!v.placement || matrix(*v.placement)) &&
                          (!v.playerAttachment || player(*v.playerAttachment));
               }) &&
           all(grabs,
               [](const auto& v) {
                   return player(v.player) && v.critter >= -1 &&
                          (!v.attachment || matrix(*v.attachment)) && vector(v.velocity) &&
                          nonnegative(v.damage);
               }) &&
           all(spews,
               [](const auto& v) {
                   return v.critter >= -1 && vector(v.origin) && vector(v.velocity) &&
                          scalar(v.halfAngle);
               }) &&
           all(shots, shotValid) &&
           all(arena, [](const auto& v) { return v.index <= 65535 && nonnegative(v.delay); }) &&
           all(tramples,
               [](const auto& v) {
                   return v.enemy >= 0 && nonnegative(v.damage) && vector(v.position);
               }) &&
           all(rams, [](const auto& v) { return v.id >= -1 && nonnegative(v.damage); }) &&
           all(pushes, [](const auto& v) { return player(v.player) && vector(v.velocity); }) &&
           all(blows,
               [](const auto& v) {
                   return player(v.player) && v.critter >= -1 && nonnegative(v.damage) &&
                          vector(v.direction) && vector(v.origin) && nonnegative(v.repeatGap);
               }) &&
           all(losses, [](const auto& v) {
               return v.critter >= -1 && kind(v.kind) && name(v.form) && player(v.player) &&
                      nonnegative(v.experience) && vector(v.position);
           });
}
bool CombatantCheckpoint::Events::operator==(const Events& other) const {
    const auto fields = [](const auto& v) { return values(v); };
    return equal(cues, other.cues, fields) && equal(grabs, other.grabs, fields) &&
           equal(spews, other.spews, fields) && shots == other.shots &&
           equal(arena, other.arena, fields) && equal(tramples, other.tramples, fields) &&
           equal(rams, other.rams, fields) && equal(pushes, other.pushes, fields) &&
           equal(blows, other.blows, fields) && equal(losses, other.losses, fields);
}
bool CombatantCheckpoint::Actor::valid() const {
    const auto row = [](s32 v) { return v >= -1 && v < static_cast<s32>(kMaxRows); };
    if (state > 2 || parent < -1 || parent >= static_cast<s32>(kMaxParts) ||
        definition.index >= kMaxParts || (branch && *branch >= TreePose::Checkpoint::kMaxNodes) ||
        !scalar(health) || !nonnegative(burnGap) || !nonnegative(maxHealth) || !vector(position) ||
        (floor && (floor->object < 0 || !vector(floor->local))) || !scalar(yaw) ||
        !scalar(initialYaw) || !vector(homePosition) || !vector(initialRoot) || !vector(push) ||
        !game::player(target) || !scalar(targetDistance) || !scalar(targetYaw) || !row(move) ||
        !game::player(moveTarget) || !row(pattern) || !scalar(finishedSeconds) ||
        !nonnegative(age) || patternStep > kMaxRows || !all(moveTimes, cooldown, kMaxRows) ||
        !all(patternTimes, cooldown, kMaxRows) || !all(struckThisMove, game::player, 4) ||
        lookout < -1 || lookout >= static_cast<s32>(LookoutRoute::kMost) ||
        (!patrolBound && lookout != -1) || !nonnegative(sight) || lastArenaTarget < -1 ||
        lastArenaTarget >= static_cast<s32>(kMaxRows) || !nonnegative(hurtPending) ||
        !vector(hurtDirection) || !nonnegative(roarOwed) || !nonnegative(sinceHurt) ||
        flashTicks < 0 || !row(skin) || !nonnegative(skinAge) || !nonnegative(alpha) || alpha > 1 ||
        !nonnegative(scale) || !nonnegative(shrink) || frozenTicks < 0 || blindTicks < 0 ||
        !nonnegative(curbSeconds) || !game::player(grabbed) || !row(grabMove) || !name(grabNode) ||
        !vector(grabOffset) || !optionalVector(attackTarget) || !optionalVector(stepTarget) ||
        !optionalVector(patrolAim) || !player.valid() || !game::pose(pose) || !game::pose(smooth) ||
        (state != 0 && smoothValid && !smooth) || presentation.state > 2 ||
        !vector(presentation.position) || !scalar(presentation.yaw) ||
        !nonnegative(presentation.frame) || !game::pose(presentation.pose) ||
        (presentation.valid && !presentation.pose)) {
        return false;
    }
    return all(
               targets,
               [](const auto& v) {
                   return game::player(v.player) && scalar(v.distance) && scalar(v.score) &&
                          nonnegative(v.inverseAnger) && vector2(v.direction);
               },
               4) &&
           std::ranges::all_of(playerDamage,
                               [](const auto& v) {
                                   return nonnegative(v.dealt) && nonnegative(v.received) &&
                                          scalar(v.dealtTime) && scalar(v.receivedTime);
                               }) &&
           all(
               areas,
               [](const auto& v) {
                   return matrix(v.local) && name(v.node) &&
                          (!v.worldParent || matrix(*v.worldParent)) && vector(v.velocity) &&
                          vector(v.displacement) && nonnegative(v.radius) && scalar(v.minDot) &&
                          v.minDot >= -1 && v.minDot <= 1 && nonnegative(v.damage) &&
                          scalar(v.secondsLeft) && scalar(v.lifetime);
               },
               kMaxRows) &&
           all(arenaAnchors, matrix, kMaxRows) &&
           all(
               arenaTargets, [](const auto& v) { return v.index <= 65535 && matrix(v.placement); },
               kMaxRows) &&
           all(
               hitNodes,
               [](const auto& v) {
                   return scalar(v.health) && v.flashTicks >= 0 &&
                          (!v.heldPose ||
                           (vector(v.heldPose->rotation) && vector(v.heldPose->position) &&
                            vector(v.heldPose->scale)));
               },
               TreePose::Checkpoint::kMaxNodes) &&
           all(
               replacements,
               [](const auto& v) {
                   return v.node < TreePose::Checkpoint::kMaxNodes && name(v.sourceNode) &&
                          name(v.object);
               },
               TreePose::Checkpoint::kMaxNodes) &&
           all(
               attachments,
               [](const auto& v) {
                   return v.player.valid() && game::pose(v.pose) && matrix(v.world) &&
                          nonnegative(v.previousFrame);
               },
               kMaxParts) &&
           std::ranges::all_of(gaze, vector2);
}
bool CombatantCheckpoint::Actor::operator==(const Actor& other) const {
    const auto fields = [](const auto& v) { return values(v); };
    return actorValues(*this) == actorValues(other) && equal(areas, other.areas, fields) &&
           equal(arenaTargets, other.arenaTargets, fields);
}
bool CombatantCheckpoint::Part::valid() const {
    return actor.valid() && id >= -1 &&
           id <= std::numeric_limits<s32>::max() - static_cast<s32>(kMaxParts) &&
           game::scales(scales) && realm >= 'A' && realm <= 'Z' && arenaRandom > 0 &&
           arenaRandom < 2147483647U && events.valid() && pressure.activePlayers < 16 &&
           std::ranges::all_of(pressure.attemptedDamage, nonnegative) &&
           (!pressure.levelBonus || nonnegative(*pressure.levelBonus)) &&
           // Retirement resets Actor, not the enclosing native slot identity.
           (actor.definition.resource != 0 || actor.state == 0);
}
bool CombatantCheckpoint::Part::operator==(const Part& other) const {
    return std::tie(actor, incarnation, id, collision, hazards, scales, realm, fullHarm, pressure,
                    arenaRandom, events) == std::tie(other.actor, other.incarnation, other.id,
                                                     other.collision, other.hazards, other.scales,
                                                     other.realm, other.fullHarm, other.pressure,
                                                     other.arenaRandom, other.events);
}
bool CombatantCheckpoint::State::valid() const {
    if (parts.empty() || parts.size() > kMaxParts || !std::ranges::all_of(parts, &Part::valid)) {
        return false;
    }
    // Native NextWaypoint treats out-of-roster links as the end of the route;
    // preserve the authored signed link rather than narrowing it to live indices.
    if (lookouts && (lookouts->points.size() > LookoutRoute::kMost ||
                     lookouts->next.size() > LookoutRoute::kMost ||
                     !std::ranges::all_of(lookouts->points, vector))) {
        return false;
    }
    for (usize index = 0; index < parts.size(); ++index) {
        const auto& a = parts[index].actor;
        if (a.parent != (index == 0 ? -1 : 0) || (a.patrolBound && !lookouts) ||
            (a.lookout >= 0 &&
             (!lookouts || static_cast<usize>(a.lookout) >= lookouts->points.size()))) {
            return false;
        }
    }
    return true;
}

struct CombatantCheckpoint::Prepared::Data {
    const CombatantCheckpoint* wrapper = nullptr;
    const Combatant* owner = nullptr;
    std::shared_ptr<const u8> lifetime, binding, successor;
    u64 revision = 0;
    std::unique_ptr<Combatant> staged;
};
CombatantCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
CombatantCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
CombatantCheckpoint::Prepared&
CombatantCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
CombatantCheckpoint::Prepared::~Prepared() = default;
CombatantCheckpoint::CombatantCheckpoint(Combatant& owner, std::span<const Resource> resources,
                                         Bindings bindings)
    : m_owner(owner), m_resources(resources.begin(), resources.end()), m_bindings(bindings) {
    for (const auto& resource : m_resources) {
        m_resourceBindings.push_back(resource.stock ? resource.stock->m_checkpointBinding
                                                    : nullptr);
    }
    if (bindings.lookouts) {
        m_lookouts = *bindings.lookouts;
    }
}
CombatantCheckpoint::~CombatantCheckpoint() = default;
bool CombatantCheckpoint::bound() const {
    if (m_resources.size() > kMaxRows ||
        (m_bindings.lookouts && (!m_lookouts || *m_bindings.lookouts != *m_lookouts))) {
        return false;
    }
    for (usize i = 0; i < m_resources.size(); ++i) {
        const auto& item = m_resources[i];
        if (item.id == 0 || !item.stock ||
            item.stock->m_checkpointBinding != m_resourceBindings[i] || !item.stock->tree ||
            !item.stock->data.loaded() || item.stock->children.size() >= kMaxParts) {
            return false;
        }
        for (usize j = 0; j < i; ++j) {
            if (m_resources[j].id == item.id || m_resources[j].stock == item.stock) {
                return false;
            }
        }
    }
    return true;
}
const CombatantCheckpoint::Resource* CombatantCheckpoint::resource(u32 id) const {
    const auto found = std::ranges::find(m_resources, id, &Resource::id);
    return found == m_resources.end() ? nullptr : &*found;
}
std::optional<CombatantCheckpoint::Definition>
CombatantCheckpoint::definition(const CritterData* data) const {
    if (!data) {
        return Definition{};
    }
    for (const auto& item : m_resources) {
        if (data == &item.stock->data) {
            return Definition{item.id, 0};
        }
        for (usize i = 0; i < item.stock->children.size(); ++i) {
            if (data == &item.stock->children[i]) {
                return Definition{item.id, static_cast<u32>(i + 1)};
            }
        }
    }
    return std::nullopt;
}
const CritterData* CombatantCheckpoint::definition(Definition value) const {
    const auto* item = resource(value.resource);
    if (!item) {
        return nullptr;
    }
    if (value.index == 0) {
        return &item->stock->data;
    }
    return value.index <= item->stock->children.size() ? &item->stock->children[value.index - 1]
                                                       : nullptr;
}
std::optional<CombatantCheckpoint::Events> CombatantCheckpoint::captureEvents(
    std::span<const CombatCue> cues, std::span<const CombatGrab> grabs,
    std::span<const CombatSpew> spews, std::span<const CombatShot> shots,
    std::span<const CombatArenaActivation> arena, std::span<const CombatTrample> tramples,
    std::span<const CombatantRam> rams, std::span<const CombatPush> pushes,
    std::span<const CombatBlow> blows, std::span<const CombatLoss> losses) const {
    if (!bound() || cues.size() > kMaxEvents || grabs.size() > kMaxEvents ||
        spews.size() > kMaxEvents || shots.size() > kMaxEvents || arena.size() > kMaxEvents ||
        tramples.size() > kMaxEvents || rams.size() > kMaxEvents || pushes.size() > kMaxEvents ||
        blows.size() > kMaxEvents || losses.size() > kMaxEvents) {
        return std::nullopt;
    }
    Events out;
    out.cues.assign(cues.begin(), cues.end());
    out.grabs.assign(grabs.begin(), grabs.end());
    out.spews.assign(spews.begin(), spews.end());
    out.arena.assign(arena.begin(), arena.end());
    out.tramples.assign(tramples.begin(), tramples.end());
    out.rams.assign(rams.begin(), rams.end());
    out.pushes.assign(pushes.begin(), pushes.end());
    out.blows.assign(blows.begin(), blows.end());
    out.losses.assign(losses.begin(), losses.end());
    for (const auto& shot : shots) {
        const auto ref = definition(shot.data);
        if (!ref || ref->resource == 0) {
            return std::nullopt;
        }
        out.shots.push_back({*ref, shot.critter, shot.damageIndex, shot.origin, shot.forward,
                             shot.target, shot.rate, shot.scale, shot.damageScale, shot.birthLife,
                             shot.endVisual, shot.realm});
    }
    return out.valid() ? std::optional{std::move(out)} : std::nullopt;
}
std::optional<std::vector<CombatShot>>
CombatantCheckpoint::restoreShots(std::span<const Shot> shots) const {
    if (!bound() || shots.size() > kMaxEvents) {
        return std::nullopt;
    }
    std::vector<CombatShot> out;
    out.reserve(shots.size());
    for (const auto& shot : shots) {
        const auto* data = definition(shot.definition);
        if (!shotValid(shot) || !data || !data->damage(shot.damageIndex)) {
            return std::nullopt;
        }
        out.push_back({data, shot.critter, shot.damageIndex, shot.origin, shot.forward, shot.target,
                       shot.rate, shot.scale, shot.damageScale, shot.birthLife, shot.endVisual,
                       shot.realm});
    }
    return out;
}
std::optional<CombatantCheckpoint::Part> CombatantCheckpoint::capturePart(const Combatant& owner,
                                                                          s32 parent) const {
    // These are borrowed, possibly already expired, frame scratch views. Do not read them.
    if (!owner.m_obstacles.empty() || !owner.m_swarm.empty()) {
        return std::nullopt;
    }
    const auto& native = owner.m_sim.actor;
    const auto ref = definition(native.definition);
    const auto random = randomWord(owner.m_sim.arenaRandom);
    auto events =
        captureEvents(owner.m_sim.cues, owner.m_sim.grabs, owner.m_sim.spews, owner.m_sim.shots,
                      owner.m_sim.arenaActivations, owner.m_sim.tramples, owner.m_sim.rams,
                      owner.m_sim.pushes, owner.m_sim.blows, owner.m_sim.losses);
    if (!ref || !random || !events ||
        (native.stock &&
         (!resource(ref->resource) || resource(ref->resource)->stock != native.stock)) ||
        (owner.m_sim.collision && owner.m_sim.collision != m_bindings.collision) ||
        (owner.m_sim.hazards && owner.m_sim.hazards != m_bindings.hazards) ||
        (native.patrol.m_route && native.patrol.m_route != m_bindings.lookouts) ||
        native.patternStep > kMaxRows) {
        return std::nullopt;
    }
    Part out;
    out.id = owner.m_sim.id;
    out.incarnation = owner.m_sim.incarnation;
    out.collision = owner.m_sim.collision != nullptr;
    out.hazards = owner.m_sim.hazards != nullptr;
    out.scales = owner.m_sim.scales;
    out.realm = owner.m_sim.realm;
    out.fullHarm = owner.m_sim.fullHarm;
    out.pressure = owner.m_sim.pressure;
    out.arenaRandom = *random;
    out.events = std::move(*events);
    auto& a = out.actor;
    common(a, native);
    a.state = static_cast<u8>(native.state);
    a.definition = *ref;
    a.parent = parent;
    if (native.branch) {
        a.branch = static_cast<u32>(*native.branch);
    }
    if (native.floor) {
        a.floor = Floor{native.floor->object, native.floor->local};
    }
    for (const auto& t : native.targets) {
        a.targets.push_back({t.player, t.distance, t.score, t.inverseAnger, t.direction});
    }
    for (usize i = 0; i < a.playerDamage.size(); ++i) {
        const auto& d = native.playerDamage[i];
        a.playerDamage[i] = {d.dealt, d.received, d.dealtTime, d.receivedTime};
    }
    a.patternStep = static_cast<u32>(native.patternStep);
    a.patrolBound = native.patrol.m_route != nullptr;
    a.lookout = native.patrol.m_lookout;
    a.sight = native.patrol.m_sight;
    if (native.skin) {
        if (!native.definition) {
            return std::nullopt;
        }
        const auto sounds = native.definition->sounds();
        const auto found =
            std::ranges::find_if(sounds, [&](const auto& s) { return &s == native.skin; });
        if (found == sounds.end()) {
            return std::nullopt;
        }
        a.skin = static_cast<s32>(found - sounds.begin());
    }
    for (const auto& n : native.hitNodes) {
        a.hitNodes.push_back({n.health, n.flashTicks, n.broken, n.heldPose});
    }
    for (const auto& r : native.modelReplacements) {
        if (!native.stock || r.node >= TreePose::Checkpoint::kMaxNodes) {
            return std::nullopt;
        }
        const auto found =
            std::ranges::find_if(native.stock->replacementModels,
                                 [&](const auto& entry) { return &entry.second == r.resource; });
        if (found == native.stock->replacementModels.end()) {
            return std::nullopt;
        }
        a.replacements.push_back(
            {static_cast<u32>(r.node), found->first.first, found->first.second});
    }
    a.player = native.player.checkpoint();
    a.pose = native.pose.checkpoint();
    a.smooth = native.smooth.checkpoint();
    const auto& p = native.presentation;
    a.presentation = {
        p.valid, static_cast<u8>(p.state), p.generation, p.sequence, p.position, p.yaw,
        p.frame, p.pose.checkpoint()};
    if ((native.pose.posed() && !a.pose) || (native.smooth.posed() && !a.smooth) ||
        (p.pose.posed() && !a.presentation.pose)) {
        return std::nullopt;
    }
    for (const auto& attachment : native.attachments) {
        auto saved = attachment.pose.checkpoint();
        if (attachment.pose.posed() && !saved) {
            return std::nullopt;
        }
        a.attachments.push_back({attachment.player.checkpoint(), std::move(saved), attachment.world,
                                 attachment.previousFrame, attachment.previousGeneration});
    }
    for (usize i = 0; i < a.gaze.size(); ++i) {
        a.gaze[i] = native.gaze.tracked(i);
    }
    return out.valid() ? std::optional{std::move(out)} : std::nullopt;
}
std::optional<CombatantCheckpoint::State> CombatantCheckpoint::checkpoint() const {
    if (!bound() || m_owner.children().size() >= kMaxParts ||
        (m_owner.m_sim.actor.parent != nullptr)) {
        return std::nullopt;
    }
    State state;
    state.lookouts = m_lookouts;
    auto root = capturePart(m_owner, -1);
    if (!root) {
        return std::nullopt;
    }
    state.parts.push_back(std::move(*root));
    for (const auto& child : m_owner.children()) {
        if (!child || child->m_sim.actor.parent != &m_owner || !child->children().empty()) {
            return std::nullopt;
        }
        auto part = capturePart(*child, 0);
        if (!part) {
            return std::nullopt;
        }
        state.parts.push_back(std::move(*part));
    }
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
bool CombatantCheckpoint::preparePart(Combatant& staged, const Part& part,
                                      Combatant* parent) const {
    if (!part.valid() || (part.collision && !m_bindings.collision) ||
        (part.hazards && !m_bindings.hazards)) {
        return false;
    }
    const auto& a = part.actor;
    const auto* resolved = resource(a.definition.resource);
    auto* stock = resolved ? resolved->stock : nullptr;
    const auto* data = definition(a.definition);
    const auto* tree = stock ? stock->tree : nullptr;
    if (a.definition.resource == 0) {
        // Updating an empty native actor still captures presentation and marks
        // its empty smooth cache valid. Public arena/placement setters can also
        // retain scalar state before spawn. Preserve those values, but never
        // authorize an asset-bound pose/table reference without a definition.
        if (a.state != 0 || parent || a.definition.index != 0 || a.branch || a.player.playing ||
            a.pose || a.smooth || a.presentation.pose || a.move != -1 || a.grabMove != -1 ||
            a.pattern != -1 || a.skin != -1 || !a.moveTimes.empty() || !a.patternTimes.empty() ||
            !a.hitNodes.empty() || !a.replacements.empty() || !a.attachments.empty()) {
            return false;
        }
    } else {
        if ((stock == nullptr) || (tree == nullptr) || (data == nullptr) ||
            a.moveTimes.size() != data->moves().size() ||
            a.patternTimes.size() != data->patterns().size() ||
            a.hitNodes.size() != data->parts().size() ||
            a.attachments.size() != (a.definition.index == 0 ? stock->attachments.size() : 0) ||
            (a.move >= 0 && static_cast<usize>(a.move) >= data->moves().size()) ||
            (a.grabMove >= 0 && static_cast<usize>(a.grabMove) >= data->moves().size()) ||
            (a.pattern >= 0 &&
             (static_cast<usize>(a.pattern) >= data->patterns().size() ||
              a.patternStep > data->patterns()[static_cast<usize>(a.pattern)].moves.size())) ||
            (a.skin >= 0 && static_cast<usize>(a.skin) >= data->sounds().size()) ||
            (a.branch && *a.branch >= tree->nodes.size()) ||
            (a.presentation.valid && a.presentation.sequence >= tree->sequences.size()) ||
            (a.state != 0 && (!a.player.playing || !a.pose))) {
            return false;
        }
    }
    auto restoredShots = restoreShots(part.events.shots);
    if (!restoredShots) {
        return false;
    }
    auto& native = staged.m_sim.actor;
    common(native, a);
    native.state = static_cast<Combatant::State>(a.state);
    native.stock = stock;
    if (stock) {
        staged.m_sim.assets.push_back(stock->m_localBinding);
    }
    for (const auto& shot : part.events.shots) {
        const auto* binding = resource(shot.definition.resource);
        if (!binding) {
            return false;
        }
        if (std::ranges::none_of(staged.m_sim.assets, [&](const auto& asset) {
                return asset.lock() == binding->stock->m_localBinding;
            })) {
            staged.m_sim.assets.push_back(binding->stock->m_localBinding);
        }
    }
    native.definition = data;
    native.parent = parent;
    if (a.branch) {
        native.branch = static_cast<usize>(*a.branch);
    }
    if (a.floor) {
        native.floor = Combatant::Actor::Floor{a.floor->object, a.floor->local};
    }
    for (const auto& t : a.targets) {
        native.targets.push_back({t.player, t.distance, t.score, t.inverseAnger, t.direction});
    }
    for (usize i = 0; i < a.playerDamage.size(); ++i) {
        const auto& d = a.playerDamage[i];
        native.playerDamage[i] = {d.dealt, d.received, d.dealtTime, d.receivedTime};
    }
    native.patternStep = a.patternStep;
    native.patrol.m_route = a.patrolBound ? m_bindings.lookouts : nullptr;
    native.patrol.m_lookout = a.lookout;
    native.patrol.m_sight = a.sight;
    native.skin = a.skin >= 0 ? &data->sounds()[static_cast<usize>(a.skin)] : nullptr;
    for (const auto& n : a.hitNodes) {
        native.hitNodes.push_back({n.health, n.flashTicks, n.broken, n.heldPose});
    }
    for (const auto& r : a.replacements) {
        if (!stock || r.node >= tree->nodes.size()) {
            return false;
        }
        const auto found = stock->replacementModels.find({r.sourceNode, r.object});
        if (found == stock->replacementModels.end() ||
            tree->findNode(r.sourceNode, kCombatantNodeNameLength) != r.node ||
            std::ranges::any_of(native.modelReplacements,
                                [&](const auto& old) { return old.node == r.node; })) {
            return false;
        }
        native.modelReplacements.push_back({r.node, &found->second});
    }
    if (!restorePlayer(native.player, a.player, tree) || !restorePose(native.pose, a.pose, tree) ||
        !restorePose(native.smooth, a.smooth, tree) ||
        !restorePose(native.presentation.pose, a.presentation.pose, tree)) {
        return false;
    }
    if (a.pose && std::ranges::any_of(a.hitNodes,
                                      [](const auto& node) { return node.heldPose.has_value(); })) {
        auto held = *a.pose;
        for (usize index = 0; index < a.hitNodes.size(); ++index) {
            if (!a.hitNodes[index].heldPose) {
                continue;
            }
            const auto node = tree->findNode(data->parts()[index].node, kCombatantNodeNameLength);
            if (!node || *node >= held.poses.size()) {
                return false;
            }
            held.poses[*node] = *a.hitNodes[index].heldPose;
        }
        TreePose validated;
        if (!validated.restore(*tree, held)) {
            return false;
        }
    }
    auto& p = native.presentation;
    p.valid = a.presentation.valid;
    p.state = static_cast<Combatant::State>(a.presentation.state);
    p.generation = a.presentation.generation;
    p.sequence = a.presentation.sequence;
    p.position = a.presentation.position;
    p.yaw = a.presentation.yaw;
    p.frame = a.presentation.frame;
    for (usize i = 0; i < a.attachments.size(); ++i) {
        const auto& value = a.attachments[i];
        Combatant::Actor::Attachment attachment;
        if (!restorePlayer(attachment.player, value.player, stock->attachments[i].tree) ||
            !restorePose(attachment.pose, value.pose, stock->attachments[i].tree)) {
            return false;
        }
        attachment.world = value.world;
        attachment.previousFrame = value.previousFrame;
        attachment.previousGeneration = value.previousGeneration;
        native.attachments.push_back(std::move(attachment));
    }
    for (usize i = 0; i < a.gaze.size(); ++i) {
        native.gaze.m_tracked[i] = {a.gaze[i].x, a.gaze[i].y};
    }
    staged.m_sim.incarnation = part.incarnation;
    staged.m_sim.id = part.id;
    staged.m_sim.collision = part.collision ? m_bindings.collision : nullptr;
    staged.m_sim.hazards = part.hazards ? m_bindings.hazards : nullptr;
    staged.m_sim.scales = part.scales;
    staged.m_sim.realm = part.realm;
    staged.m_sim.fullHarm = part.fullHarm;
    staged.m_sim.pressure = part.pressure;
    staged.m_sim.arenaRandom.seed(part.arenaRandom);
    staged.m_sim.cues = part.events.cues;
    staged.m_sim.grabs = part.events.grabs;
    staged.m_sim.spews = part.events.spews;
    staged.m_sim.shots = std::move(*restoredShots);
    staged.m_sim.arenaActivations = part.events.arena;
    staged.m_sim.tramples = part.events.tramples;
    staged.m_sim.rams = part.events.rams;
    staged.m_sim.pushes = part.events.pushes;
    staged.m_sim.blows = part.events.blows;
    staged.m_sim.losses = part.events.losses;
    return true;
}
std::optional<CombatantCheckpoint::Prepared>
CombatantCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.lookouts != m_lookouts ||
        !m_owner.m_obstacles.empty() || !m_owner.m_swarm.empty() || m_owner.m_sim.actor.parent) {
        return std::nullopt;
    }
    const auto& root = state.parts.front().actor;
    const auto* stock = resource(root.definition.resource);
    if (root.definition.index != 0 || root.branch ||
        state.parts.size() != (stock ? stock->stock->children.size() + 1 : 1)) {
        return std::nullopt;
    }
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->wrapper = this;
    prepared->owner = &m_owner;
    prepared->lifetime = m_lifetime;
    prepared->binding = m_owner.m_checkpointBinding;
    prepared->revision = m_owner.m_checkpointRevision;
    prepared->successor = std::make_shared<const u8>(0);
    prepared->staged = std::make_unique<Combatant>();
    if (!preparePart(*prepared->staged, state.parts.front(), nullptr)) {
        return std::nullopt;
    }
    for (usize i = 1; i < state.parts.size(); ++i) {
        const auto& part = state.parts[i];
        const auto& a = part.actor;
        if ((stock == nullptr) || a.definition.resource != root.definition.resource ||
            a.definition.index != i || part.id != state.parts.front().id + static_cast<s32>(i) ||
            part.collision ||
            a.branch != stock->stock->tree->findNode(stock->stock->children[i - 1].rootNode(),
                                                     kCombatantNodeNameLength)) {
            return std::nullopt;
        }
        auto child = std::make_unique<Combatant>();
        // Bind the staged child directly to the stable destination root, not the
        // temporary root which is destroyed after commit.
        if (!preparePart(*child, part, &m_owner)) {
            return std::nullopt;
        }
        prepared->staged->m_children.push_back(std::move(child));
        ++prepared->staged->m_sim.childCount;
    }
    Prepared out{std::move(prepared)};
    return canCommit(out) ? std::optional{std::move(out)} : std::nullopt;
}
bool CombatantCheckpoint::canCommit(const Prepared& value) const {
    return value.m_data && value.m_data->wrapper == this && value.m_data->owner == &m_owner &&
           value.m_data->lifetime == m_lifetime &&
           value.m_data->binding == m_owner.m_checkpointBinding &&
           value.m_data->revision == m_owner.m_checkpointRevision && value.m_data->staged &&
           bound() && m_owner.m_obstacles.empty() && m_owner.m_swarm.empty();
}
void CombatantCheckpoint::install(Combatant& destination, Combatant& staged) {
    using std::swap;
    swap(destination.m_sim, staged.m_sim);
    destination.m_children.swap(staged.m_children);
    destination.m_obstacles = {};
    destination.m_swarm = {};
    // Portable diagnostic restores may replace child storage. Local snapshots
    // cannot cross that operation; ordinary in-process rewind never does so.
    ++destination.m_localEpoch;
}
bool CombatantCheckpoint::commitRestore(Prepared&& value) {
    if (!canCommit(value)) {
        return false;
    }
    auto prepared = std::move(value.m_data);
    install(m_owner, *prepared->staged);
    m_owner.m_checkpointBinding = std::move(prepared->successor);
    return true;
}
bool CombatantCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
