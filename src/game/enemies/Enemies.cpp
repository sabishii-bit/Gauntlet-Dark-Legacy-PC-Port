#include "game/enemies/Enemies.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"
#include "engine/world/SelectionHighlight.h"

#include "game/combat/BodyContact.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/NativeStateResources.h"
#include "game/players/EnemyShrink.h"

namespace gdl::game {

void visitNativeState(NativeStateDigest& digest, const EnemyScales& value) {
    digest.fields<9>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyBlow& value) {
    digest.fields<8>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyLoss& value) {
    digest.fields<8>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyBody& value) {
    digest.fields<4>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyGeneratorEvent& value) {
    digest.fields<2>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemySpawn& value) {
    digest.fields<16>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyDeathShot& value) {
    digest.fields<4>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyBurst& value) {
    digest.fields<6>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyBirth& value) {
    digest.fields<3>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyCue& value) {
    digest.fields<5>(value);
}
void visitNativeState(NativeStateDigest& digest, const EnemyFeedback& value) {
    digest.fields<12>(value);
}
void visitNativeState(NativeStateDigest& digest, const DeathEvent& value) {
    digest.fields<7>(value);
}
void visitNativeState(NativeStateDigest& digest, const ZigZag& value) {
    digest.fields<5>(value);
}
void visitNativeState(NativeStateDigest& digest, const MindMemory& value) {
    digest.fields<18>(value);
}
void visitNativeState(NativeStateDigest& digest, const LookoutRoute& value) {
    digest.fields<2>(value);
}
void visitNativeState(NativeStateDigest& digest, const Enemies::BodyState::Floor& value) {
    digest.fields<2>(value);
}
void visitNativeState(NativeStateDigest& digest, const Enemies::BodyState& value) {
    digest.fields<58>(value);
}

namespace {

// A derived record with data in its base cannot be decomposed by C++. Its real
// base is visited separately; this guard detects an unvisited derived member.
template <class T>
constexpr bool kHasExtraEnemyMember = requires { T{Enemies::BodyState{}, EnemyAnimator{}, {}}; };

template <class T> bool sameNativeField(const T& left, const T& right) {
    return left == right;
}

constexpr f32 kPi = std::numbers::pi_v<f32>;
// Height of the horizontal wall slice above the supported feet.
constexpr f32 kWallHeight = 2.0f;
constexpr f32 kSpawnDrop = 6.0f;       ///< maximum landing distance from the birth collision centre
constexpr s32 kFarRecycleCost = 10000; ///< an unseen enemy is that much cheaper to reuse
constexpr f32 kPushFloor = 0.01f;
constexpr f32 kPushFrameRate = 30.0f; ///< knock-back decays once per game frame
constexpr f32 kGravity = 100.0f;
constexpr f32 kDeathSkinRate = 15.0f;
constexpr f32 kOnScreenMargin = 15.0f; ///< past twice its radius, what still counts as in view
constexpr f32 kBomberScare = 10.0f;    ///< the swarm keeps this far from a lit suicide bomber
constexpr f32 kCastTicks = 90.0f;      ///< a caster's least wait, times the level's missile rate
constexpr s32 kContactGridDimension = 64;
constexpr s32 kScorpionKind = 0;
constexpr s32 kDogKind = 18;
constexpr s32 kLeaderWay = 12;
constexpr s32 kAcidKind = 21;
constexpr f32 kShadowReach = 1.0f; ///< a shadow finds its floor within this of the feet

constexpr f32 kCheckpointMagnitude = 1'000'000;
constexpr s32 kCheckpointCounter = 100'000'000;

bool checkpointNumber(f32 value, f32 minimum = -kCheckpointMagnitude,
                      f32 maximum = kCheckpointMagnitude) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool checkpointCounter(s32 value, s32 minimum = -kCheckpointCounter) {
    return value >= minimum && value <= kCheckpointCounter;
}

bool checkpointPoint(const Vec3& value) {
    return checkpointNumber(value.x) && checkpointNumber(value.y) && checkpointNumber(value.z);
}

bool checkpointPlayer(s32 player) {
    return player >= -1 && player < 4;
}

bool checkpointKind(s32 kind) {
    return kind >= 0 && kind < kEnemyKindCount;
}

bool checkpointSlot(s32 slot) {
    return slot >= -1 && slot < Enemies::kMost;
}

bool checkpointMind(const MindMemory& mind) {
    if (!checkpointNumber(mind.heading) || !checkpointNumber(mind.headingBefore) ||
        !checkpointNumber(mind.seekHeading) || mind.effectiveWay < -1 || mind.effectiveWay > 31 ||
        !checkpointCounter(mind.turns, 0) || (mind.effectiveWay == kFleeWay && mind.counter < 0)) {
        return false;
    }
    return std::ranges::all_of(
        std::initializer_list<s32>{mind.route, mind.collided, mind.stuck, mind.deadEnd,
                                   mind.counter, mind.mode, mind.fuse, mind.lookout,
                                   mind.zigZag.count, mind.zigZag.side, mind.zigZag.spread,
                                   mind.zigZag.hold, mind.zigZag.swings},
        [](s32 value) { return checkpointCounter(value); });
}

bool checkpointEvent(const EnemyBlow& value) {
    return checkpointPlayer(value.player) && checkpointKind(value.kind) && value.tier >= 1 &&
           value.tier <= 3 && checkpointNumber(value.damage, 0) &&
           checkpointPoint(value.direction) && value.ward <= EnemyMeleeWard::HealthVamp;
}
bool checkpointEvent(const EnemyLoss& value) {
    return checkpointSlot(value.enemy) && checkpointKind(value.kind) && value.tier >= 1 &&
           value.tier <= 3 && checkpointPlayer(value.player) &&
           checkpointCounter(value.experience, 0) && checkpointPoint(value.position);
}
bool checkpointEvent(const EnemyBurst& value) {
    return checkpointSlot(value.enemy) && (value.kind == -1 || checkpointKind(value.kind)) &&
           checkpointNumber(value.damage, 0) && checkpointPoint(value.position) &&
           (value.instance == 0 || value.enemy >= 0);
}
bool checkpointEvent(const EnemyBirth& value) {
    return checkpointKind(value.kind) && value.tier >= 1 && value.tier <= 3 &&
           checkpointPoint(value.position);
}
bool checkpointEvent(const EnemyCue& value) {
    return value.kind <= EnemyCue::Kind::Yell && checkpointKind(value.enemyKind) &&
           checkpointPoint(value.position) && checkpointSlot(value.enemy) &&
           ((value.enemy == -1 && value.instance == 0) ||
            (value.enemy >= 0 && value.instance != 0));
}
bool checkpointEvent(const EnemyFeedback& value) {
    return checkpointKind(value.kind) && value.tier >= 1 && value.tier <= 3 &&
           checkpointCounter(value.hitCount, 0) && checkpointPoint(value.position) &&
           checkpointNumber(value.yaw) && checkpointNumber(value.halfHeight, 0) &&
           checkpointSlot(value.enemy) && (value.instance == 0 || value.enemy >= 0) &&
           checkpointNumber(value.remainingHealth);
}
bool checkpointEvent(const DeathEvent& value) {
    return value.kind <= DeathEvent::Kind::Unmoved && checkpointSlot(value.enemy) &&
           checkpointPlayer(value.player) && value.form <= DeathForm::Black &&
           checkpointNumber(value.amount, 0) && checkpointPoint(value.position) &&
           (value.instance == 0 || value.enemy >= 0);
}
bool checkpointEvent(const EnemyDeathShot& value) {
    return checkpointSlot(value.enemy) && checkpointKind(value.kind) &&
           checkpointPoint(value.position) && checkpointPoint(value.direction);
}
bool checkpointEvent(const EnemyGeneratorEvent& value) {
    return checkpointCounter(value.generator, -1) &&
           value.kind <= EnemyGeneratorEvent::Kind::PatrolDetached;
}
bool checkpointEvent(s32 player) {
    return checkpointPlayer(player);
}

template <class T> bool checkpointEvents(const std::vector<T>& events) {
    return events.size() <= Enemies::Checkpoint::kMaxEvents &&
           std::ranges::all_of(events, [](const T& value) { return checkpointEvent(value); });
}

constexpr s32 kRetargetEvery = 8; ///< frames between a mind looking round again
constexpr f32 kRunFrom = 1.25f;   ///< a pace this much over a walk's runs
constexpr f32 kStopped = 0.01f;   ///< a step that gets less than this is a dead stop

// fn_8005F0F4 lets an overlapping body leave an item. Cylinders use the radial
// dot product; boxes only do so when neither local axis is moving farther inside.
// This belongs to enemy item queries, not the swept walls/projectiles API.
bool separatingFromItem(const Obstacle& box, const Vec3& from, const Vec3& to, f32 radius) {
    if (!box.enemyItem) {
        return false;
    }
    const Vec2 start{from.x - box.centre.x, from.z - box.centre.z};
    const Vec2 step{to.x - from.x, to.z - from.z};
    if (glm::dot(start, step) <= 0) {
        return false;
    }
    if (box.enemyItem->cylindrical) {
        return true;
    }
    const f32 c = portable::cos(box.yaw);
    const f32 s = portable::sin(box.yaw);
    const Vec2 local{start.x * c - start.y * s, start.x * s + start.y * c};
    const Vec2 delta{step.x * c - step.y * s, step.x * s + step.y * c};
    return std::abs(local.x) <= box.halfAcross + radius &&
           std::abs(local.y) <= box.halfAlong + radius && local.x * delta.x >= 0 &&
           local.y * delta.y >= 0;
}

Obstacle enemyItemShape(const Obstacle& box) {
    Obstacle shape = box;
    if (shape.enemyItem) {
        shape.cylinderRadius = shape.enemyItem->cylindrical ? shape.enemyItem->radius : 0;
    }
    return shape;
}

bool itemBlocksEnemy(const Obstacle& box, const Vec3& from, const Vec3& to, f32 radius) {
    if (!box.enemyItem) {
        return box.contact(from, to, radius).has_value();
    }
    // Native box/cylinder birth and route queries test the destination, not a
    // swept capsule. Retain the obstacle's vertical envelope for these foot positions.
    const Vec2 delta{to.x - box.centre.x, to.z - box.centre.z};
    const f32 reach = box.enemyItem->radius + radius;
    if (glm::dot(delta, delta) > reach * reach || separatingFromItem(box, from, to, radius)) {
        return false;
    }
    return enemyItemShape(box).contact(to, to, radius).has_value();
}

/** fn_8004646C asks CritterMoveNodeCol before the swarm grid. Mode 2 uses only
 * solid nodes (and the root fallback), expanding their vertical radius by the
 * mover's radius, not its humanoid height. */
const EnemyBody* combatantContact(std::span<const EnemyBody> bodies, const Vec3& from,
                                  const Vec3& to, f32 radius) {
    for (const auto& body : bodies) {
        if (movementTouchesBody(from, to, body.centre, radius + body.radius,
                                radius + body.halfHeight)) {
            return &body;
        }
    }
    return nullptr;
}

// The octants about a generator, as its facing is turned into each.
Vec3 octant(const Vec3& v, s32 direction, f32& yawOffset) {
    constexpr f32 kHalfRoot = 0.707f;
    const f32 x = v.x;
    const f32 z = v.z;
    switch (direction) {
    case 1: yawOffset = kPi; return Vec3{-x, v.y, -z};
    case 2: yawOffset = -kPi / 2.0f; return Vec3{-z, v.y, x};
    case 3: yawOffset = kPi / 2.0f; return Vec3{z, v.y, -x};
    case 4:
        yawOffset = kPi / 4.0f;
        return Vec3{kHalfRoot * x + kHalfRoot * z, v.y, kHalfRoot * -x + kHalfRoot * z};
    case 5:
        yawOffset = -kPi / 4.0f;
        return Vec3{kHalfRoot * -z + kHalfRoot * x, v.y, kHalfRoot * z + kHalfRoot * x};
    case 6:
        yawOffset = 3.0f * kPi / 4.0f;
        return Vec3{kHalfRoot * z - kHalfRoot * x, v.y, kHalfRoot * -x - kHalfRoot * z};
    case 7:
        yawOffset = -3.0f * kPi / 4.0f;
        return Vec3{kHalfRoot * -z - kHalfRoot * x, v.y, kHalfRoot * z - kHalfRoot * x};
    default: yawOffset = 0.0f; return v;
    }
}

// The octants a kind may be born into: the humanoids only ahead of the generator.
u32 octantMaskOf(s32 kind) {
    switch (kind) {
    case 1:
    case 4:
    case 5:
    case 7:
    case 8:
    case 10:
    case 11:
    case 14:
    case 15:
    case 19:
    case 24:
    case 25: return 0xFFCE;
    default: return 0;
    }
}

// Which way to turn round something at `to`: along the axis of the wider gap.
s32 turnDirection(const Vec3& from, const Vec3& to) {
    if (std::abs(from.x - to.x) >= std::abs(from.z - to.z)) {
        return from.z < to.z ? 1 : -1;
    }
    return from.x < to.x ? -1 : 1;
}

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

s32 resolvedWayOf(s32 kind, s32 strength, s32 way, bool mirrored) {
    constexpr s32 kMostWay = 31;
    constexpr s32 kUnsetWay = 0;
    constexpr s32 kCast = 30;
    constexpr s32 kCasterStrength = 3;
    constexpr s32 kWayOne = 1;        ///< never run: taken as seeking
    constexpr s32 kMilestoneWay = 10; ///< never run: taken as chasing
    if (way < 0 || way > kMostWay) {
        way = enemyKind(kind).algorithm;
    }
    // These go about their own way whatever they are placed with (fn_8004F87C).
    if (kind == kGarmBroodKind) {
        return kLungeWay;
    }
    if (kind == kItKind) {
        return kLurkWay;
    }
    // The kinds whose third tier casts: demons, sorcerers, plague, worms, warlocks.
    const bool caster = kind == 2 || kind == 7 || kind == 14 || kind == 17 || kind == 24;
    const bool medium = caster || kind == 1 || kind == 4 || kind == 5 || kind == 8 || kind == 10 ||
                        kind == 11 || kind == 13 || kind == 16 || kind == 19 || kind == 20 ||
                        kind == 25 || kind == 32 || kind == 33;
    if (smallKind(kind)) {
        if (way != kProwlWay && way != kMirroredProwlWay) {
            way = mirrored ? kMirroredProwlWay : kProwlWay;
        }
    } else if (medium && way == kUnsetWay) {
        switch (strength) {
        case kCasterStrength: way = caster ? kCast : kChaseWay; break;
        case kArcherStrength: way = kThrowWay; break;
        case kBomberStrength: way = kBombWay; break;
        case kSuicideStrength: way = kSuicideWay; break;
        default: way = kChaseWay; break;
        }
    }
    if (way == kWayOne) {
        way = kSeekWay;
    } else if (way == kMilestoneWay) {
        way = kChaseWay;
    }
    return way;
}

bool Enemies::BodyState::valid() const {
    if (state > State::Dying || presentationState > State::Dying || birth > Birth::Patrol ||
        !checkpointKind(kind) || tier < 1 || tier > 3 || algorithm < 0 || algorithm > 31 ||
        !checkpointCounter(variant, 0) || affinity < 0 || affinity > 4 ||
        !checkpointCounter(generator, -1) || !checkpointPoint(position) ||
        !checkpointPoint(birthPosition) || !checkpointPoint(presentationPosition) ||
        !checkpointPoint(push) || !checkpointPoint(hurtDirection) || !checkpointMind(mind) ||
        !checkpointPlayer(target) || !checkpointPlayer(targetBefore) ||
        !checkpointPlayer(contact) || !checkpointPlayer(attackIndex) || !checkpointPlayer(hurtBy) ||
        !checkpointNumber(veil, 0, kVeiled) || !checkpointNumber(health) ||
        !checkpointNumber(yaw) || !checkpointNumber(presentationYaw) ||
        !checkpointNumber(deathSeconds) ||
        (floor && (!checkpointCounter(floor->object, 0) || !checkpointPoint(floor->local))) ||
        (!deathSkin.empty() && std::ranges::find(kDeathSkins, deathSkin) == kDeathSkins.end())) {
        return false;
    }
    for (const f32 value : {fullHealth, sight, radius, height, reach, pushMagnitude, targetDistance,
                            weightedDistance, hurtPending, flashSeconds, burnGap}) {
        if (!checkpointNumber(value, 0)) {
            return false;
        }
    }
    return std::ranges::all_of(std::initializer_list<s32>{pushes, attackCount, stunTicks,
                                                          drainTicks, endurance, otherSide,
                                                          hitCount, veilClock, deathSkinFrames},
                               [](s32 value) { return checkpointCounter(value); });
}

bool Enemies::Checkpoint::valid() const {
    if (!random.valid() || nextInstance == 0 || most < 1 || most > kMost ||
        stocks.size() > kEnemyKindCount || !checkpointSlot(bomber) || inView < 0 ||
        inView > kMost || pendingTicks < 0 || pendingTicks >= 2 ||
        !checkpointNumber(pendingSeconds, 0, 3600) || (pendingTicks == 0 && pendingSeconds != 0) ||
        !checkpointNumber(shrink, 0.001f, 16) || !checkpointNumber(contactGridOrigin.x) ||
        !checkpointNumber(contactGridOrigin.y) || !checkpointNumber(contactGridInverseWidth, 0) ||
        scales.players < 0 || scales.players > 4 || lookouts.points.size() > LookoutRoute::kMost ||
        lookouts.points.size() != lookouts.next.size() || combatants.size() > kMaxCombatants ||
        !checkpointEvents(blows) || !checkpointEvents(losses) || !checkpointEvents(bursts) ||
        !checkpointEvents(births) || !checkpointEvents(cues) || !checkpointEvents(feedback) ||
        !checkpointEvents(deathEvents) || !checkpointEvents(deathShots) ||
        !checkpointEvents(tagged) || !checkpointEvents(generatorEvents)) {
        return false;
    }
    for (const f32 scale : {scales.health, scales.speed, scales.sight, scales.damage,
                            scales.missileRate, scales.missileAim, scales.playerLevel}) {
        if (!checkpointNumber(scale, 0, 10000)) {
            return false;
        }
    }
    if (view && !view->bounded(kCheckpointMagnitude)) {
        return false;
    }
    for (usize index = 0; index < lookouts.points.size(); ++index) {
        // An authored route may refer past the retained twenty lookouts. Native
        // patrol logic reselects next tick; preserve that value, not a fabricated edge.
        if (!checkpointPoint(lookouts.points[index]) || !checkpointCounter(lookouts.next[index])) {
            return false;
        }
    }
    for (const auto& body : combatants) {
        if (!checkpointCounter(body.id) || !checkpointPoint(body.centre) ||
            !checkpointNumber(body.radius, 0) || !checkpointNumber(body.halfHeight, 0)) {
            return false;
        }
    }
    for (usize index = 0; index < stocks.size(); ++index) {
        const auto& stock = stocks[index];
        if (!checkpointKind(stock.kind) || !stock.textures.valid() ||
            (index > 0 && stocks[index - 1].kind >= stock.kind)) {
            return false;
        }
    }
    for (usize index = 0; index < enemies.size(); ++index) {
        const auto& body = enemies[index];
        if (!body.valid() || body.instance >= nextInstance ||
            (body.animator && !body.animator->valid()) ||
            (body.state != State::Inactive &&
             (index >= static_cast<usize>(most) || body.instance == 0 || !body.animator))) {
            return false;
        }
        for (usize prior = 0; body.instance != 0 && prior < index; ++prior) {
            if (enemies[prior].instance == body.instance) {
                return false;
            }
        }
    }
    for (const auto& cue : cues) {
        if (cue.instance >= nextInstance ||
            (cue.enemy >= 0 && enemies[static_cast<usize>(cue.enemy)].instance == cue.instance &&
             enemies[static_cast<usize>(cue.enemy)].kind != cue.enemyKind)) {
            return false;
        }
    }
    // Diagnostic fixtures may omit a source lifetime. Native emissions always
    // carry it, but the slot is allowed to have changed hands before consumption.
    const auto sourceValid = [&](const auto& event, s32 kind) {
        return event.instance < nextInstance &&
               (event.instance == 0 ||
                enemies[static_cast<usize>(event.enemy)].instance != event.instance || kind < 0 ||
                enemies[static_cast<usize>(event.enemy)].kind == kind);
    };
    for (const auto& event : feedback) {
        if (!sourceValid(event, event.kind)) {
            return false;
        }
    }
    for (const auto& event : deathEvents) {
        if (!sourceValid(event, kDeathKind)) {
            return false;
        }
    }
    return std::ranges::all_of(bursts,
                               [&](const auto& event) { return sourceValid(event, event.kind); });
}

bool Enemies::Checkpoint::operator==(const Checkpoint& other) const {
    const auto fields = [](const Checkpoint& value) {
        return std::tie(value.enemies, value.stocks, value.nextInstance, value.random, value.most,
                        value.scales, value.lookouts, value.blows, value.losses, value.bursts,
                        value.births, value.cues, value.feedback, value.deathEvents,
                        value.deathShots, value.tagged, value.generatorEvents, value.combatants,
                        value.contactGridOrigin, value.contactGridInverseWidth, value.bomber,
                        value.inView, value.frame, value.pendingTicks, value.pendingSeconds,
                        value.shrink);
    };
    return fields(*this) == fields(other) && view == other.view;
}

std::optional<Enemies::Checkpoint> Enemies::checkpoint() const {
    if (m_device == nullptr) {
        return std::nullopt;
    }
    Checkpoint state;
    for (usize index = 0; index < m_simulation.enemies.size(); ++index) {
        const auto& enemy = m_simulation.enemies[index];
        auto& body = state.enemies[index];
        static_cast<BodyState&>(body) = static_cast<const BodyState&>(enemy);
        body.animator = enemy.animator.checkpoint();
        if (enemy.animator.bound() && !body.animator) {
            return std::nullopt;
        }
    }
    for (const auto& stock : m_simulation.stocks) {
        state.stocks.push_back({stock->kind, stock->textures.checkpoint()});
    }
    // Loading placed enemies before optional presentation preloads can produce
    // a different stock order on another machine. The value roster is canonical;
    // native stock iteration and each already-bound resource remain untouched.
    std::ranges::sort(state.stocks, {}, &Checkpoint::Stock::kind);
    state.nextInstance = m_simulation.nextInstance;
    state.random = m_simulation.random.checkpoint();
    state.most = m_simulation.most;
    state.scales = m_simulation.scales;
    state.lookouts = m_simulation.lookouts;
    state.blows = m_simulation.blows;
    state.losses = m_simulation.losses;
    state.bursts = m_simulation.bursts;
    state.births = m_simulation.births;
    state.cues = m_simulation.cues;
    state.feedback = m_simulation.feedback;
    state.deathEvents = m_simulation.deathEvents;
    state.deathShots = m_simulation.deathShots;
    state.tagged = m_simulation.tagged;
    state.generatorEvents = m_simulation.generatorEvents;
    state.combatants = m_simulation.combatants;
    state.contactGridOrigin = m_simulation.contactGridOrigin;
    state.contactGridInverseWidth = m_simulation.contactGridInverseWidth;
    state.bomber = m_simulation.bomber;
    state.view = m_simulation.view;
    state.inView = m_simulation.inView;
    state.frame = m_simulation.frame;
    state.pendingTicks = m_simulation.pendingTicks;
    state.pendingSeconds = m_simulation.pendingSeconds;
    state.shrink = m_simulation.shrink;
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

struct Enemies::Prepared::Data {
    const Enemies* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    const WorldCollision* collision = nullptr;
    const HazardSurfaces* hazards = nullptr;
    Checkpoint values;
    std::vector<const Stock*> stocks;
    std::vector<TextureAnimator> textures;
    std::array<Enemy, kMost> enemies;
};

Enemies::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
Enemies::Prepared::Prepared(Prepared&&) noexcept = default;
Enemies::Prepared& Enemies::Prepared::operator=(Prepared&&) noexcept = default;
Enemies::Prepared::~Prepared() = default;

std::optional<Enemies::Prepared> Enemies::prepareRestore(const Checkpoint& state) const {
    if (m_device == nullptr || !state.valid() ||
        state.stocks.size() != m_simulation.stocks.size()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_restoreOwner;
    data->generation = m_restoreGeneration;
    data->collision = m_collision;
    data->hazards = m_hazards;
    auto& textures = data->textures;
    textures.reserve(m_simulation.stocks.size());
    data->stocks.reserve(m_simulation.stocks.size());
    for (const auto& stock : m_simulation.stocks) {
        const auto source = std::ranges::find(state.stocks, stock->kind, &Checkpoint::Stock::kind);
        if (source == state.stocks.end()) {
            return std::nullopt;
        }
        data->stocks.push_back(stock);
        textures.push_back(stock->textures);
        if (!textures.back().restore(source->textures)) {
            return std::nullopt;
        }
    }
    auto& enemies = data->enemies;
    for (usize index = 0; index < state.enemies.size(); ++index) {
        const auto& body = state.enemies[index];
        auto& enemy = enemies[index];
        static_cast<BodyState&>(enemy) = static_cast<const BodyState&>(body);
        if (body.floor &&
            (m_collision == nullptr || !m_collision->objectTransform(body.floor->object))) {
            return std::nullopt;
        }
        if (!body.animator) {
            continue;
        }
        const Stock* stock = stockOf(body.kind);
        if (stock == nullptr) {
            return std::nullopt;
        }
        const TreeInfo* tree = treeOf(body.kind, body.tier);
        if (body.variant >= kArcherStrength) {
            const auto variant = static_cast<usize>(body.variant - kArcherStrength);
            if (variant < stock->variantTrees.size() && stock->variantTrees[variant] != nullptr) {
                tree = stock->variantTrees[variant];
            }
        }
        if (tree == nullptr || !enemy.animator.bind(*tree) ||
            !enemy.animator.restore(*body.animator)) {
            return std::nullopt;
        }
    }
    // Copy the remaining value buffers before committing any state; resource
    // validation and allocation failure cannot leave a partly restored pool.
    data->values = state;
    return Prepared{std::move(data)};
}

bool Enemies::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data || prepared.m_data->owner != this || m_device == nullptr ||
        prepared.m_data->lifetime != m_restoreOwner ||
        prepared.m_data->generation != m_restoreGeneration ||
        prepared.m_data->collision != m_collision || prepared.m_data->hazards != m_hazards ||
        prepared.m_data->stocks.size() != m_simulation.stocks.size()) {
        return false;
    }
    for (usize index = 0; index < m_simulation.stocks.size(); ++index) {
        if (prepared.m_data->stocks[index] != m_simulation.stocks[index]) {
            return false;
        }
    }
    return std::ranges::none_of(prepared.m_data->enemies, [&](const auto& enemy) {
        return enemy.floor &&
               (m_collision == nullptr || !m_collision->objectTransform(enemy.floor->object));
    });
}

bool Enemies::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    auto& values = data->values;
    m_simulation.enemies = std::move(data->enemies);
    for (usize index = 0; index < m_simulation.stocks.size(); ++index) {
        m_simulation.stocks[index]->textures = std::move(data->textures[index]);
    }
    m_simulation.nextInstance = values.nextInstance;
    m_simulation.random.restore(values.random);
    m_simulation.most = values.most;
    m_simulation.scales = values.scales;
    m_simulation.lookouts = std::move(values.lookouts);
    m_simulation.blows = std::move(values.blows);
    m_simulation.losses = std::move(values.losses);
    m_simulation.bursts = std::move(values.bursts);
    m_simulation.births = std::move(values.births);
    m_simulation.cues = std::move(values.cues);
    m_simulation.feedback = std::move(values.feedback);
    m_simulation.deathEvents = std::move(values.deathEvents);
    m_simulation.deathShots = std::move(values.deathShots);
    m_simulation.tagged = std::move(values.tagged);
    m_simulation.generatorEvents = std::move(values.generatorEvents);
    m_simulation.combatants = std::move(values.combatants);
    m_simulation.contactGridOrigin = values.contactGridOrigin;
    m_simulation.contactGridInverseWidth = values.contactGridInverseWidth;
    m_simulation.bomber = values.bomber;
    m_simulation.view = values.view;
    m_simulation.inView = values.inView;
    m_simulation.frame = values.frame;
    m_simulation.pendingTicks = values.pendingTicks;
    m_simulation.pendingSeconds = values.pendingSeconds;
    m_simulation.shrink = values.shrink;
    ++m_restoreGeneration;
    return true;
}

bool Enemies::restore(const Checkpoint& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

bool Enemies::saveLocal(LocalSnapshot& destination) const {
    if (m_device == nullptr) {
        return false;
    }
    destination.m_simulation = m_simulation;
    destination.m_textureClocks.resize(m_simulation.stocks.size());
    for (usize index = 0; index < m_simulation.stocks.size(); ++index) {
        destination.m_textureClocks[index] = m_simulation.stocks[index]->textures.clock();
    }
    destination.m_owner = m_restoreOwner;
    destination.m_epoch = m_resourceEpoch;
    destination.m_collision = m_collision;
    destination.m_hazards = m_hazards;
    return true;
}

bool Enemies::canRestoreLocal(const LocalSnapshot& source) const {
    return m_device != nullptr && source.m_owner == m_restoreOwner &&
           source.m_epoch == m_resourceEpoch && source.m_collision == m_collision &&
           source.m_hazards == m_hazards &&
           source.m_textureClocks.size() == source.m_simulation.stocks.size();
}

bool Enemies::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    if (!canRestoreLocal(left) || !canRestoreLocal(right)) {
        return false;
    }
    if (left.m_textureClocks != right.m_textureClocks) {
        return false;
    }
    bool equal = false;
    // Same native traversal as hashing, retaining same-owner pointer equality
    // for immutable resources and comparing captured rather than live clocks.
    visitNativeFields<27>(left.m_simulation, [&](const auto&... first) {
        visitNativeFields<27>(right.m_simulation, [&](const auto&... second) {
            equal = (sameNativeField(first, second) && ...);
        });
    });
    return equal;
}

void Enemies::appendSimulationDigest(
    NativeStateDigest& digest, const Simulation& state,
    std::optional<std::span<const TextureAnimator::Clock>> savedClocks) {
    static_assert(std::is_aggregate_v<Enemy>);
    static_assert(requires { Enemy{BodyState{}, EnemyAnimator{}}; });
    static_assert(!kHasExtraEnemyMember<Enemy>);
    const auto append = [&]<class T>(const T& field) {
        if constexpr (std::is_same_v<T, std::vector<Stock*>>) {
            digest.wide(field.size());
            for (usize index = 0; index < field.size(); ++index) {
                if (field[index] == nullptr) {
                    digest.invalidate();
                    return;
                }
                const auto& [kind, archive, textures, trees, bodies, variantTrees, variantBodies,
                             arrow, bomb, fireball, deathStatues, deathStatueTrees, shadows,
                             unseen] = *field[index];
                digest.value(kind);
                digest.resource(archive.loaded() ? &archive : nullptr);
                for (const auto* tree : trees) {
                    digest.resource(tree);
                }
                for (const auto* tree : variantTrees) {
                    digest.resource(tree);
                }
                for (const auto* tree : deathStatueTrees) {
                    digest.resource(tree);
                }
                // TreeModels and BlobShadows are shared render caches, never
                // copied into native history. Only immutable availability counts.
                for (const auto& body : bodies) {
                    digest.flag(body.bound());
                }
                for (const auto& body : variantBodies) {
                    digest.flag(body.bound());
                }
                for (const auto& body : deathStatues) {
                    digest.flag(body.bound());
                }
                digest.values(arrow.bound(), bomb.bound(), fireball.bound());
                (void)shadows;
                (void)unseen; // the built-in IT tree is identified through trees
                digest.resource(archive.loaded() ? &textures : nullptr);
                digest.fields<3>(savedClocks ? (*savedClocks)[index] : textures.clock());
            }
        } else if constexpr (std::is_same_v<T, std::array<Enemy, kMost>>) {
            digest.wide(field.size());
            for (const auto& enemy : field) {
                // Inactive slots retain animation, lifetime and AI values too.
                digest.value(static_cast<const BodyState&>(enemy));
                digest.value(enemy.animator);
            }
        } else if constexpr (std::is_same_v<T, std::optional<ViewSet>>) {
            digest.flag(field.has_value());
            if (field) {
                digest.value(*field);
            }
        } else {
            digest.value(field);
        }
    };
    visitNativeFields<27>(state, [&](const auto&... fields) { (append(fields), ...); });
}

bool Enemies::appendNativeDigest(NativeStateDigest& digest) const {
    if (m_device == nullptr) {
        digest.invalidate();
        return false;
    }
    digest.string("Enemies.native.1");
    digest.resource(m_collision);
    digest.resource(m_hazards);
    appendSimulationDigest(digest, m_simulation);
    return digest.good();
}

bool Enemies::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("Enemies.native.1");
    digest.resource(saved.m_collision);
    digest.resource(saved.m_hazards);
    appendSimulationDigest(digest, saved.m_simulation,
                           std::span<const TextureAnimator::Clock>{saved.m_textureClocks});
    return digest.good();
}

bool Enemies::restoreLocal(const LocalSnapshot& source) {
    if (!canRestoreLocal(source)) {
        return false;
    }
    m_simulation = source.m_simulation;
    for (usize index = 0; index < m_simulation.stocks.size(); ++index) {
        m_simulation.stocks[index]->textures.restoreClock(source.m_textureClocks[index]);
    }
    // Legacy diagnostic preparations must not commit across a local rewind.
    ++m_restoreGeneration;
    return true;
}

Enemies::~Enemies() {
    close();
}

void Enemies::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                   const WorldCollision* collision, s32 most, const EnemyScales& scales, u32 seed) {
    close();
    m_device = &device;
    m_root = unpackedRoot;
    m_collision = collision;
    m_simulation.most = std::clamp(most, 1, kMost);
    m_levelCap = m_simulation.most;
    m_simulation.scales = scales;
    m_simulation.random.seed(seed);
}

void Enemies::narrowPool() {
    if (m_simulation.most <= m_levelCap) {
        return;
    }
    // The slots past the cap leave play from the top down, each as the body in it goes.
    s32 most = m_levelCap;
    for (s32 i = m_levelCap; i < m_simulation.most; ++i) {
        if (m_simulation.enemies[static_cast<usize>(i)].state != State::Inactive) {
            most = i + 1;
        }
    }
    m_simulation.most = most;
}

void Enemies::close() {
    ++m_resourceEpoch;
    ++m_restoreGeneration;
    for (Enemy& enemy : m_simulation.enemies) {
        enemy = Enemy{};
    }
    for (auto& stock : m_stockCache) {
        for (TreeModel& body : stock->bodies) {
            body.clear();
        }
        for (TreeModel& body : stock->variantBodies) {
            body.clear();
        }
        stock->arrow.clear();
        stock->bomb.clear();
        for (TreeModel& statue : stock->deathStatues) {
            statue.clear();
        }
        stock->textures.clear();
        stock->archive.release();
    }
    m_simulation.stocks.clear();
    m_stockCache.clear();
    m_archiveLoader = {};
    m_simulation.blows.clear();
    m_simulation.losses.clear();
    m_simulation.bursts.clear();
    m_simulation.births.clear();
    m_simulation.cues.clear();
    m_simulation.feedback.clear();
    m_simulation.deathEvents.clear();
    m_simulation.deathShots.clear();
    m_simulation.tagged.clear();
    m_simulation.generatorEvents.clear();
    m_simulation.combatants.clear();
    m_simulation.contactGridOrigin = Vec2{0};
    m_simulation.contactGridInverseWidth = 0;
    m_device = nullptr;
    m_collision = nullptr;
    m_hazards = nullptr;
    m_simulation.frame = 0;
    m_simulation.pendingTicks = 0;
    m_simulation.pendingSeconds = 0;
}

Enemies::Stock* Enemies::stockOf(s32 kind) {
    for (auto& stock : m_simulation.stocks) {
        if (stock->kind == kind) {
            return stock;
        }
    }
    return nullptr;
}

void Enemies::setCombatantBodies(std::span<const MissileTarget> bodies) {
    m_simulation.combatants.clear();
    for (const auto& body : bodies) {
        if (body.radius > 0 && body.height > 0) {
            m_simulation.combatants.push_back({body.id, body.base + Vec3{0, 0.5f * body.height, 0},
                                               body.radius, 0.5f * body.height});
        }
    }
}

void Enemies::setContactGridBounds(const Vec3& minimum, const Vec3& maximum) {
    m_simulation.contactGridOrigin = Vec2{minimum.x, minimum.z};
    // InitDynGrid (80043A04): 803466A4 is 1/64, 803466A8 is double 1.
    const f32 cellWidth =
        std::max(maximum.x - minimum.x, maximum.z - minimum.z) / kContactGridDimension;
    m_simulation.contactGridInverseWidth = cellWidth > 0 ? static_cast<f32>(1.0 / cellWidth) : 0;
}

const Enemies::Stock* Enemies::stockOf(s32 kind) const {
    for (const auto& stock : m_simulation.stocks) {
        if (stock->kind == kind) {
            return stock;
        }
    }
    return nullptr;
}

bool Enemies::setArchiveLoader(ArchiveLoader loader) {
    if (!m_stockCache.empty() || !m_simulation.stocks.empty())
        return false;
    m_archiveLoader = std::move(loader);
    return true;
}

bool Enemies::loadKind(s32 kind) {
    if (stockOf(kind) != nullptr) {
        return true;
    }
    if (m_device == nullptr || kind < 0 || kind >= kEnemyKindCount) {
        return false;
    }
    for (const auto& cached : m_stockCache) {
        if (cached->kind == kind) {
            cached->textures.restoreClock({});
            m_simulation.stocks.push_back(cached.get());
            return true;
        }
    }
    const EnemyKind& info = enemyKind(kind);
    auto stock = std::make_unique<Stock>();
    stock->kind = kind;
    const std::filesystem::path directory = m_root / "MONSTERS" / std::string(info.name);
    bool loaded = false;
    if (m_archiveLoader) {
        auto imported = m_archiveLoader(kind);
        if (!imported)
            return false;
        stock->archive = std::move(*imported);
        loaded = stock->archive.loaded();
    } else {
        const bool bodiless = kind == kItKind && !std::filesystem::exists(directory);
        loaded = !bodiless && stock->archive.load(directory);
    }
    if (!loaded) {
        if (m_archiveLoader)
            return false;
        if (kind != kItKind) {
            return false;
        }
        // No archive holds IT's trees ("IT1".."IT3"): it goes about with no body to see.
        stock->unseen.name = info.prefix;
        TreeSequenceInfo stance;
        stance.name = "READY";
        stance.frames = 1;
        stance.frameRate = 30;
        stance.repeats = true;
        stock->unseen.sequences.push_back(stance);
        stock->trees.fill(&stock->unseen);
        m_simulation.stocks.push_back(stock.get());
        m_stockCache.push_back(std::move(stock));
        return true;
    }
    stock->textures.bind(stock->archive.trees.textureAnimations(), stock->archive.textures,
                         *m_device);
    for (s32 tier = 1; tier <= 3; ++tier) {
        const auto tree = stock->archive.trees.find(std::format("{}{}", info.prefix, tier));
        if (!tree.has_value()) {
            continue;
        }
        const TreeInfo& treeInfo = stock->archive.trees.tree(*tree);
        if (stock->bodies[static_cast<usize>(tier - 1)].bind(treeInfo, stock->archive.models,
                                                             stock->archive.textures, *m_device)) {
            stock->trees[static_cast<usize>(tier - 1)] = &treeInfo;
        }
    }
    // The archer, bomber and suicide ("ZOMA", "ZOMB", "ZOMS") and what the first two throw.
    const std::array<const char*, 3> suffixes{"A", "B", "S"};
    for (usize v = 0; v < suffixes.size(); ++v) {
        const auto tree = stock->archive.trees.find(std::format("{}{}", info.prefix, suffixes[v]));
        if (!tree.has_value()) {
            continue;
        }
        const TreeInfo& treeInfo = stock->archive.trees.tree(*tree);
        if (stock->variantBodies[v].bind(treeInfo, stock->archive.models, stock->archive.textures,
                                         *m_device)) {
            stock->variantTrees[v] = &treeInfo;
        }
    }
    if (const auto arrow = stock->archive.trees.find(std::format("{}_ARROW", info.prefix));
        arrow.has_value()) {
        stock->arrow.bind(stock->archive.trees.tree(*arrow), stock->archive.models,
                          stock->archive.textures, *m_device);
    }
    if (const auto bomb = stock->archive.trees.find(std::format("{}_BOMB", info.prefix));
        bomb.has_value()) {
        stock->bomb.bind(stock->archive.trees.tree(*bomb), stock->archive.models,
                         stock->archive.textures, *m_device);
    }
    if (const auto ball = stock->archive.trees.find(std::format("{}_FBALL", info.prefix));
        ball.has_value()) {
        stock->fireball.bind(stock->archive.trees.tree(*ball), stock->archive.models,
                             stock->archive.textures, *m_device);
    }
    // Every kind lies a shadow of its tier under it but these (InitEnemyGeo).
    if (kind != kScorpionKind && kind != kAcidKind && kind != kDeathKind && kind != kItKind) {
        for (usize i = 0; i < stock->shadows.size(); ++i) {
            stock->shadows[i].bind(*m_device, stock->archive, std::format("SHADOW{}L1", i + 1));
        }
    }
    if (kind == kDeathKind) {
        for (usize i = 0; i < stock->deathStatues.size(); ++i) {
            if (const auto tree = stock->archive.trees.find(std::format("DEATHSTATUE{}", i + 1))) {
                stock->deathStatueTrees[i] = &stock->archive.trees.tree(*tree);
                stock->deathStatues[i].bind(stock->archive.trees.tree(*tree), stock->archive.models,
                                            stock->archive.textures, *m_device);
            }
        }
    }
    m_simulation.stocks.push_back(stock.get());
    m_stockCache.push_back(std::move(stock));
    return true;
}

bool Enemies::kindLoaded(s32 kind) const {
    return stockOf(kind) != nullptr;
}

const ItemArchive* Enemies::archiveOf(s32 kind) const {
    const Stock* stock = stockOf(kind);
    return stock != nullptr ? &stock->archive : nullptr;
}

ItemArchive* Enemies::archive(s32 kind) {
    Stock* stock = stockOf(kind);
    return stock != nullptr ? &stock->archive : nullptr;
}

bool Enemies::registerNativeResources(NativeStateResources& resources,
                                      std::string_view stablePrefix) const {
    if (stablePrefix.empty()) {
        return false;
    }
    // Restoring a native branch removes its newly admitted kinds from stocks,
    // but cached boss figures and retained history can still borrow their art.
    // Register the owning cache without admitting those kinds to gameplay again.
    for (const auto& stock : m_stockCache) {
        if (!stock->archive.loaded()) {
            // IT deliberately has no archive on the disc, but its READY body
            // and retained sequence/pose bindings are still actual native state.
            if (!resources.registerBuiltinTree(
                    stock->unseen, std::format("{}/{}/body", stablePrefix, stock->kind))) {
                return false;
            }
            continue;
        }
        if (!resources.registerArchive(stock->archive,
                                       std::format("{}/{}", stablePrefix, stock->kind))) {
            return false;
        }
        if (!resources.registerTextureAnimator(stock->textures, stock->archive, "swarm/textures")) {
            return false;
        }
        const std::array models{&stock->arrow, &stock->bomb, &stock->fireball};
        for (usize slot = 0; slot < models.size(); ++slot) {
            if (models[slot]->bound() && !resources.registerModel(*models[slot], stock->archive,
                                                                  std::format("shot/{}", slot))) {
                return false;
            }
        }
    }
    return resources.good();
}

const TreeModel* Enemies::projectileModel(s32 kind, s32 slot) const {
    const auto* stock = stockOf(kind);
    if (stock == nullptr || slot < 0 || slot > 2) {
        return nullptr;
    }
    const std::array models{&stock->arrow, &stock->bomb, &stock->fireball};
    const auto* model = models[static_cast<usize>(slot)];
    return model->bound() ? model : nullptr;
}

const TreeInfo* Enemies::treeOf(s32 kind, s32 tier) const {
    const Stock* stock = stockOf(kind);
    if (stock == nullptr) {
        return nullptr;
    }
    // A tier the archive lacks wears the nearest it has.
    for (s32 t = std::clamp(tier, 1, 3); t >= 1; --t) {
        if (const TreeInfo* tree = stock->trees[static_cast<usize>(t - 1)]; tree != nullptr) {
            return tree;
        }
    }
    for (const TreeInfo* tree : stock->trees) {
        if (tree != nullptr) {
            return tree;
        }
    }
    return nullptr;
}

f32 Enemies::paceOf(s32 kind) const {
    return enemyKind(kind).pace * m_simulation.scales.speed * tuning().speedScale;
}

SwarmTuning Enemies::tuning() const {
    return m_rules.swarm != nullptr ? m_rules.swarm->tuning() : SwarmTuning{};
}

Vec3 Enemies::bodyCentre(const Enemy& enemy) {
    return enemy.position + Vec3{0.0f, enemy.reach, 0.0f};
}

f32 Enemies::playerDistance(const Enemy& enemy, const EnemyView& player) {
    // calc_enemy_to_player_distance measures all three axes between collision centres,
    // not the horizontal separation of the feet. Players on another floor count as farther.
    const Vec3 from = enemy.position + Vec3{0, enemyKind(enemy.kind).collisionHeight, 0};
    const Vec3 to = player.decoy.value_or(
        player.position + Vec3{0, player.collisionHeight.value_or(0.5f * player.height), 0});
    return glm::distance(from, to);
}

const EnemyView* Enemies::viewOf(std::span<const EnemyView> players, s32 player) {
    for (const EnemyView& view : players) {
        if (view.player == player) {
            return &view;
        }
    }
    return nullptr;
}

// ---- spawning ---------------------------------------------------------------------------

std::optional<s32> Enemies::takeSlot(const EnemySpawn& spawn) {
    // Prefer a free slot, otherwise the greatest recycling score. Dying/sleeping
    // enemies and authored sentries get reduced scores; other unseen enemies
    // get the offscreen bonus.
    for (s32 i = 0; i < m_simulation.most; ++i) {
        if (m_simulation.enemies[static_cast<usize>(i)].state == State::Inactive) {
            return i;
        }
    }
    s32 best = -1;
    bool bestVisible = false;
    f32 bestCost = -1.0f;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (enemy.kind == kItKind) {
            continue;
        }
        f32 cost = enemy.targetDistance;
        const bool seen = enemy.onScreen;
        if (enemy.state == State::Dying || enemy.state == State::Asleep ||
            enemy.birth != Birth::Brood) {
            cost *= 0.01f;
        } else if (!seen) {
            cost += kFarRecycleCost;
        }
        if (cost > bestCost) {
            bestCost = cost;
            best = i;
            bestVisible = seen;
        }
    }
    if (best < 0) {
        return std::nullopt;
    }
    // Visibility importance, not combat strength, controls replacement.
    if (spawn.kind < kSwarmKindCount && static_cast<s32>(spawn.priority) < (bestVisible ? 1 : 0)) {
        return std::nullopt;
    }
    Enemy& taken = m_simulation.enemies[static_cast<usize>(best)];
    detachGenerator(taken);
    taken = Enemy{};
    return best;
}

bool Enemies::birthPathClear(const Enemy& enemy, const Vec3& from, const Vec3& to,
                             std::span<const EnemyView> players, s32 self) const {
    // check_enemy_pos (8004F9AC) passes the entire birth segment to the same
    // directional body queries used by movement. A free endpoint is not enough,
    // and an existing overlap may separate instead of sealing every exit.
    for (const EnemyView& view : players) {
        // check_enemy_pos's player sweep only considers active players. A fallen
        // party member's last position must not obstruct the generator's exits.
        const Vec3 centre =
            view.position + Vec3{0, view.collisionHeight.value_or(0.5f * view.height), 0};
        const f32 radius = view.collisionRadius.value_or(view.radius);
        if (!view.hidden && movementTouchesBody(from, to, centre, radius + enemy.radius,
                                                0.5f * view.height + enemy.reach)) {
            return false;
        }
    }
    const f32 radius = 0.5f * enemy.radius;
    if (combatantContact(m_simulation.combatants, from, to, radius) != nullptr) {
        return false;
    }
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& other = m_simulation.enemies[static_cast<usize>(i)];
        if (i == self || other.state == State::Inactive || other.state == State::Dying) {
            continue;
        }
        if (movementTouchesBody(from, to, bodyCentre(other), radius + other.radius,
                                enemy.reach + other.reach)) {
            return false;
        }
    }
    return true;
}

void Enemies::initialise(Enemy& enemy, const EnemySpawn& spawn, const EnemyKind& kind) {
    enemy.state = spawn.asleep ? State::Asleep : State::Active;
    enemy.veil = 0.0f;
    enemy.veilClock = 0;
    enemy.kind = spawn.kind;
    // A strength past the tiers is a variant: the archer and bomber of the second tier, the
    // suicide of the first, each with its own way unless the placement gives one.
    enemy.variant = spawn.tier >= kArcherStrength ? spawn.tier : 0;
    if (enemy.variant == 0) {
        enemy.tier = std::clamp(spawn.tier, 1, 3);
    } else {
        enemy.tier = enemy.variant == kSuicideStrength ? 1 : 2;
    }
    const bool mirrored = smallKind(spawn.kind) && (m_simulation.random() & 1U) != 0;
    enemy.algorithm = resolvedWayOf(spawn.kind, spawn.tier, spawn.algorithm, mirrored);
    if (spawn.patrolBirth) {
        enemy.algorithm = kPatrolWay; // generate_single overrides the kind's remapping.
    }
    initializeEnemyMind(enemy.mind, enemy.algorithm, m_simulation.random());
    enemy.generator = spawn.generator;
    enemy.bred = spawn.generator >= 0 && !spawn.patrolBirth;
    enemy.birth = spawn.placed ? Birth::Placement : Birth::Brood;
    if (spawn.patrolBirth) {
        enemy.birth = Birth::Patrol;
    }
    enemy.radius = kind.radius;
    enemy.height = kind.height;
    enemy.reach = 0.5f * kind.height;
    // A placement sees as far as it says, else thirty, at the level's scale (SetItem,
    // init_enemy_vars); what a generator breeds always thirty.
    enemy.sight = (spawn.sight > 0.0f ? spawn.sight : kBaseSight) * m_simulation.scales.sight;
    // A placement of ordinary strength stands still thirty ticks on appearing (SetItem, items.c
    // 5564; do_enemy_move zeroes its step): not the variants, nor one that sleeps first.
    enemy.stunTicks =
        spawn.placed && !spawn.asleep && spawn.tier < kArcherStrength ? kPlacedStun : 0;
    // Death alone is not scaled by the level nor by the mechanics' tuning; the swarm has a
    // third of its kind's health a tier, and hits softer as it loses its kind's full share.
    const f32 scale =
        spawn.kind != kDeathKind ? m_simulation.scales.health * tuning().healthScale : 1.0f;
    enemy.fullHealth = kind.health * scale;
    enemy.health =
        spawn.kind < kSwarmKindCount ? kind.healthAtTier(enemy.tier) * scale : enemy.fullHealth;
    enemy.target = -1;
    enemy.targetBefore = -1;
    enemy.targetDistance = 100000.0f;
    enemy.weightedDistance = 100000.0f;
    enemy.contact = -1;
    enemy.attackIndex = -1;
    if (spawn.kind == kDeathKind) {
        enemy.sight = 100000.0f;
        enemy.endurance = spawn.placed ? 1 : 0;
    }
}

std::optional<s32> Enemies::spawn(const EnemySpawn& spawn, std::span<const EnemyView> players,
                                  std::span<const Obstacle> obstacles) {
    if (m_simulation.nextInstance == std::numeric_limits<u64>::max()) {
        return std::nullopt; // Never recycle a network identity after exhaustion.
    }
    const Stock* stock = stockOf(spawn.kind);
    if (stock == nullptr) {
        return std::nullopt;
    }
    const auto slot = takeSlot(spawn);
    if (!slot.has_value()) {
        return std::nullopt;
    }
    Enemy& enemy = m_simulation.enemies[static_cast<usize>(*slot)];
    detachGenerator(enemy);
    const EnemyKind& kind = enemyKind(spawn.kind);
    initialise(enemy, spawn, kind);
    // What the mechanics give the birth beyond its kind, drawing on the pool's own stream
    // only when they must.
    const BodyTraits traits =
        m_rules.births != nullptr
            ? m_rules.births->traitsFor({.kind = spawn.kind, .frenzied = spawn.frenzied},
                                        m_simulation.random)
            : BodyTraits{};
    enemy.affinity = traits.affinity;
    if (traits != BodyTraits{}) {
        enemy.fullHealth *= traits.healthScale;
        enemy.health *= traits.healthScale;
        enemy.radius *= traits.sizeScale;
        enemy.height *= traits.sizeScale;
        enemy.reach *= traits.sizeScale;
    }
    const TreeInfo* tree = treeOf(spawn.kind, enemy.tier);
    // A strength past the known variants (retail's unshipped F) keeps its tier's tree.
    if (enemy.variant != 0) {
        const auto v = static_cast<usize>(enemy.variant - kArcherStrength);
        if (v < stock->variantTrees.size() && stock->variantTrees[v] != nullptr) {
            tree = stock->variantTrees[v];
        }
    }
    const bool walksIn = enemy.variant == 0 && (spawn.kind == 1 || spawn.kind == kGruntKind ||
                                                spawn.kind == 10 || spawn.kind == 7);
    if (tree == nullptr || !enemy.animator.bind(*tree, walksIn)) {
        enemy = Enemy{};
        return std::nullopt;
    }
    enemy.animator.setThrowInterval(spawn.throwInterval * m_simulation.scales.missileRate);
    const f32 facing = portable::atan2(spawn.direction.x, spawn.direction.z);
    const auto settle = [this](Vec3 at, Vec3& out) {
        if (m_collision == nullptr) {
            out = at;
            return true;
        }
        const auto floor = m_collision->floorAt(at, kSpawnDrop, kSpawnDrop);
        if (!floor.has_value()) {
            return false;
        }
        at.y = floor->y;
        out = at;
        return true;
    };
    bool placed = false;
    Vec3 where = spawn.position;
    f32 yaw = facing;
    if (spawn.placed) {
        placed = settle(spawn.position, where);
    } else {
        const f32 out = spawn.kind == kWormKind ? 0.0f : spawn.clearance + enemy.radius;
        const Vec3 v = spawn.direction * out;
        u32 mask = spawn.allBirthDirections ? 0 : octantMaskOf(spawn.kind);
        // generate_enemy (GC 0x8004f4b4) gives dogs front/back exits;
        // leaders use only the generator's forward bearing. Special mixed
        // broods choose their directions from the original -2/-3 kind instead.
        s32 directions = 8;
        if (!spawn.allBirthDirections && spawn.kind == kDogKind) {
            directions = 2;
        }
        if (enemy.algorithm == kLeaderWay) {
            directions = 1;
        }
        const auto start = static_cast<s32>(m_simulation.random() % static_cast<u32>(directions));
        s32 d = start;
        do {
            if ((mask & (1U << static_cast<u32>(d))) == 0) {
                f32 yawOffset = 0.0f;
                const Vec3 offset = octant(v, d, yawOffset);
                Vec3 at = spawn.position + offset;
                const Vec3 birthFrom = spawn.position + Vec3{0, kind.collisionHeight, 0};
                const Vec3 birthTo = birthFrom + offset;
                bool clear = true;
                if (m_collision != nullptr) {
                    // check_enemy_pos (0x8004F9AC) checks the path out of the generator
                    // BEFORE choosing a floor, using EnemyWallCollide's sphere at
                    // coll_offset, not the movement solver's two horizontal samples.
                    clear = !m_collision->wallBetween(birthFrom, birthTo, enemy.radius);
                    if (clear) {
                        // FloorCollide(pos, .1, 4, -10) also starts at that centre.
                        // Probing +/-6 from the feet can miss a raised deck and choose
                        // the ground below it, or admit an excessively deep drop.
                        const auto floor = m_collision->floorAt(birthTo, 4, 10, 0.1f);
                        clear = floor && std::abs(floor->y - birthFrom.y) <= kSpawnDrop;
                        if (clear) {
                            at.y = floor->y;
                        }
                    }
                }
                clear = clear && std::ranges::none_of(obstacles, [&](const Obstacle& box) {
                            // check_enemy_pos passes half the enemy radius to
                            // fn_8005EFAC's item query. A second full-radius endpoint
                            // test rejects births beside items that retail permits.
                            return itemBlocksEnemy(box, spawn.position, at, 0.5f * enemy.radius);
                        });
                if (!clear) {
                    mask |= 1U << static_cast<u32>(d);
                } else if (birthPathClear(enemy, birthFrom, birthTo, players, *slot)) {
                    where = at;
                    yaw = wrapAngle(facing + yawOffset);
                    placed = true;
                    break;
                }
            }
            d = (d + 1) % directions;
        } while (d != start);
    }
    if (!placed) {
        enemy = Enemy{};
        return std::nullopt;
    }
    enemy.position = where;
    enemy.birthPosition = where;
    rememberFloor(enemy);
    enemy.yaw = yaw;
    enemy.mind.heading = yaw;
    enemy.mind.headingBefore = yaw;
    if (enemy.algorithm == kZigZagWay && spawn.zigZagSide != 0) {
        // do_items' algorithm-14 birth setup follows generate_enemy: keep the body facing
        // out of its generator, but offset its intended path by +/-45 degrees. A zero
        // side cannot alternate when move_logic14 negates it and makes it circle instead.
        enemy.mind.zigZag.side = spawn.zigZagSide;
        enemy.mind.heading = wrapAngle(yaw + static_cast<f32>(spawn.zigZagSide) * kPi / 4);
        enemy.mind.headingBefore = enemy.mind.heading;
    }
    if (enemy.generator >= 0) {
        m_simulation.generatorEvents.push_back({enemy.generator, EnemyGeneratorEvent::Kind::Born});
    }
    if (enemy.variant == 0) {
        m_simulation.births.push_back({spawn.position, spawn.kind, enemy.tier});
    }
    enemy.instance = m_simulation.nextInstance++;
    return slot;
}

void Enemies::rememberFloor(Enemy& enemy) const {
    enemy.floor.reset();
    if (m_collision == nullptr) {
        return;
    }
    // Retain only a surface actually supporting the feet, not a floor below a jump.
    const auto floor = m_collision->floorAt(enemy.position, 0.1f, 0.1f);
    if (floor) {
        if (const auto placement = m_collision->objectTransform(floor->object)) {
            enemy.floor = Enemy::Floor{floor->object,
                                       Vec3{glm::inverse(*placement) * Vec4{enemy.position, 1}}};
        }
    }
}

void Enemies::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (Enemy& enemy : m_simulation.enemies) {
        if (!enemy.floor || enemy.state == State::Inactive) {
            continue;
        }
        if (enemy.kind == kDeathKind && enemy.state == State::Dying) {
            enemy.floor.reset();
            continue;
        }
        const auto placement = m_collision->objectTransform(enemy.floor->object);
        if (!placement || !m_collision->solid(enemy.floor->object)) {
            enemy.floor.reset();
            continue;
        }
        const Vec3 carried = Vec3{*placement * Vec4{enemy.floor->local, 1}};
        // Floor synchronization can occur while gameplay is held. Carry both
        // endpoints so an old walk snapshot cannot pull a rider off its platform.
        enemy.presentationPosition += carried - enemy.position;
        enemy.position = carried;
    }
}

void Enemies::wake(s32 id) {
    if (id >= 0 && id < m_simulation.most &&
        m_simulation.enemies[static_cast<usize>(id)].state == State::Asleep) {
        m_simulation.enemies[static_cast<usize>(id)].state = State::Active;
    }
}

void Enemies::generatorGone(s32 generator) {
    for (Enemy& enemy : m_simulation.enemies) {
        if (enemy.generator == generator) {
            enemy.generator = -1;
        }
    }
}

// ---- the tick ----------------------------------------------------------------------------

void Enemies::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                     std::span<const Obstacle> obstacles, EnemyMissiles* missiles,
                     f32 missileSpeedScale, bool timeStopped) {
    for (auto& stock : m_simulation.stocks) {
        stock->textures.advance(timeStopped ? 0.0f : seconds);
    }
    syncFloors();
    if (timeStopped) {
        for (Enemy& enemy : m_simulation.enemies) {
            if (enemy.state == State::Active) {
                capturePresentation(enemy);
            }
        }
    }
    if (ticks > 0) {
        // A body with no death skin has no animation left to simulate. Retire it
        // on the first death update, not after waiting for the next AI frame.
        // Death itself instead rises and fades on its separate timed path.
        for (s32 i = 0; i < m_simulation.most; ++i) {
            Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
            if (enemy.state == State::Dying && enemy.kind != kDeathKind &&
                (enemy.deathSkinFrames <= 0 || enemy.algorithm == kSuicideWay)) {
                if (enemy.kind == kGarmBroodKind && enemy.algorithm != kSuicideWay) {
                    aimDeathShot(enemy, i, players);
                }
                die(enemy);
            }
        }
    }
    // Keep the native two-tick mind AND collision step together. Splitting just
    // movement changes the footprint support selected on narrow stair treads.
    const f32 secondsPerTick = ticks > 0 ? seconds / static_cast<f32>(ticks) : 0;
    for (s32 tick = 0; tick < ticks; ++tick) {
        ++m_simulation.pendingTicks;
        m_simulation.pendingSeconds += secondsPerTick;
        if (m_simulation.pendingTicks == 2) {
            step(m_simulation.pendingSeconds, players, obstacles, missiles, missileSpeedScale,
                 timeStopped);
            m_simulation.pendingTicks = 0;
            m_simulation.pendingSeconds = 0;
        }
    }
}

void Enemies::step(f32 seconds, std::span<const EnemyView> players,
                   std::span<const Obstacle> obstacles, EnemyMissiles* missiles,
                   f32 missileSpeedScale, bool timeStopped) {
    constexpr s32 kMotionTicks = 2;
    const f32 kMotionSeconds = seconds;
    for (Enemy& enemy : m_simulation.enemies) {
        capturePresentation(enemy);
    }
    ++m_simulation.frame;
    // What is on screen, by twice its radius and fifteen more (do_enemies' visactive), and how
    // many are in view by twice their radius alone (its visible count).
    m_simulation.inView = 0;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        enemy.onScreen =
            enemy.state == State::Inactive || !m_simulation.view.has_value() ||
            m_simulation.view->sees(bodyCentre(enemy), (2.0f * enemy.radius) + kOnScreenMargin);
        if (enemy.state != State::Inactive &&
            (!m_simulation.view.has_value() ||
             m_simulation.view->sees(bodyCentre(enemy), 2.0f * enemy.radius))) {
            ++m_simulation.inView;
        }
    }
    // The first suicide bomber on screen running at the party is one the rest keep away from.
    m_simulation.bomber = -1;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (enemy.state == State::Active && !enemy.killed && enemy.algorithm == kSuicideWay &&
            enemy.onScreen && enemy.animator.action() == EnemyAction::Run) {
            m_simulation.bomber = i;
            break;
        }
    }
    std::array<f32, 4> crowding{};
    for (s32 i = 0; i < m_simulation.most; ++i) {
        Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (enemy.state == State::Inactive || enemy.state == State::Asleep) {
            continue;
        }
        // Detonation replaces the bomber's whole body with its separate fragment effect.
        if (enemy.killed && enemy.algorithm == kSuicideWay) {
            die(enemy);
            continue;
        }
        enemy.flashSeconds = std::max(0.0f, enemy.flashSeconds - kMotionSeconds);
        enemy.burnGap = std::max(0.0f, enemy.burnGap - kMotionSeconds);
        if (enemy.state == State::Dying) {
            enemy.deathSeconds += kMotionSeconds;
            if (enemy.kind == kDeathKind) {
                enemy.position.y += DeathRules::kRiseSpeed * kMotionSeconds;
                if (enemy.deathSeconds >= DeathRules::kFadeSeconds) {
                    die(enemy);
                }
                continue;
            }
            react(enemy);
            enemy.animator.request(EnemyAction::Dying);
            enemy.yaw = turnToward(enemy, enemy.mind.heading, kMotionTicks);
            move(enemy, i, kMotionTicks, kMotionSeconds, Vec3{0.0f, 0.0f, 0.0f}, players,
                 obstacles);
            enemy.animator.updateCaptured(kMotionTicks, kMotionSeconds, false);
            // No skin sequence (the small swarm), or a completed one, retires the body
            // even if the fallback animation is still playing: do_enemies, DYING.
            const bool dissolved =
                enemy.deathSkinFrames <= 0 ||
                enemy.deathSeconds * kDeathSkinRate >= static_cast<f32>(enemy.deathSkinFrames);
            if (dissolved || enemy.animator.dead() || !enemy.animator.reacting()) {
                if (enemy.kind == kGarmBroodKind) {
                    aimDeathShot(enemy, i, players);
                }
                die(enemy);
            }
            continue;
        }
        if (timeStopped) {
            react(enemy);
            // Keep contact bookkeeping and incoming damage, without advancing
            // attacks, AI, knockback or the animation's one-shot release cues.
            move(enemy, i, 0, 0, Vec3{0}, players, obstacles);
            continue;
        }
        chooseTarget(enemy, i, players, crowding);
        // A caster's swing, or a lunger's power swing, that touches nobody casts its missile
        // instead (fn_8004DF58).
        if (castsNow(enemy) && enemy.contact < 0) {
            if (missiles != nullptr) {
                shoot(enemy, i, players, *missiles, missileSpeedScale, obstacles);
            }
            enemy.attackIndex = -1;
        } else {
            resolveBlows(enemy, i, players);
        }
        if (enemy.kind != kDeathKind) {
            react(enemy);
        }
        if (enemy.state == State::Dying) {
            continue;
        }
        // Out of view with its player out of its sight, it waits where it stands, neither
        // thinking nor moving nor animating, and only its knock-back dies away (fn_8004D958).
        if (!enemy.onScreen && enemy.targetDistance > enemy.sight) {
            decayPush(enemy, kMotionSeconds);
            continue;
        }
        // The tick's default action (do_enemies' daction): the stance, or for the scorpion
        // the walk, which then also refuses the stance and, as loud, a run.
        enemy.animator.request(enemyKind(enemy.kind).idle);
        const bool held =
            enemy.kind == kDeathKind && std::ranges::any_of(players, [&](const EnemyView& player) {
                constexpr f32 kContactTolerance = 0.001f;
                return player.heldDeath == i && !player.hidden && !player.captured &&
                       player.antiDeath &&
                       flatDistance(player.position, enemy.position) <=
                           player.radius + enemy.radius + kContactTolerance &&
                       enemy.position.y <= player.position.y + player.height &&
                       enemy.position.y + enemyKind(enemy.kind).height >= player.position.y;
            });
        if (held) {
            // A live halo contact owns this frame. Do not keep a sticky stun or
            // change the AI route: releasing the last holder resumes it immediately.
            enemy.contact = -1;
            enemy.draining = false;
            enemy.stunTicks = std::max(0, enemy.stunTicks - kMotionTicks);
            enemy.animator.stopWalking();
            enemy.animator.updateCaptured(kMotionTicks, kMotionSeconds, false);
            decayPush(enemy, kMotionSeconds);
            continue;
        }
        think(enemy, i, kMotionTicks, players, obstacles);
        if (enemy.kind == kDeathKind) {
            drain(enemy, i, 2, players);
        }
        if (enemy.expired) {
            die(enemy);
            continue;
        }
        // Nothing has been asked of a body just born: it walks off from its entrance.
        if (enemy.animator.entering()) {
            enemy.animator.request(EnemyAction::Walk);
        }
        // A swarm the mechanics hurry swings quicker, so its blow lands sooner.
        const f32 swingSeconds =
            enemy.animator.swinging() ? kMotionSeconds * tuning().attackPace : kMotionSeconds;
        enemy.animator.updateCaptured(kMotionTicks, swingSeconds, enemy.contact >= 0);
        if (enemy.kind == kVeilingKind) {
            veil(enemy, kMotionTicks);
        }
        if (enemy.animator.threw() && missiles != nullptr) {
            shoot(enemy, i, players, *missiles, missileSpeedScale, obstacles);
        }
        decayPush(enemy, kMotionSeconds);
        touchHazards(enemy, i);
    }
}

void Enemies::veil(Enemy& enemy, s32 ticks) {
    constexpr f32 kVeilStep = 16.0f; ///< a tick
    constexpr s32 kVeilLeast = 60;   ///< ticks seen or unseen, and as many again at random
    const EnemyAction action = enemy.animator.action();
    const auto hold = [this] {
        return kVeilLeast + static_cast<s32>(m_simulation.random() % static_cast<u32>(kVeilLeast));
    };
    const f32 step = kVeilStep * static_cast<f32>(ticks);
    if (action != EnemyAction::Ready && action != EnemyAction::Walk && action != EnemyAction::Run) {
        enemy.veil = std::max(0.0f, enemy.veil - step);
        enemy.veilClock = 0;
        return;
    }
    if (enemy.veilClock == 0) {
        enemy.veil = std::max(0.0f, enemy.veil - step);
        enemy.veilClock = hold();
    } else if (enemy.veilClock > 0) {
        enemy.veil = std::max(0.0f, enemy.veil - step);
        enemy.veilClock -= ticks;
        if (enemy.veilClock < 0) {
            enemy.veilClock = -hold();
        }
    } else {
        enemy.veil = std::min(kVeiled, enemy.veil + step);
        enemy.veilClock += ticks;
        if (enemy.veilClock > 0) {
            enemy.veilClock = 0;
        }
    }
}

/** Knock-back dies away, and what was thrown up comes down. */
void Enemies::decayPush(Enemy& enemy, f32 seconds) {
    enemy.push *= portable::pow(kPushDecay, seconds * kPushFrameRate);
    if (std::abs(enemy.push.x) < kPushFloor) {
        enemy.push.x = 0.0f;
    }
    if (std::abs(enemy.push.z) < kPushFloor) {
        enemy.push.z = 0.0f;
    }
    enemy.push.y = std::max(enemy.push.y - kGravity * seconds, 0.0f);
}

/** EnemyWorldDamage, with the requested quarter-second fire repeat gate. */
void Enemies::touchHazards(Enemy& enemy, s32 slot) {
    if (m_hazards == nullptr || m_collision == nullptr || enemy.state != State::Active ||
        enemy.kind == kGarmBroodKind) {
        return;
    }
    const auto touch =
        m_hazards->touching(*m_collision, enemy.position, enemy.radius, enemy.height);
    if (!touch) {
        return;
    }
    const auto harm = HazardSurfaces::enemyHarmOf(m_hazards->flagsOf(touch->object));
    if (!harm) {
        return;
    }
    if (HazardSurfaces::burning(m_hazards->flagsOf(touch->object))) {
        if (enemy.burnGap > 0) {
            return;
        }
        enemy.burnGap = HazardSurfaces::kEnemyBurnGap;
    }
    EnemyHit hit;
    hit.damage = harm->damage;
    hit.flags = harm->impact;
    hit.direction = touch->away;
    hit.where = enemy.position;
    hurt(slot, hit);
}

void Enemies::chooseTarget(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                           std::span<f32> crowding) {
    bool anyone = false;
    for (const EnemyView& view : players) {
        anyone = anyone || (!view.hidden && !view.invisible);
    }
    if (!anyone) {
        enemy.recognized = false;
    }
    // Everyone goes for the player IT has tagged while they can be seen (fn_800516F8).
    const auto tagged = std::ranges::find_if(players, [&](const EnemyView& view) {
        return view.it && !view.hidden && !view.invisible &&
               (enemy.kind != kDeathKind || !view.antiDeath);
    });
    // A mind looks round again every eighth frame, or at once when its player is gone.
    bool look =
        (m_simulation.frame % kRetargetEvery) == (static_cast<u32>(slot) % kRetargetEvery) ||
        enemy.target < 0;
    if (enemy.target >= 0) {
        const EnemyView* current = viewOf(players, enemy.target);
        if (current == nullptr || current->hidden || current->invisible ||
            (enemy.kind == kDeathKind && current->antiDeath)) {
            look = true;
        }
    }
    if (tagged != players.end()) {
        enemy.targetBefore = enemy.target;
        enemy.target = tagged->player;
        enemy.targetDistance = playerDistance(enemy, *tagged);
        enemy.weightedDistance = enemy.targetDistance;
        if (tagged->player >= 0 && static_cast<usize>(tagged->player) < crowding.size()) {
            enemy.weightedDistance += crowding[static_cast<usize>(tagged->player)];
        }
    } else if (look) {
        enemy.targetBefore = enemy.target;
        enemy.target = -1;
        enemy.weightedDistance = 100000.0f;
        enemy.targetDistance = 100000.0f;
        for (const EnemyView& view : players) {
            if (view.hidden || view.invisible || (enemy.kind == kDeathKind && view.antiDeath)) {
                continue;
            }
            const f32 distance = playerDistance(enemy, view);
            if (distance > enemy.sight) {
                continue;
            }
            f32 weighted = distance;
            if (distance > 5.0f * enemy.radius && view.player >= 0 &&
                static_cast<usize>(view.player) < crowding.size()) {
                weighted += crowding[static_cast<usize>(view.player)];
            }
            // The retail slot scan breaks ties by controller ID, not party storage order.
            if (weighted < enemy.weightedDistance ||
                (weighted == enemy.weightedDistance && enemy.target >= 0 &&
                 view.player < enemy.target)) {
                enemy.weightedDistance = weighted;
                enemy.targetDistance = distance;
                enemy.target = view.player;
            }
        }
    } else if (const EnemyView* current = viewOf(players, enemy.target); current != nullptr) {
        enemy.targetDistance = playerDistance(enemy, *current);
    }
    if (enemy.target >= 0) {
        if (enemy.targetDistance <= enemy.sight) {
            enemy.recognized = true;
            if (static_cast<usize>(enemy.target) < crowding.size()) {
                crowding[static_cast<usize>(enemy.target)] += 2.0f; // the next prefers another
            }
        }
    } else {
        enemy.targetDistance = 100000.0f;
        enemy.weightedDistance = 100000.0f;
    }
}

f32 Enemies::fightOf(const Enemy& enemy) const {
    const EnemyKind& kind = enemyKind(enemy.kind);
    const f32 fight = kind.damage * m_simulation.scales.damage;
    // A weakened one hits softer: two thirds under two thirds health, a third under a third.
    const f32 full = enemy.fullHealth;
    if (enemy.health > 0.667f * full || enemy.kind == kDeathKind) {
        return fight;
    }
    return enemy.health > 0.333f * full ? 0.667f * fight : 0.333f * fight;
}

bool Enemies::castsNow(const Enemy& enemy) {
    constexpr s32 kFirstCastingWay = 28;
    if (enemy.algorithm == kLungeWay) {
        return enemy.animator.powerStruck();
    }
    return enemy.algorithm >= kFirstCastingWay && enemy.algorithm <= kCastWay &&
           (enemy.animator.struck() || enemy.animator.powerStruck());
}

void Enemies::resolveBlows(Enemy& enemy, s32 slot, std::span<const EnemyView> players) {
    if (enemy.kind == kDeathKind) {
        return;
    }
    const bool landed = enemy.animator.struck() || enemy.animator.powerStruck();
    if (!landed || enemy.attackIndex < 0) {
        if (landed) {
            enemy.attackIndex = -1;
        }
        return;
    }
    const EnemyView* victim = viewOf(players, enemy.attackIndex);
    if (victim != nullptr && !victim->hidden) {
        EnemyBlow blow;
        blow.player = enemy.attackIndex;
        blow.kind = enemy.kind;
        blow.tier = enemy.tier;
        blow.damage = fightOf(enemy);
        blow.flags |= static_cast<u32>(enemy.affinity); // an attuned one's blow is of its element
        blow.power = enemy.animator.powerStruck();
        // Shrunk, it deals half, as a low blow, and its power blow is no more (fn_8004DF58).
        if (EnemyShrink::shrunk(m_simulation.shrink)) {
            blow.damage = EnemyShrink::harmDealt(m_simulation.shrink, blow.damage);
            blow.flags |= damage::kLow;
        } else {
            if (enemy.reach <= kKnockBackHeight) {
                blow.flags |= damage::kLow;
            }
            if (blow.power) {
                blow.damage *= kBlowGrowth;
                if (enemy.reach > kKnockBackHeight) {
                    blow.flags |= EnemyHit::kKnockBack;
                }
            }
        }
        const Vec3 toward = victim->position - enemy.position;
        const f32 length = flatDistance(victim->position, enemy.position);
        blow.direction = length > 0.001f
                             ? Vec3{toward.x / length, 0.0f, toward.z / length}
                             : Vec3{portable::sin(enemy.yaw), 0.0f, portable::cos(enemy.yaw)};
        blow.ward = victim->meleeWard;
        if (blow.ward != EnemyMeleeWard::None) {
            EnemyHit returned;
            returned.damage = blow.damage;
            returned.flags = blow.ward == EnemyMeleeWard::HealthVamp ? EnemyHit::kMagic : 0;
            returned.direction = Vec3{0};
            returned.where = bodyCentre(enemy);
            // The ward belongs to the victim, but the return carries no player credit
            // or level scaling. Resolve it before the attacker advances its reaction.
            hurt(slot, returned);
        }
        m_simulation.blows.push_back(blow);
        ++enemy.attackCount;
    }
    enemy.attackIndex = -1;
}

void Enemies::react(Enemy& enemy) { // NOLINT(readability-convert-member-functions-to-static)
    if (enemy.hurtPending < 1.0f && enemy.health > 0.0f) {
        enemy.pushMagnitude = enemy.push.x * enemy.push.x + enemy.push.z * enemy.push.z;
        return;
    }
    const bool floors = (enemy.hurtFlags & EnemyHit::kFloors) != 0 ||
                        (enemy.hurtPending > 10.0f && (enemy.hurtFlags & EnemyHit::kMagic) != 0);
    // A rooted kind (the acid blob) is thrown down where it stands and never shoved; a
    // knock-back alone leaves it as it was (fn_8004DC2C's E_ACID cases).
    const bool rooted = enemyKind(enemy.kind).rooted;
    f32 scale = 0.0f;
    if (floors) {
        enemy.animator.request(EnemyAction::HitReact2);
        if (!rooted) {
            scale = enemy.reach <= kKnockBackHeight ? 40.0f : 20.0f;
        }
    } else if ((enemy.hurtFlags & EnemyHit::kKnockBack) != 0) {
        if (!rooted) {
            enemy.animator.request(EnemyAction::HitReact1);
            scale = 8.0f;
        }
    } else {
        enemy.animator.request(EnemyAction::HitReact1);
    }
    if (scale > 0.0f) {
        enemy.push += enemy.hurtDirection * scale;
        ++enemy.pushes;
    }
    const f32 magnitude = glm::length(enemy.push);
    if (magnitude > kMostPush) {
        enemy.push *= kMostPush / magnitude;
    }
    enemy.hurtPending = 0.0f;
    enemy.hurtFlags = 0;
    enemy.hurtDirection = Vec3{0.0f, 0.0f, 0.0f};
    enemy.pushMagnitude = enemy.push.x * enemy.push.x + enemy.push.z * enemy.push.z;
    if (enemy.health <= 0.0f && enemy.state != State::Dying) {
        enemy.state = State::Dying;
        enemy.animator.request(EnemyAction::Dying);
    }
}

// ---- minds -------------------------------------------------------------------------------

f32 Enemies::turnToward(const Enemy& enemy, f32 wanted, s32 ticks) {
    const EnemyAction action = enemy.animator.action();
    if (action >= EnemyAction::HitReact1 || action == EnemyAction::Start) {
        return enemy.yaw;
    }
    f32 rate = enemyKind(enemy.kind).turnRate;
    if (action == EnemyAction::Run) {
        rate *= 3.0f;
    }
    const f32 step = rate * static_cast<f32>(ticks);
    const f32 d = wrapAngle(wanted - enemy.yaw);
    if (std::abs(d) <= step) {
        return wrapAngle(wanted);
    }
    return wrapAngle(enemy.yaw + (d > 0.0f ? step : -step));
}

/** Whether the proposed step reaches clear floor without crossing a wall or another body. */
bool Enemies::probeClear(const Enemy& enemy, const Vec3& at, std::span<const Obstacle> obstacles,
                         s32 self) const {
    if (m_collision != nullptr) {
        // FloorCollide uses half the enemy radius, not just the centre point. Resolve
        // reachable tread support before testing walls, so the next riser does not
        // block a body whose feet have already reached the preceding tread.
        const auto floor = stepFloor(enemy, enemy.position, at);
        if (!floor) {
            return false;
        }
        const f32 wallY = std::max(at.y, floor->y) + kWallHeight;
        // Use the physical body for both route planning and travel; the enlarged
        // preliminary retail wall query is not a permanent collision cylinder.
        const Vec3 pushed = m_collision->sweepWalls(enemy.position, at, enemy.radius, wallY, wallY);
        if (flatDistance(pushed, at) > 0.01f) {
            return false;
        }
    }
    for (const Obstacle& box : obstacles) {
        if (box.enemyItem ? itemBlocksEnemy(box, enemy.position, at, enemy.radius + 0.1f)
                          : box.solid && box.pushOut(at, enemy.radius) != at) {
            return false;
        }
    }
    const Vec3 offset{0, enemyKind(enemy.kind).collisionHeight, 0};
    if (combatantContact(m_simulation.combatants, enemy.position + offset, at + offset,
                         enemy.radius) != nullptr) {
        return false;
    }
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& other = m_simulation.enemies[static_cast<usize>(i)];
        if (i == self || other.state == State::Inactive || other.state == State::Dying) {
            continue;
        }
        const Vec3 centre = other.position + Vec3{0, enemyKind(other.kind).collisionHeight, 0};
        if (movementTouchesBody(enemy.position + offset, at + offset, centre,
                                enemy.radius + other.radius, enemy.reach + other.reach)) {
            return false;
        }
    }
    return true;
}

/** What the mind is given to go on this tick. */
MindSense Enemies::sense(const Enemy& enemy, s32 slot, s32 ticks,
                         std::span<const EnemyView> players,
                         std::span<const Obstacle> obstacles) const {
    MindSense sense;
    sense.position = enemy.position;
    sense.yaw = enemy.yaw;
    sense.radius = enemy.radius;
    sense.sight = enemy.sight;
    sense.pace = paceOf(enemy.kind);
    sense.ticks = ticks;
    sense.target = enemy.target;
    sense.targetDistance = enemy.targetDistance;
    sense.closeDistance = enemy.weightedDistance;
    sense.recognized = enemy.recognized;
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        sense.targetPosition = view->decoy.value_or(view->position);
        sense.targetPlayerPosition = view->position;
    }
    sense.contact = enemy.contact;
    if (const EnemyView* view = viewOf(players, enemy.contact); view != nullptr) {
        sense.contactPosition = view->position;
    }
    sense.bumpedWall = enemy.bumpedWall;
    sense.bumpedOther = enemy.bumpedOther;
    sense.blocked = enemy.blocked;
    sense.otherSide = enemy.otherSide;
    sense.generatorGone = enemy.generator < 0;
    sense.onScreen = enemy.onScreen;
    sense.mirroredWander = (slot & 1) != 0;
    sense.lookouts = &m_simulation.lookouts;
    sense.action = enemy.animator.action();
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        sense.targetVertical = view->position.y - enemy.position.y;
    }
    const f32 reach = paceOf(enemy.kind) * static_cast<f32>(ticks);
    sense.clear = [this, &enemy, obstacles, slot, reach](f32 heading) {
        Vec3 probe = enemy.position;
        probe.x += reach * portable::sin(heading);
        probe.z += reach * portable::cos(heading);
        return probeClear(enemy, probe, obstacles, slot);
    };
    sense.open = [this, &enemy, obstacles, slot, reach](f32 heading) {
        Vec3 probe = enemy.position;
        probe.x += reach * portable::sin(heading);
        probe.z += reach * portable::cos(heading);
        // Route selection must see the same floor, item and body obstructions as movement.
        return probeClear(enemy, probe, obstacles, slot);
    };
    sense.wanderClear = [this, &enemy, obstacles, slot, reach](f32 heading) {
        const Vec3 direction{portable::sin(heading), 0, portable::cos(heading)};
        // move_logic05/06 look radius + 0.5 ahead at radius + 0.1 above the feet,
        // then check the tick- and level-scaled step from do_enemies' speed table.
        if (m_collision != nullptr) {
            const Vec3 from = enemy.position + Vec3{0, enemy.radius + 0.1f, 0};
            const Vec3 to = from + direction * (enemy.radius + 0.5f);
            std::vector<WallContact> contacts;
            m_collision->sweepWalls(from, to, 0, from.y, from.y, &contacts);
            if (!contacts.empty()) {
                return false;
            }
        }
        return probeClear(enemy, enemy.position + direction * reach, obstacles, slot);
    };
    return sense;
}

/** The mind decides and the body carries it out: a step along the heading at the pace, a
 * turn toward it, the action asked of the animator, and perhaps a change of mind. */
void Enemies::think(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
                    std::span<const Obstacle> obstacles) {
    MindSense sensed = sense(enemy, slot, ticks, players, obstacles);
    sensed.random = m_simulation.random();
    sensed.tier = enemy.tier;
    sensed.castWait = static_cast<s32>(kCastTicks * m_simulation.scales.missileRate);
    s32 algorithm = enemy.algorithm;
    // Near a lit bomber most ways run from it for the tick (FoundSuicideBomber): within ten
    // of it, its player within this one's sight, and not held after a bump or rising.
    // Level placements and generate_single sentries have nonzero birth_style and stay put.
    if (m_simulation.bomber >= 0 && m_simulation.bomber != slot && fleesBombers(algorithm) &&
        enemy.mind.deadEnd <= 0 && enemy.birth == Birth::Brood &&
        enemy.animator.action() != EnemyAction::Start) {
        const Enemy& bomber = m_simulation.enemies[static_cast<usize>(m_simulation.bomber)];
        if (bomber.targetDistance <= enemy.sight &&
            glm::distance(bomber.position, enemy.position) < kBomberScare) {
            algorithm = kFleeWay;
            sensed.bomber = bomber.position;
        }
    }
    std::optional<f32> retreat;
    if (enemy.kind == kDeathKind) {
        algorithm = kSeekWay; // move_logic03 uses Seek's slot-dependent wander fallback.
        if (enemy.target < 0) {
            const EnemyView* threat = nullptr;
            f32 nearest = enemy.sight;
            for (const EnemyView& view : players) {
                if (view.hidden || view.invisible || !view.antiDeath) {
                    continue;
                }
                const f32 distance = playerDistance(enemy, view);
                if (distance < nearest ||
                    (distance == nearest && (threat == nullptr || view.player < threat->player))) {
                    threat = &view;
                    nearest = distance;
                }
            }
            if (threat != nullptr) {
                f32 offset = 0;
                if (!enemy.blocked) {
                    enemy.mind.counter = 0; // each stop starts the nudges over
                } else if (enemy.mind.counter < 8) {
                    const s32 attempt = enemy.mind.counter++;
                    const s32 pair = attempt / 2 + 1;
                    offset =
                        static_cast<f32>(pair) * (kPi / 36) * (attempt % 2 == 0 ? 1.0f : -1.0f);
                }
                const Vec3 toward = threat->decoy.value_or(threat->position) - enemy.position;
                retreat = wrapAngle(portable::atan2(toward.x, toward.z) + kPi + offset);
            }
        }
    }
    MindIntent intent;
    if (retreat.has_value()) {
        intent.heading = *retreat;
        intent.pace = 0.9f;
        enemy.mind.heading = *retreat;
    } else {
        intent = enemyMindOf(algorithm).think(enemy.mind, sensed);
    }
    if (intent.become.has_value()) {
        enemy.algorithm = *intent.become;
    }
    if (intent.expire) {
        enemy.expired = true;
        return;
    }
    if (intent.explode) {
        // It goes up: dead of its own blast, which its death sets off.
        EnemyHit own;
        own.damage = 999.0f;
        own.player = -1;
        own.selfInflicted = true;
        hurt(slot, own);
        react(enemy); // dead of it at once
        return;
    }
    if (intent.yell) {
        m_simulation.cues.push_back(
            {EnemyCue::Kind::Yell, enemy.kind,
             enemy.position + Vec3{0.0f, enemyKind(enemy.kind).attentionHeight, 0.0f}, slot,
             enemy.instance});
    }
    if (intent.throwing) {
        enemy.animator.request(EnemyAction::Throw);
    }
    if (intent.strike) {
        enemy.attackIndex = enemy.target;
    }
    if (intent.pace > 0.0f) {
        enemy.animator.request(intent.action == EnemyAction::Walk && intent.pace >= kRunFrom
                                   ? EnemyAction::Run
                                   : intent.action);
    } else {
        enemy.animator.request(intent.action);
    }
    if (intent.facing.has_value() || intent.turn) {
        enemy.yaw = turnToward(enemy, intent.facing.value_or(intent.heading), ticks);
    }
    const Vec3 step = Vec3{portable::sin(intent.heading), 0.0f, portable::cos(intent.heading)} *
                      (paceOf(enemy.kind) * intent.pace * static_cast<f32>(ticks));
    move(enemy, slot, ticks, static_cast<f32>(ticks) / static_cast<f32>(kTicksPerSecond), step,
         players, obstacles, !retreat.has_value() && enemy.mind.effectiveWay == kSeekWay);
    if (!retreat.has_value() &&
        (enemy.mind.effectiveWay == kZigZagWay || enemy.mind.effectiveWay == kProwlWay ||
         enemy.mind.effectiveWay == kMirroredProwlWay)) {
        // Use the handler that actually moved: a prowler may temporarily seek.
        // Collision-driven turns and clear-step bookkeeping run after this step.
        enemyMindOf(enemy.mind.effectiveWay)
            .afterMove(enemy.mind, sensed, sense(enemy, slot, ticks, players, obstacles));
    }
}

// ---- bodies ------------------------------------------------------------------------------

std::optional<FloorHit> Enemies::stepFloor(const Enemy& enemy, const Vec3& from,
                                           const Vec3& to) const {
    // do_enemy_collide / fn_80045C30 (80045488 / 80045C30) probe from the
    // collision centre + 2, then reject excessive floor deltas separately.
    // A short upward ray from the feet instead sees the ground UNDER a raised
    // ramp, letting a lateral approach bury the body in its wooden deck.
    const Vec3 step = to - from;
    const f32 distance = glm::length(step);
    const f32 retryDistance = flatDistance(from, to);
    const Vec3 offset{0, enemyKind(enemy.kind).collisionHeight + 2, 0};
    const Vec3 origin = from + offset;
    f32 tolerance = 2 * (0.1f + retryDistance + enemy.radius);
    if (enemy.kind == kGolemEnemyKind || enemy.reach <= 2) {
        // Preserve the target's signed birth-reference arithmetic; this is
        // not simply a distance from the proposed position to the birthplace.
        const Vec3 birthDelta = enemy.birthPosition - origin;
        const Vec3 movedDelta = birthDelta + step;
        if (glm::dot(movedDelta, movedDelta) > glm::dot(birthDelta, birthDelta)) {
            tolerance = 0.5f * (retryDistance + enemy.radius);
        }
    }
    const auto probe = [&](const Vec3& at) {
        // Forward-only footprint support cannot hold a body walking away from
        // a ledge on the surface behind it.
        return distance > 0.0001f ? m_collision->floorAhead(at, step, enemy.reach, enemy.reach + 5,
                                                            0.5f * enemy.radius)
                                  : m_collision->floorAt(at, enemy.reach, enemy.reach + 5);
    };
    const Vec3 ahead = distance > 0.0001f ? step * (enemy.radius / distance) : Vec3{0};
    const auto floor = probe(to + offset + ahead);
    if (!floor || std::abs(floor->y - from.y) > tolerance) {
        return std::nullopt;
    }
    // Retail conditionally repeats at the endpoint when the ahead height
    // changes. Our multi-wall/substep solver must always re-resolve that
    // footprint: an equal-height surface ahead can skip a narrow rail below
    // the actual body, especially after a lateral wall slide.
    const auto endpoint = probe(to + offset);
    return endpoint && std::abs(endpoint->y - from.y) <= tolerance ? endpoint : std::nullopt;
}

Vec3 Enemies::travel(const Enemy& enemy, const Vec3& from, const Vec3& to) const {
    if (m_collision == nullptr) {
        return to;
    }
    const Vec3 offset = to - from;
    const auto steps =
        std::max(1, static_cast<s32>(std::ceil(glm::length(offset) / (0.5f * enemy.radius))));
    const Vec3 stride = offset / static_cast<f32>(steps);
    Vec3 position = from;
    for (s32 i = 0; i < steps; ++i) {
        const Vec3 wanted = position + stride;
        const auto support = stepFloor(enemy, position, wanted);
        if (!support) {
            break;
        }
        const f32 wallY = std::max(position.y, support->y) + kWallHeight;
        // do_enemy_collide initially probes at 1.5 * rad, but fn_80045C30 resolves
        // the supported step with rad. Applying the enlarged radius to every pass
        // of our multi-wall sweep wedges bodies between nearby walls (B1's first
        // vent passage). Sweep the authored body, retaining solid-wall clipping.
        Vec3 next = m_collision->sweepWalls(position, wanted, enemy.radius, wallY, wallY);
        const auto floor = stepFloor(enemy, position, next);
        if (!floor) {
            break;
        }
        next.y = floor->y;
        position = next;
    }
    return position;
}

void Enemies::move(Enemy& enemy, s32 slot, s32 ticks, f32 seconds, const Vec3& step,
                   std::span<const EnemyView> players, std::span<const Obstacle> obstacles,
                   bool seeking) {
    const auto finishSeek = [&](SeekContact contact, s32 side = 1) {
        if (!seeking) {
            return;
        }
        const SeekResponse response = finishSeekStep(enemy.mind, contact, side);
        if (response.faceTarget) {
            enemy.yaw = enemy.mind.heading;
        }
        if (response.stopWalking) {
            enemy.animator.stopWalking();
        }
    };
    Vec3 translation = step;
    // Nothing of its own while stunned (the stun running down with the step it stops,
    // do_enemy_move), reacting or swinging (a running attack runs on); a push moves it
    // regardless.
    const bool stunned = enemy.stunTicks > 0;
    if (stunned) {
        enemy.stunTicks -= ticks;
    }
    const EnemyAction doing = enemy.animator.action();
    const bool runningAttack = doing == EnemyAction::RunAttack || doing == EnemyAction::RunAttack2;
    if (stunned || enemy.animator.reacting() || (enemy.animator.swinging() && !runningAttack) ||
        enemy.animator.entering()) {
        translation = Vec3{0.0f, 0.0f, 0.0f};
    }
    translation += enemy.push * seconds;
    // A touch of the world is remembered while the mind's hold on the heading runs, so
    // the mind sees it once more as the hold ends; a dead stop is this step's alone.
    if (enemy.mind.deadEnd <= 0) {
        enemy.bumpedWall = false;
    }
    enemy.bumpedOther = false;
    enemy.blocked = false;
    enemy.contact = -1;
    const Vec3 from = enemy.position;
    Vec3 to = from + translation;
    const Vec3 collisionOffset{0, enemyKind(enemy.kind).collisionHeight, 0};
    // do_enemy_move first lets do_enemy_collide correct the translation. A player
    // behind a wall must not intercept the unclipped part of a long movement.
    bool stoppedByWorld = false;
    if (m_collision != nullptr && glm::length(translation) > 0.0f) {
        const Vec3 target = travel(enemy, from, to);
        stoppedByWorld = flatDistance(target, to) > 0.001f && flatDistance(target, from) < kStopped;
        to = target;
        if (stoppedByWorld) {
            finishSeek(SeekContact::World);
        }
    }
    // fn_80046680's movement path tests the actual nearest live player, not the
    // crowd-weighted sight target or the first overlapping member of the roster.
    // Invisible players still have a body; an enemy without a sight target does not
    // test player contact at all. Controller order breaks equal-distance ties.
    const EnemyView* nearest = nullptr;
    f32 nearestDistance = std::numeric_limits<f32>::max();
    if (enemy.onScreen && enemy.target >= 0) {
        for (const EnemyView& view : players) {
            if (view.hidden) {
                continue;
            }
            const f32 distance = playerDistance(enemy, view);
            if (distance < nearestDistance || (distance == nearestDistance && nearest != nullptr &&
                                               view.player < nearest->player)) {
                nearest = &view;
                nearestDistance = distance;
            }
        }
    }
    if (nearest != nullptr) {
        const EnemyView& view = *nearest;
        const Vec3 centre =
            view.position + Vec3{0, view.collisionHeight.value_or(0.5f * view.height), 0};
        // fn_80046680 adds Player::col_radius (the authored width), not
        // the smaller radius used when moving the player against walls.
        const f32 radius = view.collisionRadius.value_or(view.radius);
        if (movementTouchesBody(from + collisionOffset, to + collisionOffset, centre,
                                radius + enemy.radius + 0.5f, enemy.reach + 0.5f * view.height)) {
            enemy.contact = view.player;
        }
    }
    if (enemy.contact >= 0) {
        if (const EnemyView* view = viewOf(players, enemy.contact); view != nullptr) {
            enemy.mind.route = turnDirection(from, view->position);
        }
        // IT touching a player tags them and is gone, never striking (fn_80046140,
        // enemy_dies: no reward). Death drains at contact; it has no melee swing or recovery
        // to hold movement.
        if (enemy.kind == kItKind) {
            if (enemy.state == State::Active && !enemy.killed) {
                m_simulation.tagged.push_back(enemy.contact);
                enemy.health = 0.0f;
                enemy.killed = true;
                enemy.state = State::Dying;
                enemy.animator.request(EnemyAction::Dying);
            }
        } else if (enemy.kind != kDeathKind && enemy.state == State::Active &&
                   enemy.algorithm != kLungeWay) {
            enemy.attackIndex = enemy.contact;
            enemy.animator.request((enemy.attackCount & 7) == 7 ? EnemyAction::PowerAttack
                                                                : EnemyAction::Attack);
        }
        return;
    }
    if (stoppedByWorld) {
        // A slide that still gets somewhere is no bump; a dead stop is.
        enemy.bumpedWall = true;
        enemy.blocked = true;
        return;
    }
    if (const auto* body = combatantContact(m_simulation.combatants, from + collisionOffset,
                                            to + collisionOffset, enemy.radius)) {
        enemy.bumpedOther = true;
        enemy.blocked = true;
        enemy.otherSide = turnDirection(from, body->centre);
        finishSeek(SeekContact::Critter);
        return;
    }
    std::optional<s32> itemSide;
    for (const Obstacle& obstacle : obstacles) {
        if (obstacle.solid) {
            const Obstacle box = enemyItemShape(obstacle);
            if (separatingFromItem(box, from, to, enemy.radius)) {
                continue;
            }
            // Short steps resolve by overlap below, retaining the tangential slide. A long
            // push can skip the body entirely, so clip that path before resolving contact.
            if (flatDistance(from, to) > enemy.radius && box.pushOut(from, enemy.radius) == from &&
                box.pushOut(to, enemy.radius) == to) {
                if (const auto contact = box.contact(from, to, enemy.radius)) {
                    const Vec3 clipped = from + (to - from) * *contact;
                    if (flatDistance(clipped, to) > 0.001f) {
                        enemy.bumpedWall = true;
                        enemy.blocked = flatDistance(clipped, from) < kStopped;
                        itemSide = turnDirection(from, box.centre);
                    }
                    to = clipped;
                }
            }
            const Vec3 pushed = box.pushOut(to, enemy.radius);
            if (pushed != to) {
                enemy.bumpedWall = true;
                itemSide = turnDirection(from, box.centre);
                to = travel(enemy, to, pushed);
                if (flatDistance(to, from) < kStopped &&
                    flatDistance(from + translation, from) >= kStopped) {
                    enemy.blocked = true;
                }
            }
        }
    }
    // Against another it stops, unless it is being thrown, when half the push carries over.
    for (s32 i = 0; i < m_simulation.most; ++i) {
        Enemy& other = m_simulation.enemies[static_cast<usize>(i)];
        // fn_8004646C drops DYING bodies immediately, but sleeping placements remain solid.
        if (i == slot || other.state == State::Inactive || other.state == State::Dying) {
            continue;
        }
        const Vec3 centre = other.position + Vec3{0, enemyKind(other.kind).collisionHeight, 0};
        if (movementTouchesBody(from + collisionOffset, to + collisionOffset, centre,
                                enemy.radius + other.radius, enemy.reach + other.reach)) {
            enemy.bumpedOther = true;
            if (enemy.pushMagnitude > 1.0f && enemy.animator.reacting()) {
                other.push += 0.5f * enemy.push;
                other.position = travel(other, other.position, other.position + 0.5f * translation);
                rememberFloor(other);
            } else {
                enemy.blocked = true;
                enemy.otherSide = turnDirection(from, other.position);
                finishSeek(SeekContact::Enemy, enemy.otherSide);
                return;
            }
        }
    }
    enemy.position = to;
    rememberFloor(enemy);
    finishSeek(itemSide ? SeekContact::Item : SeekContact::Clear, itemSide.value_or(1));
    (void)ticks;
}

// ---- being hit ---------------------------------------------------------------------------

f32 Enemies::hurt(s32 id, const EnemyHit& hit) {
    if (id < 0 || id >= m_simulation.most) {
        return 0;
    }
    Enemy& enemy = m_simulation.enemies[static_cast<usize>(id)];
    if ((enemy.state != State::Active && enemy.state != State::Asleep) || enemy.killed) {
        return 0;
    }
    if (enemy.kind == kDeathKind) {
        hurtDeath(enemy, id, hit);
        return 0;
    }
    if (enemy.kind == kItKind) {
        return 0; // nothing harms IT (damage_enemy)
    }
    if (enemy.affinity != 0 &&
        (hit.flags & damage::kElementMask) == static_cast<u32>(enemy.affinity)) {
        return 0; // an attuned one shrugs off its own element
    }
    const EnemyKind& kind = enemyKind(enemy.kind);
    f32 amount = hit.damage;
    // damage_enemy credits healing before level scaling, armor and shrink effects.
    const f32 credited = std::clamp(amount, 0.0f, enemy.health);
    // A character under the level the place is meant for hits a hundredth softer a level;
    // one over it a tenth harder. Armour comes off, but a character always gets a point in.
    if (hit.player >= 0 && m_simulation.scales.playerLevel > 0.0f) {
        const f32 gap = static_cast<f32>(hit.level) - m_simulation.scales.playerLevel;
        amount *= gap < 0.0f ? 1.0f + 0.01f * gap : 1.0f + 0.1f * gap;
    }
    const Damage modified =
        Damage::modify(amount, hit.flags, 0, kind.armor, m_simulation.scales.bossEncounter);
    amount = std::max(EnemyShrink::harmTaken(m_simulation.shrink, modified.amount),
                      hit.player >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return credited;
    }
    enemy.state = State::Active;
    enemy.health -= amount;
    if (enemy.algorithm == kPatrolWay && enemy.generator >= 0) {
        m_simulation.generatorEvents.push_back(
            {enemy.generator, EnemyGeneratorEvent::Kind::PatrolHit});
    }
    enemy.hurtPending += amount;
    if ((modified.flags & damage::kElementMask) != 0) {
        enemy.hurtFlags &= ~damage::kElementMask;
    }
    enemy.hurtFlags |= modified.flags;
    const f32 length = glm::length(hit.direction);
    if (length > 0.001f) {
        enemy.hurtDirection = hit.direction / length;
    }
    enemy.hurtBy = hit.player;
    const bool killed = enemy.health <= 0.0f;
    ++enemy.hitCount;
    const EnemyFeedback feedback{enemy.kind,
                                 enemy.tier,
                                 enemy.hitCount,
                                 killed,
                                 hit.close,
                                 hit.flags,
                                 hit.where.has_value() && enemy.reach >= 4.0f
                                     ? *hit.where
                                     : enemy.position + Vec3{0, kind.attentionHeight, 0},
                                 enemy.yaw,
                                 enemy.reach,
                                 id,
                                 enemy.instance,
                                 enemy.health};
    // A suicide's own fuse kills it without a cry, a burst of blood or a death skin
    // (damage_enemy's play_effects, fn_800945D0's player_index test).
    if (!hit.selfInflicted) {
        m_simulation.feedback.push_back(feedback);
    }
    enemy.flashSeconds = killed ? 0.0f : 2.0f / 30.0f;
    if (killed) {
        detachGenerator(enemy);
        enemy.killed = true;
        enemy.state = State::Dying;
        if (!hit.selfInflicted) {
            enemy.deathSkin = feedback.deathSkin();
            enemy.deathSkinFrames = feedback.deathSkinFrames();
        }
        // SetSkinFX starts at -rate so the first 30 Hz death update displays frame zero.
        enemy.deathSeconds = -1.0f / 30.0f;
        // Whatever kills a suicide sets it off, a blast where it stood (enemy_dies); only a
        // hit cuts its yell short.
        if (enemy.algorithm == kSuicideWay) {
            EnemyBurst burst;
            burst.position = bodyCentre(enemy);
            burst.damage = kSuicideDamage * m_simulation.scales.damage;
            burst.enemy = id;
            burst.kind = enemy.kind;
            burst.silencesYell = !hit.selfInflicted;
            burst.instance = enemy.instance;
            m_simulation.bursts.push_back(burst);
        }
    }
    if (hit.player >= 0) {
        EnemyLoss loss;
        loss.enemy = id;
        loss.kind = enemy.kind;
        loss.tier = enemy.tier;
        loss.player = hit.player;
        loss.killed = killed;
        loss.experience = killed ? kind.experienceKill : kind.experienceHit;
        loss.position = bodyCentre(enemy);
        loss.affinity = enemy.affinity;
        m_simulation.losses.push_back(loss);
    }
    return credited;
}

std::vector<s32> Enemies::takeTagged() {
    std::vector<s32> out;
    out.swap(m_simulation.tagged);
    return out;
}

std::vector<EnemyDeathShot> Enemies::takeDeathShots() {
    std::vector<EnemyDeathShot> out;
    out.swap(m_simulation.deathShots);
    return out;
}

void Enemies::aimDeathShot(const Enemy& enemy, s32 slot, std::span<const EnemyView> players) {
    const EnemyView* at = viewOf(players, enemy.target);
    if (at == nullptr || at->hidden) {
        at = nullptr;
        for (const EnemyView& view : players) {
            if (!view.hidden) {
                at = &view;
                break;
            }
        }
    }
    if (at == nullptr) {
        return;
    }
    EnemyDeathShot shot;
    shot.enemy = slot;
    shot.kind = enemy.kind;
    shot.position = enemy.position;
    const Vec3 way = at->position - enemy.position;
    const f32 length = glm::length(way);
    shot.direction = length > 0.001f
                         ? way / length
                         : Vec3{portable::sin(enemy.yaw), 0.0f, portable::cos(enemy.yaw)};
    m_simulation.deathShots.push_back(shot);
}

void Enemies::die(Enemy& enemy) {
    detachGenerator(enemy);
    enemy = Enemy{};
}

void Enemies::detachGenerator(Enemy& enemy) {
    if (enemy.generator >= 0) {
        m_simulation.generatorEvents.push_back(
            {enemy.generator, enemy.algorithm == kPatrolWay
                                  ? EnemyGeneratorEvent::Kind::PatrolDetached
                                  : EnemyGeneratorEvent::Kind::Detached});
        enemy.generator = -1;
    }
}

std::vector<EnemyGeneratorEvent> Enemies::takeGeneratorEvents() {
    return std::exchange(m_simulation.generatorEvents, {});
}

std::vector<EnemyFeedback> Enemies::takeFeedback() {
    std::vector<EnemyFeedback> out;
    out.swap(m_simulation.feedback);
    return out;
}

/** A shot or a lob at the player it is after, aimed from its middle at theirs (or twenty
 * ahead with nobody), with the launch sound of what goes (fn_8004E448). */
void Enemies::shoot(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                    EnemyMissiles& missiles, f32 speedScale, std::span<const Obstacle> items) {
    constexpr f32 kBlindReach = 20.0f;
    Stock* stock = stockOf(enemy.kind);
    if (stock == nullptr) {
        return;
    }
    const Vec3 from = bodyCentre(enemy);
    Vec3 aim = from + Vec3{portable::sin(enemy.yaw), 0.0f, portable::cos(enemy.yaw)} * kBlindReach;
    if (const EnemyView* view = viewOf(players, enemy.target); view != nullptr) {
        aim = view->position + Vec3{0.0f, 0.5f * view->height, 0.0f};
    }
    // The slot its way throws from, and what the kind keeps there; the medium kinds' arrow
    // when it keeps nothing.
    const s32 which = enemy.variant == kBomberStrength ? EnemyMissileKind::kBomb
                                                       : missileSlotOfWay(enemy.algorithm);
    EnemyMissileKind what = enemyMissileOf(enemy.kind, which).value_or(EnemyMissileKind::arrow());
    if (enemy.affinity != 0) {
        // The low nibble is an element identifier, not independent elemental bits.
        what.flags = (what.flags & ~damage::kElementMask) | static_cast<u32>(enemy.affinity);
    }
    // The trees are the kind's <PREFIX>_ARROW, _BOMB and _FBALL (InitEnemyMissiles); a kind
    // with no tree for the slot lets nothing go, and nothing goes at a player more than an
    // eighth of a turn off its facing (EnemyStartMissile).
    const TreeModel* model = &stock->fireball;
    if (which == EnemyMissileKind::kArrow) {
        model = &stock->arrow;
    } else if (which == EnemyMissileKind::kBomb) {
        model = &stock->bomb;
    }
    if (!model->bound()) {
        return;
    }
    EnemyMissileLaunch launch;
    launch.body = from;
    launch.target = aim;
    launch.facing = enemy.yaw;
    launch.point = enemyLaunchPointOf(enemy.kind, which);
    launch.speedScale = speedScale;
    launch.aimError = m_simulation.scales.missileAim;
    launch.model = model;
    launch.shooter = slot;
    if (!missiles.launch(what, launch, m_collision, items)) {
        return;
    }
    // The arrow's and the bolt's go off as they leave; a lob goes quietly.
    if (which == EnemyMissileKind::kArrow) {
        m_simulation.cues.push_back(
            {EnemyCue::Kind::Arrow, enemy.kind, from, slot, enemy.instance});
    } else if (which == EnemyMissileKind::kBolt) {
        m_simulation.cues.push_back({EnemyCue::Kind::Bolt, enemy.kind, from, slot, enemy.instance});
    }
}

std::vector<EnemyCue> Enemies::takeCues() {
    return std::exchange(m_simulation.cues, {});
}

Enemies::Figure Enemies::bodyOf(const Enemy& enemy) {
    return bodyOf(Appearance{enemy.kind, enemy.tier, enemy.variant, enemy.state == State::Asleep});
}

Enemies::Figure Enemies::bodyOf(const Appearance& appearance) {
    if (appearance.tier < 1 || appearance.tier > 3) {
        return {};
    }
    Stock* stock = stockOf(appearance.kind);
    if (stock == nullptr || !stock->unseen.sequences.empty()) {
        return {};
    }
    if (appearance.kind == kDeathKind && appearance.asleep) {
        const usize index = appearance.tier == 2 ? 1 : 0;
        TreeModel& statue = stock->deathStatues[index];
        if (statue.bound()) {
            return {&statue, stock->deathStatueTrees[index], &stock->textures};
        }
    }
    if (appearance.variant != 0) {
        const auto v = static_cast<usize>(appearance.variant - kArcherStrength);
        if (v < stock->variantBodies.size() && stock->variantBodies[v].bound()) {
            return {&stock->variantBodies[v], stock->variantTrees[v], &stock->textures};
        }
    }
    // A tier the archive lacks wears the nearest it has, as treeOf does: below, else any.
    s32 tier = appearance.tier;
    while (tier > 1 && stock->trees[static_cast<usize>(tier - 1)] == nullptr) {
        --tier;
    }
    for (s32 t = 1; stock->trees[static_cast<usize>(tier - 1)] == nullptr && t <= 3; ++t) {
        if (stock->trees[static_cast<usize>(t - 1)] != nullptr) {
            tier = t;
        }
    }
    const auto index = static_cast<usize>(tier - 1);
    TreeModel& body = stock->bodies[index];
    return body.bound() ? Figure{&body, stock->trees[index], &stock->textures} : Figure{};
}

const TreeInfo* Enemies::appearanceTree(const Appearance& appearance) {
    return bodyOf(appearance).tree;
}

void Enemies::drawPose(RenderDevice& device, const Mat4& clip, const Mat4& placement,
                       const WorldLighting& lighting, const Appearance& appearance,
                       const TreePose& pose, u32 sequence, f32 frame, f32 textureFrame,
                       const Texture* hitFlash, const CameraFrame* camera, TreeModel::Pass pass,
                       f32 alpha, const Texture* deathSkin, bool dying) {
    const Figure found = bodyOf(appearance);
    if (found.model == nullptr || found.tree == nullptr || found.textures == nullptr) {
        return;
    }
    const bool statue = appearance.kind == kDeathKind && appearance.asleep;
    auto& body = *found.model;
    found.textures->apply(body, *found.tree, statue ? 0 : sequence, statue ? 0 : frame,
                          textureFrame - static_cast<f32>(found.textures->frame()));
    body.setAppearance(hitFlash != nullptr || (dying && appearance.kind != kDeathKind));
    body.setMaskedTexture(deathSkin != nullptr ? deathSkin : hitFlash,
                          deathSkin != nullptr ? TreeModel::SkinAlpha::Dissolve
                                               : TreeModel::SkinAlpha::PreserveCoverage);
    body.setPresentationFrame(statue ? 0 : sequence, statue ? 0 : frame);
    body.draw(device, clip, placement, lighting, statue ? std::span<const Mat4>{} : pose.matrices(),
              camera, alpha, pass);
    body.setAppearance(false);
    body.setMaskedTexture(nullptr);
}

std::vector<EnemyBurst> Enemies::takeBursts() {
    return std::exchange(m_simulation.bursts, {});
}

std::vector<EnemyBirth> Enemies::takeBirths() {
    return std::exchange(m_simulation.births, {});
}

std::vector<EnemyBlow> Enemies::takeBlows() {
    return std::exchange(m_simulation.blows, {});
}

std::vector<EnemyLoss> Enemies::takeLosses() {
    return std::exchange(m_simulation.losses, {});
}

std::vector<MissileTarget> Enemies::targets() const {
    std::vector<MissileTarget> out;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        // Nothing takes aim at IT (PlayerGetTarget).
        if (!alive(i) || enemy.kind == kItKind) {
            continue;
        }
        MissileTarget target{i, enemy.position, enemy.radius, enemy.height};
        constexpr f32 kAimHeightLimit = 10;
        target.acquisition = MissileTarget::Acquisition{
            enemy.position + Vec3{0, enemyKind(enemy.kind).collisionHeight, 0}, enemy.radius, 1,
            kAimHeightLimit};
        out.push_back(target);
    }
    return out;
}

std::vector<EnemyBody> Enemies::movementBodies() const {
    std::vector<EnemyBody> bodies;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (alive(i) && enemy.kind != kItKind) {
            bodies.push_back({i, bodyCentre(enemy), enemy.radius, 0.5f * enemy.height});
        }
    }
    return bodies;
}

MeleeGeometry::Target Enemies::meleeBody(s32 id) const {
    const auto& enemy = m_simulation.enemies[static_cast<usize>(id)];
    const auto coordinate = [this](f32 value, f32 origin) {
        return static_cast<s32>(std::clamp((value - origin) * m_simulation.contactGridInverseWidth,
                                           0.0f, static_cast<f32>(kContactGridDimension - 1)));
    };
    constexpr f32 kAimHeightLimit = 10;
    constexpr f32 kLowEnemyHeight = 4;
    MeleeGeometry::Target target;
    target.id = id;
    target.instance = enemy.instance;
    target.base = enemy.position;
    target.radius = enemy.radius;
    target.height = enemy.height;
    target.acquisition = {enemy.position + Vec3{0, enemyKind(enemy.kind).collisionHeight, 0},
                          enemy.radius, 1, kAimHeightLimit};
    target.contactCell =
        coordinate(enemy.position.z, m_simulation.contactGridOrigin.y) * kContactGridDimension +
        coordinate(enemy.position.x, m_simulation.contactGridOrigin.x);
    target.contact = true;
    target.requireFacing = enemy.kind == kDeathKind;
    // A low target takes the low swing: one whose hit height is two or under, or any of the
    // swarm while the shrinker has them under full size (PlayerMotion, pmotion.c 1603: the
    // enemy scale under one sets coll_flags' low bit for every swarm target).
    target.low = enemy.height <= kLowEnemyHeight || EnemyShrink::shrunk(m_simulation.shrink);
    target.blocksMovement = enemy.kind != kDeathKind;
    return target;
}

std::vector<MeleeGeometry::Target> Enemies::meleeGeometry() const {
    std::vector<MeleeGeometry::Target> targets;
    targets.reserve(static_cast<usize>(m_simulation.most));
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (alive(i) && enemy.kind != kItKind) {
            targets.push_back(meleeBody(i));
        }
    }
    return targets;
}

std::optional<s32> Enemies::playerContact(const Vec3& from, const Vec3& to, f32 width,
                                          f32 halfHeight, f32* endpointDistance) const {
    std::array<MeleeGeometry::Target, kMost> targets;
    usize count = 0;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (alive(i) && enemy.kind != kItKind) {
            targets[count++] = meleeBody(i);
        }
    }
    const auto best = MeleeGeometry::contact(std::span{targets}.first(count), from, to, width,
                                             halfHeight, m_collision);
    if (best && endpointDistance != nullptr) {
        *endpointDistance = best->endpointDistance;
    }
    return best ? std::optional{targets[best->index].id} : std::nullopt;
}

std::optional<s32> Enemies::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<s32> best;
    f32 bestDistance = 0.0f;
    const Vec3 sweep = to - from;
    const f32 length = glm::length(sweep);
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if ((enemy.state != State::Active && enemy.state != State::Asleep) ||
            enemy.kind == kItKind) {
            continue;
        }
        const Vec3 centre = bodyCentre(enemy);
        const f32 t =
            length > 0.001f
                ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                : 0.0f;
        const Vec3 nearest = from + sweep * t;
        if (flatDistance(nearest, centre) <= radius + enemy.radius &&
            std::abs(nearest.y - centre.y) <= radius + enemy.reach) {
            const f32 distance = glm::length(nearest - from);
            if (!best.has_value() || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
    }
    return best;
}

std::vector<s32> Enemies::within(const Vec3& centre, f32 radius) const {
    std::vector<s32> out;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (enemy.state != State::Active && enemy.state != State::Asleep) {
            continue;
        }
        if (glm::length(bodyCentre(enemy) - centre) <= radius + enemy.radius) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<s32> Enemies::reachedBy(const Vec3& centre, f32 radius, f32 arc,
                                    const Vec3& facing) const {
    std::vector<s32> out;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        if (enemy.state != State::Active && enemy.state != State::Asleep) {
            continue;
        }
        const Vec3 toward = enemy.position - centre;
        const f32 distance = flatDistance(enemy.position, centre);
        if (distance > radius + enemy.radius || centre.y > enemy.position.y + enemy.height ||
            centre.y < enemy.position.y - enemy.height) {
            continue;
        }
        if (arc < kPi && distance > 0.001f) {
            const f32 angle = portable::acos(
                std::clamp((toward.x * facing.x + toward.z * facing.z) / distance, -1.0f, 1.0f));
            if (angle > arc) {
                continue;
            }
        }
        out.push_back(i);
    }
    return out;
}

// ---- looking -----------------------------------------------------------------------------

Vec3 Enemies::supportedPresentation(const WorldCollision* collision, const Vec3& from,
                                    const Vec3& to, f32 blend, EnemyAction action, bool active,
                                    const WorldCollision* fromFloor,
                                    const WorldCollision* toFloor) {
    Vec3 position = glm::mix(from, to, blend);
    if (blend <= 0 || blend >= 1 || collision == nullptr || !active ||
        action == EnemyAction::Start || action >= EnemyAction::HitReact1 ||
        action == EnemyAction::Fly || action == EnemyAction::Hover ||
        action == EnemyAction::Landing) {
        return position;
    }
    fromFloor = fromFloor != nullptr ? fromFloor : collision;
    toFloor = toFloor != nullptr ? toFloor : collision;
    if (from.y == to.y && fromFloor == collision && toFloor == collision) {
        return position;
    }
    const auto first = fromFloor->floorAt(from, 0.1f, 0.1f);
    const auto last = toFloor->floorAt(to, 0.1f, 0.1f);
    if (!first || !last || !collision->solid(first->object) || !collision->solid(last->object)) {
        return position;
    }
    // Prove endpoint contact at its own clock, then carry that contact into the
    // displayed geometry. A rotating platform can rise between equal-height
    // endpoints; a moving floor must not be tested at its stale load-time pose.
    const auto carriedHeight = [&](const WorldCollision& source, const FloorHit& floor,
                                   const Vec3& endpoint) {
        const auto old = source.objectTransform(floor.object);
        const auto shown = collision->objectTransform(floor.object);
        return old && shown ? (*shown * glm::inverse(*old) * Vec4{endpoint, 1}).y : endpoint.y;
    };
    const f32 ceiling =
        std::max(carriedHeight(*fromFloor, *first, from), carriedHeight(*toFloor, *last, to));
    // A straight presentation segment can cut through a descending ramp lip
    // (G2FLOOR#109). Lift only grounded artwork, bounded by its supported contacts.
    if (ceiling > position.y) {
        if (const auto floor = collision->floorAt(position, ceiling - position.y, 0)) {
            position.y = std::max(position.y, floor->y);
        }
    }
    return position;
}

void Enemies::capturePresentation(Enemy& enemy) {
    enemy.presentationPosition = enemy.position;
    enemy.presentationYaw = enemy.yaw;
    enemy.presentationState = enemy.state;
    enemy.animator.holdPresentation();
}

f32 Enemies::presentationBlend(const Enemy& enemy, f32 alpha) const {
    if (alpha < 0 || enemy.presentationState != enemy.state ||
        glm::distance(enemy.presentationPosition, enemy.position) > 2 * enemy.radius) {
        return 1;
    }
    // The swarm advances once per two simulation ticks. Keep its presentation
    // one native step behind rather than predicting future collisions/actions.
    return std::clamp((static_cast<f32>(m_simulation.pendingTicks) + alpha) * 0.5f, 0.0f, 1.0f);
}

std::optional<Enemies::Presentation> Enemies::presentation(s32 id) const {
    if (id < 0 || id >= m_simulation.most) {
        return std::nullopt;
    }
    const auto& enemy = m_simulation.enemies[static_cast<usize>(id)];
    if (enemy.state == State::Inactive || !enemy.animator.bound() ||
        (enemy.killed && (enemy.algorithm == kSuicideWay || enemy.kind == kItKind))) {
        return std::nullopt;
    }
    const auto* stock = stockOf(enemy.kind);
    if (stock == nullptr) {
        return std::nullopt;
    }
    const auto clock = [&](f32 blend) {
        return static_cast<f32>(stock->textures.frame()) +
               stock->textures.presentationOffset(blend).value_or(0);
    };
    return Presentation{enemy.instance,
                        enemy.kind,
                        enemy.tier,
                        enemy.variant,
                        enemy.presentationPosition,
                        enemy.position,
                        enemy.presentationYaw,
                        enemy.yaw,
                        presentationBlend(enemy, 0),
                        presentationBlend(enemy, 1),
                        clock(0),
                        clock(1),
                        enemy.animator.presentation()};
}

void Enemies::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                   const Texture* hitFlash, ItemArchive* weapons, const CameraFrame* camera,
                   f32 presentationAlpha, TreeModel::Pass pass) {
    // How the mechanics show the living swarm: a tint, and perhaps a rim marking each body out
    // the way the console's selection is.
    const SwarmLook look =
        m_rules.appearance != nullptr ? m_rules.appearance->look(m_simulation.frame) : SwarmLook{};
    std::optional<SelectionHighlight> swarmMark;
    if (look.marked) {
        swarmMark.emplace(camera != nullptr ? camera->position : Vec3{0.0f}, look.rim, look.wash,
                          look.rimWidth);
    }
    const Color tint = look.tint;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        // IT has already transferred its tag; it has no corpse to display in
        // the interval before the next simulation tick retires its pool slot.
        if (enemy.state == State::Inactive || !enemy.animator.bound() ||
            (enemy.killed && (enemy.algorithm == kSuicideWay || enemy.kind == kItKind))) {
            continue;
        }
        const Figure found = bodyOf(enemy);
        if (found.model == nullptr || found.tree == nullptr) {
            continue;
        }
        TreeModel& body = *found.model;
        const AnimationPlayer& player = enemy.animator.player();
        const f32 blend = presentationBlend(enemy, presentationAlpha);
        const f32 visualFrame =
            presentationAlpha >= 0 ? enemy.animator.presentationFrame(blend) : player.frame();
        const bool statue = enemy.kind == kDeathKind && enemy.state == State::Asleep;
        found.textures->apply(body, *found.tree, statue ? 0 : player.sequence(),
                              statue ? 0 : visualFrame,
                              found.textures->presentationOffset(presentationAlpha));
        const bool flashing = enemy.flashSeconds > 0;
        body.setAppearance((enemy.killed && enemy.kind != kDeathKind) || flashing,
                           flashing || enemy.kind == kDeathKind ? Color::white() : tint);
        if (flashing) {
            body.setMaskedTexture(hitFlash);
        }
        if (enemy.killed && !enemy.deathSkin.empty()) {
            ItemArchive* skins = enemy.deathSkin == "DEATHALT" ? archive(enemy.kind) : weapons;
            if (skins != nullptr) {
                for (const TextureAnimationInfo& animation : skins->trees.textureAnimations()) {
                    if (animation.name != enemy.deathSkin) {
                        continue;
                    }
                    const auto frame = static_cast<s32>(enemy.deathSeconds * kDeathSkinRate);
                    if (frame < enemy.deathSkinFrames && frame < animation.frames) {
                        const auto first =
                            animation.source >= 0
                                ? std::optional<u32>{static_cast<u32>(animation.source)}
                                : skins->textures.find(animation.frameName);
                        if (first.has_value() &&
                            *first + static_cast<u32>(frame) < skins->textures.size()) {
                            body.setMaskedTexture(
                                &skins->textures.texture(device, *first + static_cast<u32>(frame)),
                                TreeModel::SkinAlpha::Dissolve);
                        }
                    }
                    break;
                }
            }
        }
        // The flip-book kinds change their whole mesh with the frame; the rest are posed.
        const Vec3 position =
            supportedPresentation(m_collision, enemy.presentationPosition, enemy.position, blend,
                                  enemy.animator.action(), enemy.state == State::Active);
        const f32 yaw = blend == 1
                            ? enemy.yaw
                            : enemy.presentationYaw +
                                  TreePose::wrapAngle(enemy.yaw - enemy.presentationYaw) * blend;
        if (presentationAlpha >= 0) {
            body.setPresentationFrame(player.sequence(), enemy.animator.presentationFrame(blend));
        } else {
            body.setFrame(player.sequence(), static_cast<s32>(std::lround(player.frame())));
        }
        const Mat4 model = glm::scale(
            portable::rotate(glm::translate(Mat4{1.0f}, position), yaw, Vec3{0.0f, 1.0f, 0.0f}),
            Vec3{m_simulation.shrink * drawScale(enemy)});
        f32 alpha = enemy.kind == kDeathKind && enemy.killed
                        ? std::max(0.0f, 1.0f - enemy.deathSeconds / DeathRules::kFadeSeconds)
                        : 1.0f;
        alpha *= 1.0f - enemy.veil / kVeiled;
        if (alpha <= 0.0f) {
            continue; // gone from sight altogether
        }
        const SelectionHighlight* mark = i == m_highlightedId ? m_highlight : nullptr;
        if (mark == nullptr && swarmMark.has_value() && !enemy.killed && enemy.kind != kDeathKind) {
            mark = &*swarmMark;
        }
        body.setHighlight(mark);
        if (enemy.kind == kDeathKind && enemy.state == State::Asleep) {
            body.draw(device, clip, model, lighting, {}, camera, 1, pass);
        } else {
            TreePose pose;
            if (presentationAlpha >= 0) {
                enemy.animator.evaluatePresentation(pose, blend);
            }
            body.draw(device, clip, model, lighting,
                      presentationAlpha >= 0 ? pose.matrices() : enemy.animator.pose().matrices(),
                      camera, alpha, pass);
        }
        body.setHighlight(nullptr); // the body is its kind's, shared
    }
}

void Enemies::drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                          const WorldLighting& lighting, f32 presentationAlpha) const {
    for (s32 i = 0; i < m_simulation.most; ++i) {
        const Enemy& enemy = m_simulation.enemies[static_cast<usize>(i)];
        const Stock* stock = stockOf(enemy.kind);
        // It lies on the floor under the body, but not while the body rises (UpdateEnemy).
        if (enemy.state == State::Inactive || !enemy.animator.bound() || stock == nullptr ||
            enemy.animator.action() == EnemyAction::Start ||
            (enemy.killed && (enemy.algorithm == kSuicideWay || enemy.kind == kItKind))) {
            continue;
        }
        const auto tier = static_cast<usize>(std::clamp(enemy.tier, 1, 3) - 1);
        Vec3 ground = supportedPresentation(m_collision, enemy.presentationPosition, enemy.position,
                                            presentationBlend(enemy, presentationAlpha),
                                            enemy.animator.action(), enemy.state == State::Active);
        Vec3 normal{0.0f, 1.0f, 0.0f};
        if (m_collision != nullptr) {
            if (const auto floor = m_collision->floorAt(ground, kShadowReach, kShadowReach)) {
                ground.y = floor->y;
                normal = floor->normal;
            }
        }
        const f32 shown = 1.0f - enemy.veil / kVeiled;
        if (shown > 0.0f) {
            stock->shadows[tier].draw(device, clip, eye, ground, normal, lighting, shown,
                                      m_simulation.shrink * drawScale(enemy));
        }
    }
}

bool Enemies::alive(s32 id) const {
    return id >= 0 && id < m_simulation.most &&
           !m_simulation.enemies[static_cast<usize>(id)].killed &&
           (m_simulation.enemies[static_cast<usize>(id)].state == State::Active ||
            m_simulation.enemies[static_cast<usize>(id)].state == State::Asleep);
}

bool Enemies::dying(s32 id) const {
    return id >= 0 && id < m_simulation.most &&
           m_simulation.enemies[static_cast<usize>(id)].state == State::Dying;
}

std::optional<Enemies::Observation> Enemies::observe(s32 id) const {
    if (id < 0 || id >= m_simulation.most) {
        return std::nullopt;
    }
    const auto& enemy = m_simulation.enemies[static_cast<usize>(id)];
    if (enemy.state == State::Inactive) {
        return std::nullopt;
    }
    Observation result{enemy.instance, enemy.health, enemy.fullHealth, enemy.state == State::Asleep,
                       enemy.flashSeconds > 0};
    result.scale = m_simulation.shrink * drawScale(enemy);
    result.alpha = opacityOf(id);
    if (enemy.killed && (enemy.algorithm == kSuicideWay || enemy.kind == kItKind)) {
        result.alpha = 0; // the explosion or transferred tag replaces the body immediately
    } else if (enemy.killed && enemy.kind == kDeathKind) {
        result.alpha *= std::max(0.0f, 1.0f - enemy.deathSeconds / DeathRules::kFadeSeconds);
    }
    // An array iterator is a pointer only on some libraries:
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto skin = std::ranges::find(kDeathSkins, enemy.deathSkin);
    const auto frame = static_cast<u32>(enemy.deathSeconds * kDeathSkinRate);
    if (enemy.killed && skin != kDeathSkins.end() &&
        frame < static_cast<u32>(std::max(0, enemy.deathSkinFrames))) {
        result.deathSkin = static_cast<u32>(skin - kDeathSkins.begin()) + 1;
        result.deathFrame = frame;
    }
    return result;
}

usize Enemies::count() const {
    usize n = 0;
    for (s32 i = 0; i < m_simulation.most; ++i) {
        n += m_simulation.enemies[static_cast<usize>(i)].state != State::Inactive ? 1 : 0;
    }
    return n;
}

s32 Enemies::kindOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].kind;
}
s32 Enemies::tierOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].tier;
}
bool Enemies::bred(s32 id) const {
    return id >= 0 && id < m_simulation.most && m_simulation.enemies[static_cast<usize>(id)].bred;
}

s32 Enemies::generatorOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].generator;
}
f32 Enemies::healthOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].health;
}
const Vec3& Enemies::positionOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].position;
}
s32 Enemies::floorOf(s32 id) const {
    if (id < 0 || id >= m_simulation.most) {
        return -1;
    }
    const auto& enemy = m_simulation.enemies[static_cast<usize>(id)];
    return enemy.state != State::Inactive && enemy.floor ? enemy.floor->object : -1;
}
f32 Enemies::yawOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].yaw;
}
f32 Enemies::radiusOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].radius;
}
f32 Enemies::heightOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].height;
}
s32 Enemies::targetOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].target;
}
s32 Enemies::algorithmOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].algorithm;
}
s32 Enemies::pushCountOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].pushes;
}

f32 Enemies::sightOf(s32 id) const {
    return alive(id) ? m_simulation.enemies[static_cast<usize>(id)].sight : 0.0f;
}

s32 Enemies::stunTicksOf(s32 id) const {
    return alive(id) ? std::max(0, m_simulation.enemies[static_cast<usize>(id)].stunTicks) : 0;
}

s32 Enemies::variantOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].variant;
}
f32 Enemies::drawScale(const BodyState& body) const {
    return m_rules.appearance != nullptr ? m_rules.appearance->bodyScale(body.affinity) : 1.0f;
}
s32 Enemies::affinityOf(s32 id) const {
    return m_simulation.enemies[static_cast<usize>(id)].affinity;
}
u64 Enemies::instanceOf(s32 id) const {
    return id >= 0 && id < m_simulation.most ? m_simulation.enemies[static_cast<usize>(id)].instance
                                             : 0;
}

const EnemyAnimator* Enemies::animatorOf(s32 id) const {
    if (id < 0 || id >= m_simulation.most ||
        m_simulation.enemies[static_cast<usize>(id)].state == State::Inactive) {
        return nullptr;
    }
    return &m_simulation.enemies[static_cast<usize>(id)].animator;
}

} // namespace gdl::game
