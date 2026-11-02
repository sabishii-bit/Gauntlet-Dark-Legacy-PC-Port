#include "game/enemies/EnemyMissiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/combat/DamageTypes.h"
#include "game/critters/CombatantProjectile.h"
#include "game/effects/DynamicLights.h"
#include "game/players/EnemyShrink.h"

namespace gdl::game {

namespace {

constexpr f32 kFootClearance = 0.1f;
constexpr f32 kSimulationStep = 1.0f / 30.0f;
constexpr f32 kLeastSpatialStep = 0.05f;
constexpr f32 kFlatEnough = 0.001f;
constexpr f32 kShotHeight = 2.5f; ///< over the body's middle, for the arrow and the lob
constexpr f32 kSightProbe = 0.1f; ///< the width of the look a blast takes at a player
constexpr f32 kHeldFrom = 2.0f;   ///< a blast's hit past this holds its victim off

// The kinds with a launch point of their own (EnemyStartMissile's jump table).
constexpr s32 kGrunt = 4;
constexpr s32 kSorcerer = 7;
constexpr s32 kZombie = 13;
constexpr s32 kPlague = 14;
constexpr s32 kWorm = 17;
constexpr s32 kImp = 23;
constexpr s32 kWarlock = 24;
constexpr s32 kGarm = 27;

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool checkpointNumber(f32 value, f32 least = -1'000'000, f32 most = 1'000'000) {
    return std::isfinite(value) && value >= least && value <= most;
}

bool checkpointPoint(const Vec3& value) {
    return checkpointNumber(value.x) && checkpointNumber(value.y) && checkpointNumber(value.z);
}

bool checkpointId(s32 value, s32 least = -1) {
    return value >= least && value <= 1'000'000;
}

bool checkpointPlayer(s32 value, s32 least = -1) {
    return value >= least && value < 4;
}

bool checkpointKind(const EnemyMissileKind& kind) {
    return kind.slot >= EnemyMissileKind::kArrow && kind.slot <= EnemyMissileKind::kDeathShot &&
           checkpointNumber(kind.damage, 0) && checkpointNumber(kind.speed, 0, 10000) &&
           checkpointNumber(kind.radius, 0, 10000) &&
           checkpointNumber(kind.burstRadius, 0, 10000) && checkpointPoint(kind.spin) &&
           checkpointNumber(kind.weight) && checkpointNumber(kind.held, 0, 3600);
}

bool checkpointResources(std::span<const EnemyMissiles::Resource> resources) {
    if (resources.size() > EnemyMissiles::Checkpoint::kMaxResources) {
        return false;
    }
    for (usize index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (resource.id == 0 || resource.model == nullptr || !resource.model->bound()) {
            return false;
        }
        for (usize previous = 0; previous < index; ++previous) {
            if (resources[previous].id == resource.id ||
                resources[previous].model == resource.model) {
                return false;
            }
        }
    }
    return true;
}

bool checkpointCount(usize amount, usize& used, usize most) {
    if (amount > most - used) {
        return false;
    }
    used += amount;
    return true;
}

} // namespace

bool EnemyMissiles::Checkpoint::valid() const {
    if (!random.valid() || missiles.size() > kMaxMissiles || bursts.size() > kMaxBursts ||
        !checkpointNumber(shrink, 0.001f, 16) || !checkpointNumber(ricochetIn, 0, kRicochetGap)) {
        return false;
    }
    usize events = 0;
    if (!checkpointCount(hits.size(), events, kMaxEvents) ||
        !checkpointCount(rockHits.size(), events, kMaxEvents) ||
        !checkpointCount(pickupBlasts.size(), events, kMaxEvents)) {
        return false;
    }
    usize contacts = 0;
    usize stages = 0;
    std::unordered_set<u64> instances;
    for (const auto& missile : missiles) {
        if (missile.instance == 0 || (nextInstance != 0 && missile.instance >= nextInstance) ||
            !instances.insert(missile.instance).second || missile.continuity == 0 ||
            !checkpointPoint(missile.position) || !checkpointPoint(missile.velocity) ||
            !checkpointPoint(missile.turned) || !checkpointKind(missile.kind) ||
            missile.shooter < -1 || missile.shooter >= Enemies::kMost ||
            !checkpointNumber(missile.secondsLeft, 0, kLife + missile.kind.held) ||
            missile.secondsLeft == 0 || !checkpointNumber(missile.lived, 0, 3604) ||
            !checkpointNumber(missile.heldLeft, 0, missile.kind.held) ||
            !checkpointNumber(missile.scale, 0.001f, 16) || missile.pierced.size() > 4 ||
            !checkpointCount(missile.pierced.size(), contacts, kMaxContacts)) {
            return false;
        }
        std::array<bool, 4> pierced{};
        for (const auto& [player, seconds] : missile.pierced) {
            if (!checkpointPlayer(player, 0) || !checkpointNumber(seconds, 0, kPierceGap) ||
                seconds == 0 || pierced[static_cast<usize>(player)]) {
                return false;
            }
            pierced[static_cast<usize>(player)] = true;
        }
    }
    for (const auto& burst : bursts) {
        const auto& blast = burst.blast;
        if ((burst.instance != 0 && ((nextInstance != 0 && burst.instance >= nextInstance) ||
                                     !instances.insert(burst.instance).second)) ||
            blast.stages.empty() || burst.stage >= blast.stages.size() ||
            !checkpointCount(blast.stages.size(), stages, kMaxStages) ||
            !checkpointPoint(blast.position) || !checkpointNumber(blast.radius, 0, 10000) ||
            !checkpointNumber(blast.damage, 0) || !checkpointPlayer(blast.spared) ||
            !checkpointNumber(burst.stageLeft, 0, blast.stages[burst.stage]) ||
            burst.stageLeft == 0 || burst.players.size() > 4 ||
            !checkpointCount(burst.players.size(), contacts, kMaxContacts) ||
            !checkpointCount(burst.swarm.size(), contacts, kMaxContacts) ||
            !checkpointCount(burst.scenery.size(), contacts, kMaxContacts)) {
            return false;
        }
        for (const f32 seconds : blast.stages) {
            if (!checkpointNumber(seconds, 0, 3600) || seconds == 0) {
                return false;
            }
        }
        std::array<bool, 4> players{};
        for (const auto& held : burst.players) {
            if (!checkpointPlayer(held.id, 0) || players[static_cast<usize>(held.id)]) {
                return false;
            }
            const bool spared = held.id == blast.spared;
            if (spared ? held.secondsLeft != std::numeric_limits<f32>::infinity()
                       : !checkpointNumber(held.secondsLeft, 0, 3601) || held.secondsLeft == 0) {
                return false;
            }
            players[static_cast<usize>(held.id)] = true;
        }
        if (blast.spared >= 0 && !players[static_cast<usize>(blast.spared)]) {
            return false;
        }
        std::unordered_set<s32> swarm;
        for (const auto& held : burst.swarm) {
            if (!checkpointId(held.id, 0) || !checkpointNumber(held.secondsLeft, 0, 3601) ||
                held.secondsLeft == 0 || !swarm.insert(held.id).second) {
                return false;
            }
        }
        for (const s32 id : burst.scenery) {
            if (!checkpointId(id, 0)) {
                return false;
            }
        }
    }
    for (const auto& hit : hits) {
        if ((hit.instance != 0 && nextInstance != 0 && hit.instance >= nextInstance) ||
            !checkpointPlayer(hit.player) || hit.shooter < -1 || hit.shooter >= Enemies::kMost ||
            !checkpointNumber(hit.damage, 0) || !checkpointNumber(hit.burstRadius, 0, 10000) ||
            !checkpointPoint(hit.position) || !checkpointPoint(hit.direction) ||
            !checkpointId(hit.target) || !checkpointId(hit.worldObject)) {
            return false;
        }
    }
    for (const auto& hit : rockHits) {
        if (hit.rock > 1'000'000 || !checkpointNumber(hit.damage, 0)) {
            return false;
        }
    }
    return std::ranges::all_of(pickupBlasts, [](const auto& reach) {
        return checkpointPoint(reach.position) && checkpointNumber(reach.radius, 0, 10000) &&
               checkpointNumber(reach.damage, 0);
    });
}

struct EnemyMissiles::Prepared::Data {
    const EnemyMissiles* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    Checkpoint values;
    std::vector<EnemyMissile> missiles;
};

EnemyMissiles::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
EnemyMissiles::Prepared::Prepared(Prepared&&) noexcept = default;
EnemyMissiles::Prepared& EnemyMissiles::Prepared::operator=(Prepared&&) noexcept = default;
EnemyMissiles::Prepared::~Prepared() = default;

std::optional<EnemyMissiles::Checkpoint>
EnemyMissiles::checkpoint(std::span<const Resource> resources) const {
    if (!checkpointResources(resources) || m_sim.missiles.size() > Checkpoint::kMaxMissiles ||
        m_sim.bursts.size() > Checkpoint::kMaxBursts) {
        return std::nullopt;
    }
    usize contacts = 0;
    usize stages = 0;
    usize events = 0;
    if (!checkpointCount(m_sim.hits.size(), events, Checkpoint::kMaxEvents) ||
        !checkpointCount(m_sim.rockHits.size(), events, Checkpoint::kMaxEvents) ||
        !checkpointCount(m_sim.pickupBlasts.size(), events, Checkpoint::kMaxEvents)) {
        return std::nullopt;
    }
    for (const auto& missile : m_sim.missiles) {
        if (!checkpointCount(missile.pierced.size(), contacts, Checkpoint::kMaxContacts)) {
            return std::nullopt;
        }
    }
    for (const auto& burst : m_sim.bursts) {
        if (!checkpointCount(burst.blast.stages.size(), stages, Checkpoint::kMaxStages) ||
            !checkpointCount(burst.players.size(), contacts, Checkpoint::kMaxContacts) ||
            !checkpointCount(burst.swarm.size(), contacts, Checkpoint::kMaxContacts) ||
            !checkpointCount(burst.scenery.size(), contacts, Checkpoint::kMaxContacts)) {
            return std::nullopt;
        }
    }
    Checkpoint state;
    state.missiles.reserve(m_sim.missiles.size());
    for (const auto& missile : m_sim.missiles) {
        u32 resource = 0;
        if (missile.model != nullptr) {
            const auto found = std::ranges::find(resources, missile.model, &Resource::model);
            if (found == resources.end()) {
                return std::nullopt;
            }
            resource = found->id;
        }
        state.missiles.push_back(
            {missile.instance, missile.continuity, missile.position, missile.velocity,
             missile.turned, missile.kind, resource, missile.shooter, missile.secondsLeft,
             missile.lived, missile.heldLeft, missile.scale, missile.reflected, missile.pierced});
    }
    state.bursts = m_sim.bursts;
    state.hits = m_sim.hits;
    state.rockHits = m_sim.rockHits;
    state.pickupBlasts = m_sim.pickupBlasts;
    state.nextInstance = m_sim.nextInstance;
    state.random = m_sim.random.checkpoint();
    state.shrink = m_sim.shrink;
    state.ricochetIn = m_sim.ricochetIn;
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<EnemyMissiles::Prepared>
EnemyMissiles::prepareRestore(const Checkpoint& state, std::span<const Resource> resources) const {
    if (m_restoreGeneration == 0 || !state.valid() || !checkpointResources(resources)) {
        return std::nullopt;
    }
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->owner = this;
    prepared->lifetime = m_restoreOwner;
    prepared->generation = m_restoreGeneration;
    prepared->values = state;
    prepared->missiles.reserve(state.missiles.size());
    for (const auto& flight : state.missiles) {
        const TreeModel* model = nullptr;
        if (flight.resource != 0) {
            const auto found = std::ranges::find(resources, flight.resource, &Resource::id);
            if (found == resources.end()) {
                return std::nullopt;
            }
            model = found->model;
        }
        prepared->missiles.push_back(
            {flight.instance, flight.continuity, flight.position, flight.velocity, flight.turned,
             flight.kind, model, flight.shooter, flight.secondsLeft, flight.lived, flight.heldLeft,
             flight.scale, flight.reflected, flight.pierced});
    }
    return Prepared{std::move(prepared)};
}

bool EnemyMissiles::canCommit(const Prepared& state) const noexcept {
    return state.m_data != nullptr && state.m_data->owner == this &&
           state.m_data->lifetime == m_restoreOwner && m_restoreGeneration != 0 &&
           state.m_data->generation == m_restoreGeneration &&
           std::ranges::all_of(state.m_data->missiles, [](const auto& missile) {
               return missile.model == nullptr || missile.model->bound();
           });
}

bool EnemyMissiles::commitRestore(Prepared&& state) noexcept {
    if (!canCommit(state)) {
        return false;
    }
    auto prepared = std::move(state.m_data);
    m_sim.missiles.swap(prepared->missiles);
    m_sim.bursts.swap(prepared->values.bursts);
    m_sim.hits.swap(prepared->values.hits);
    m_sim.rockHits.swap(prepared->values.rockHits);
    m_sim.pickupBlasts.swap(prepared->values.pickupBlasts);
    m_sim.nextInstance = prepared->values.nextInstance;
    m_sim.random.restore(prepared->values.random);
    m_sim.shrink = prepared->values.shrink;
    m_sim.ricochetIn = prepared->values.ricochetIn;
    ++m_restoreGeneration;
    ++m_localEpoch;
    return true;
}

void EnemyMissiles::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    saved.m_owner = m_restoreOwner;
    saved.m_epoch = m_localEpoch;
}
bool EnemyMissiles::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_restoreOwner && saved.m_epoch == m_localEpoch;
}
bool EnemyMissiles::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    return canRestoreLocal(a) && canRestoreLocal(b) && a.m_values == b.m_values;
}
bool EnemyMissiles::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("EnemyMissiles.native.1");
    digest.value(m_sim);
    return digest.good();
}
bool EnemyMissiles::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    digest.string("EnemyMissiles.native.1");
    digest.value(saved.m_values);
    return digest.good();
}
bool EnemyMissiles::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    ++m_restoreGeneration;
    return true;
}

bool EnemyMissiles::restore(const Checkpoint& state, std::span<const Resource> resources) {
    auto prepared = prepareRestore(state, resources);
    return prepared && commitRestore(std::move(*prepared));
}

bool EnemyMissiles::walled(const WorldCollision& collision, const Vec3& from, const Vec3& to,
                           f32 radius) {
    const f32 length = glm::distance(from, to);
    const f32 stride = std::max(radius * 0.5f, kLeastSpatialStep);
    const auto steps = std::max(1, static_cast<s32>(std::ceil(length / stride)));
    for (s32 i = 1; i <= steps; ++i) {
        const Vec3 at = glm::mix(from, to, static_cast<f32>(i) / static_cast<f32>(steps));
        if (glm::distance(collision.resolveWalls(at, radius, at.y - radius, at.y + radius), at) >
            kFlatEnough) {
            return true;
        }
    }
    return false;
}

std::string_view EnemyMissileHit::effect() const {
    if (burstRadius > 0) {
        return "EXPSMALL";
    }
    constexpr std::array<std::string_view, 5> kWorldHits{"SPARKS", "FIREHIT", "HITCOL", "HITCOL",
                                                         "HITCOL"};
    const u32 element = flags & damage::kElementMask;
    return worldContact && element < kWorldHits.size() ? kWorldHits[element] : std::string_view{};
}

std::string_view EnemyMissileHit::sound() const {
    if (liquid) {
        return "S_SPLASH";
    }
    // Ordinary enemy bolts have no wall sound; the lobber's bomb supplies its own.
    return burstRadius > 0 ? "S_LOBBER_BOMB" : std::string_view{};
}

EnemyMissileKind EnemyMissileKind::arrow() {
    EnemyMissileKind kind;
    kind.slot = kArrow;
    kind.damage = 10.0f;
    kind.speed = 25.0f;
    kind.radius = 0.5f;
    kind.weight = 30.0f;
    return kind;
}

EnemyMissileKind EnemyMissileKind::bomb() {
    EnemyMissileKind kind;
    kind.slot = kBomb;
    kind.flags = kKnockBack;
    kind.damage = 10.0f;
    kind.speed = 20.0f;
    kind.radius = 0.2f;
    kind.burstRadius = 3.0f;
    kind.spin = Vec3{0.0f, 1.0f, 0.0f};
    kind.weight = 35.0f;
    return kind;
}

EnemyMissileKind EnemyMissileKind::bolt(f32 damage, f32 speed, f32 radius, u32 flags, f32 weight) {
    EnemyMissileKind kind;
    kind.slot = kBolt;
    kind.flags = flags;
    kind.damage = damage;
    kind.speed = speed;
    kind.radius = radius;
    kind.weight = weight;
    return kind;
}

EnemyMissileKind EnemyMissileKind::deathShot() {
    constexpr f32 kDamage = 50.0f; ///< 0x803480FC
    constexpr f32 kSpeed = 20.0f;  ///< 0x803480F0
    constexpr f32 kRadius = 3.0f;  ///< 0x803480F8
    EnemyMissileKind kind;
    kind.slot = kDeathShot;
    kind.flags = kPierces | kKnockDown; // SetEnemyDeathParams' 0x100020
    kind.damage = kDamage;
    kind.speed = kSpeed;
    kind.radius = kRadius;
    kind.weight = 0.0f;
    kind.throughWorld = true;
    return kind;
}

u32 EnemyMissileKind::hitFlags() const {
    switch (slot) {
    case kArrow: return flags | kArrowHit;
    case kBolt: return flags | kBoltHit;
    default: return flags;
    }
}

std::optional<EnemyMissileKind> enemyMissileOf(s32 kind, s32 slot) {
    // The original's table (0x80119128): the medium kinds share the arrow and the bomb, and
    // a few have a bolt of their own; the worm has three of its own.
    const bool medium = kind == 1 || kind == 4 || kind == 7 || kind == 10 || kind == 13 ||
                        kind == 14 || kind == 16 || kind == 19 || kind == 23 || kind == 24;
    if (kind == kWorm) {
        std::optional<EnemyMissileKind> worm;
        switch (slot) {
        case 0: worm = EnemyMissileKind::bolt(5.0f, 15.0f, 0.3f); break;
        case 1: worm = EnemyMissileKind::bolt(10.0f, 20.0f, 0.3f); break;
        case 2: worm = EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f); break;
        default: return std::nullopt;
        }
        worm->slot = slot;
        return worm;
    }
    if (slot == EnemyMissileKind::kArrow && medium) {
        return EnemyMissileKind::arrow();
    }
    if (slot == EnemyMissileKind::kBomb && medium) {
        return EnemyMissileKind::bomb();
    }
    if (slot == EnemyMissileKind::kBolt) {
        switch (kind) {
        case 2:  // the demon
        case 20: // the ghost
            return EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f, damage::kFire);
        case kPlague: return EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f);
        case kSorcerer:
        case kWarlock: return EnemyMissileKind::bolt(20.0f, 20.0f, 0.2f, damage::kLight);
        case kGarm:
            return EnemyMissileKind::bolt(25.0f, 80.0f, 2.0f,
                                          damage::kElectric | EnemyMissileKind::kPierces, 0.0f);
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

s32 missileSlotOfWay(s32 way) {
    switch (way) {
    case 16:
    case 23: return EnemyMissileKind::kArrow;
    case 17:
    case 26: return EnemyMissileKind::kBomb;
    default: return EnemyMissileKind::kBolt;
    }
}

EnemyLaunchPoint enemyLaunchPointOf(s32 kind, s32 slot) {
    EnemyLaunchPoint point;
    point.height = slot == EnemyMissileKind::kBolt ? 0.0f : kShotHeight;
    switch (kind) {
    case kGrunt:
        if (slot == EnemyMissileKind::kArrow) {
            point.height = 1.5f;
        }
        break;
    case kZombie:
        if (slot == EnemyMissileKind::kArrow) {
            point.height = 1.0f;
        }
        break;
    case kSorcerer:
    case kWarlock:
        if (slot == EnemyMissileKind::kBolt) {
            point.height = 1.5f;
        }
        break;
    case kPlague:
        if (slot == EnemyMissileKind::kBolt) {
            point.height = 1.0f;
        } else if (slot == EnemyMissileKind::kArrow) {
            point.height = 1.5f;
        }
        break;
    case kWorm:
        point.height = 2.0f;
        point.shift = 2.0f;
        break;
    case kImp:
        if (slot == EnemyMissileKind::kArrow) {
            point.height = 0.0f;
        } else if (slot == EnemyMissileKind::kBomb) {
            point.height = 0.0f;
            point.shift = -2.5f;
        }
        break;
    case kGarm: point.height = 0.0f; break;
    default: break;
    }
    return point;
}

Vec3 EnemyMissiles::lobVelocity(const Vec3& from, const Vec3& to, f32 speed) {
    const f32 across = flatDistance(from, to);
    const f32 flight = std::max(across / std::max(speed, 0.001f), kLeastFlight);
    Vec3 velocity{(to.x - from.x) / flight, 0.0f, (to.z - from.z) / flight};
    velocity.y = (to.y - from.y) / flight + 0.5f * kGravity * flight;
    return velocity;
}

Vec3 EnemyMissiles::heading(const EnemyMissileKind& kind, const Vec3& from, const Vec3& to,
                            f32 speed, f32 error) {
    Vec3 way = to - from;
    if (kind.slot == EnemyMissileKind::kBolt) {
        const f32 length = glm::length(way);
        way = length > 0.0f ? way / length : Vec3{0.0f};
    } else {
        // A fixed pace along the ground, rising so as to fall on the aim (taken under the
        // target's middle, and off by the error) under the missile's own weight.
        const f32 drop = kind.slot == EnemyMissileKind::kBomb ? kBombDrop : kArrowDrop;
        const f32 across = portable::hypot(way.x, way.z);
        const f32 inverse = across > kFlatEnough ? 1.0f / across : 1.0f;
        const f32 perSpeed = 1.0f / std::max(speed, kFlatEnough);
        const f32 rise = way.y;
        way.x *= inverse;
        way.z *= inverse;
        way.y = 0.5f * kind.weight * across * perSpeed * perSpeed + (rise + error + drop) * inverse;
    }
    way.y = std::max(way.y, 0.0f); // never thrown down
    return way;
}

EnemyMissileKind EnemyMissiles::thrown(const EnemyMissileKind& kind) const {
    // Only what EnemyStartMissile throws is thrown small; a death shot is its own size.
    if (kind.slot == EnemyMissileKind::kDeathShot) {
        return kind;
    }
    EnemyMissileKind out = kind;
    out.damage = EnemyShrink::harmDealt(m_sim.shrink, kind.damage);
    return out;
}

bool EnemyMissiles::launch(const EnemyMissileKind& kind, const EnemyMissileLaunch& launch,
                           const WorldCollision* collision, std::span<const Obstacle> items) {
    if (m_sim.nextInstance == 0) {
        return false;
    }
    const f32 speed = kind.speed * std::max(launch.speedScale, 0.01f);
    // Pin the distribution as well as the raw stream across standard libraries.
    // Consume one word even for zero error or a launch subsequently rejected.
    const f32 spread = m_sim.random.unitFloat24() * kAimSpread;
    const f32 error = launch.aimError * (-0.5f * kAimSpread + spread);
    const Vec3 way = heading(kind, launch.body, launch.target, speed, error);
    const Vec2 flat{way.x, way.z};
    const Vec2 facing{portable::sin(launch.facing), portable::cos(launch.facing)};
    if (glm::length(flat) <= 0.0f || glm::dot(glm::normalize(flat), facing) < kFacing) {
        return false;
    }
    Vec3 start = launch.body + Vec3{0.0f, launch.point.height, 0.0f};
    start += Vec3{way.x, 0.0f, way.z} * launch.point.shift;
    const Vec3 from = start + way * kLead;
    const f32 radius = std::max(kind.radius, 0.01f);
    if (collision != nullptr && walled(*collision, start, from, radius)) {
        return false;
    }
    if (std::ranges::any_of(items, [&](const Obstacle& item) {
            return item.solid && item.blocksSegment(start, from, radius);
        })) {
        return false;
    }
    EnemyMissile missile;
    missile.instance = m_sim.nextInstance++;
    missile.kind = thrown(kind);
    missile.scale = m_sim.shrink;
    missile.position = from;
    missile.velocity = way * speed;
    missile.model = launch.model;
    missile.shooter = launch.shooter;
    missile.secondsLeft = kLife + kind.held;
    missile.heldLeft = kind.held;
    m_sim.missiles.push_back(missile);
    return true;
}

void EnemyMissiles::launch(const EnemyMissileKind& kind, const Vec3& from, const Vec3& aim,
                           f32 speedScale, const TreeModel* model, s32 shooter) {
    if (m_sim.nextInstance == 0) {
        return;
    }
    EnemyMissile missile;
    missile.instance = m_sim.nextInstance++;
    missile.kind = thrown(kind);
    missile.scale = m_sim.shrink;
    missile.position = from;
    missile.model = model;
    missile.shooter = shooter;
    missile.secondsLeft = kLife + kind.held;
    missile.heldLeft = kind.held;
    const f32 speed = kind.speed * std::max(speedScale, 0.01f);
    if (kind.slot != EnemyMissileKind::kBolt && kind.weight > 0.0f) {
        // Exactly onto the aim under its own weight.
        const f32 flight =
            std::max(flatDistance(from, aim) / std::max(speed, 0.001f), kLeastFlight);
        missile.velocity = Vec3{(aim.x - from.x) / flight, 0.0f, (aim.z - from.z) / flight};
        missile.velocity.y = (aim.y - from.y) / flight + 0.5f * kind.weight * flight;
    } else {
        const Vec3 way = aim - from;
        const f32 length = glm::length(way);
        missile.velocity = length > 0.001f ? way * (speed / length) : Vec3{0.0f, 0.0f, speed};
    }
    m_sim.missiles.push_back(missile);
}

void EnemyMissiles::burst(const Vec3& position, f32 radius, f32 damage, u32 flags, f32 seconds,
                          s32 spared) {
    EnemyBlast lob;
    lob.position = position;
    lob.radius = radius;
    lob.damage = damage;
    lob.flags = flags;
    lob.stages = {seconds};
    lob.spared = spared;
    lob.lit = true;
    blast(std::move(lob));
}

void EnemyMissiles::blast(EnemyBlast blast) {
    std::erase_if(blast.stages, [](f32 stage) { return stage <= 0.0f; });
    if (blast.stages.empty() || m_sim.nextInstance == 0) {
        return;
    }
    Burst burst;
    burst.instance = m_sim.nextInstance++;
    burst.stageLeft = blast.stages.front();
    if (blast.spared >= 0) {
        burst.players.push_back({blast.spared, std::numeric_limits<f32>::infinity()});
    }
    burst.blast = std::move(blast);
    m_sim.bursts.push_back(std::move(burst));
}

/** Each stage the blast grows to its radius as its harm fades, and is harmless over its last
 * third (ProcessEffects' mode 1). A player it reaches is pushed a little away and left alone
 * until the stage is out (gas every half second); past ten, a wall between shelters them.
 * One of the swarm is left alone for the rest of the stage, and a second at least. */
void EnemyMissiles::stepBursts(f32 seconds, const WorldCollision* collision,
                               std::span<const EnemyView> players,
                               std::span<const MissileTarget> swarm, const SceneryBlast& scenery) {
    const auto held = [](std::vector<Held>& holds, s32 id) {
        return std::ranges::any_of(holds, [id](const Held& hold) { return hold.id == id; });
    };
    for (Burst& burst : m_sim.bursts) {
        for (std::vector<Held>* holds : {&burst.players, &burst.swarm}) {
            for (Held& hold : *holds) {
                hold.secondsLeft -= seconds;
            }
            std::erase_if(*holds, [](const Held& hold) { return hold.secondsLeft <= 0.0f; });
        }
        const EnemyBlast& blast = burst.blast;
        const f32 stage = blast.stages[burst.stage];
        const f32 phase = burst.stageLeft / stage;
        burst.stageLeft -= seconds;
        if (burst.stageLeft <= 0.0f) {
            ++burst.stage;
            burst.stageLeft = burst.stage < blast.stages.size() ? blast.stages[burst.stage] : 0.0f;
        }
        if (phase <= kBurstFade) {
            continue;
        }
        const f32 radius = blast.radius * (kBurstFade + (1.0f - phase));
        const f32 damage = blast.damage * kBurstGrowth * (phase - kBurstFade);
        const f32 remaining = phase * stage;
        const PickupBlastReach reach{blast.position, radius, damage, blast.flags};
        m_sim.pickupBlasts.push_back(reach);
        if (scenery) {
            scenery(reach, burst.scenery);
        }
        u32 flags = blast.flags;
        if (damage < kBurstKnockFrom) {
            flags &= ~damage::kKnocks;
        }
        const auto push = [](const Vec3& away) {
            const Vec2 flat{away.x, away.z};
            const Vec2 out = glm::length(flat) > 0.0f ? glm::normalize(flat) : Vec2{0.0f};
            return Vec3{out.x, 0.0f, out.y};
        };
        for (const EnemyView& view : players) {
            if (view.hidden || held(burst.players, view.player)) {
                continue;
            }
            const Vec3 middle = view.position + Vec3{0.0f, 0.5f * view.height, 0.0f};
            const Vec3 away = middle - blast.position;
            const f32 across = portable::hypot(away.x, away.z);
            if (std::abs(away.y) > 0.5f * view.height + radius || across > radius + view.radius) {
                continue;
            }
            if (across > kSightFrom && collision != nullptr &&
                walled(*collision, blast.position, middle, kSightProbe)) {
                continue;
            }
            f32 hold = 0.0f;
            if ((blast.flags & EnemyBlast::kGas) != 0) {
                hold = kGasGap;
            } else if (damage > kHeldFrom) {
                hold = remaining + kBlastSlack;
            }
            if (hold > 0.0f) {
                burst.players.push_back({view.player, hold});
            }
            EnemyMissileHit hit;
            hit.fromBurst = true;
            hit.instance = burst.instance;
            hit.player = view.player;
            hit.damage = damage;
            hit.flags = flags;
            hit.position = blast.position;
            hit.direction = push(away) * kBurstPush;
            m_sim.hits.push_back(hit);
        }
        for (const MissileTarget& body : swarm) {
            const Vec3 away = body.base - blast.position;
            if (held(burst.swarm, body.id) ||
                portable::hypot(away.x, away.z) > radius + std::max(body.radius, 0.0f)) {
                continue;
            }
            burst.swarm.push_back({body.id, std::max(kSwarmGap, remaining + kBlastSlack)});
            EnemyMissileHit hit;
            hit.fromBurst = true;
            hit.instance = burst.instance;
            hit.target = body.id;
            hit.damage = damage;
            hit.flags = flags;
            hit.position = blast.position;
            hit.direction = push(away);
            m_sim.hits.push_back(hit);
        }
    }
    std::erase_if(m_sim.bursts,
                  [](const Burst& burst) { return burst.stage >= burst.blast.stages.size(); });
}

/** Whether one of the level's `items` stops `missile` between `from` and `to`, a safe rock
 * taking its blow (fn_8005ED44, SfxSkipItem, fn_8005C1DC): the rest stop it unharmed, and a
 * piercing bolt goes through all but a rock left standing (ProcessEffects). */
bool EnemyMissiles::stopped(const EnemyMissile& missile, std::span<const MissileStop> items,
                            const Vec3& from, const Vec3& to, f32 radius) {
    const bool pierces = missile.kind.pierces();
    for (const MissileStop& item : items) {
        if (!item.box.solid || !item.box.blocksSegment(from, to, radius)) {
            continue;
        }
        if (item.rock < 0) {
            if (pierces) {
                continue;
            }
            return true;
        }
        // What this update has already dealt it counts against what it has.
        f32 dealt = 0.0f;
        for (const RockHit& hit : m_sim.rockHits) {
            if (static_cast<s32>(hit.rock) == item.rock) {
                dealt += std::max(1.0f, std::round(hit.damage - static_cast<f32>(item.rockArmor)));
            }
        }
        if (static_cast<f32>(item.rockHealth) <= dealt) {
            continue; // brought down already: nothing there
        }
        const f32 blow =
            std::max(1.0f, std::round(missile.kind.damage - static_cast<f32>(item.rockArmor)));
        m_sim.rockHits.push_back(RockHit{static_cast<usize>(item.rock), missile.kind.damage});
        if (!pierces || static_cast<f32>(item.rockHealth) > dealt + blow) {
            return true;
        }
    }
    return false;
}

std::vector<RockHit> EnemyMissiles::takeRockHits() {
    return std::exchange(m_sim.rockHits, {});
}

void EnemyMissiles::update(f32 seconds, const WorldCollision* collision,
                           std::span<const EnemyView> players, std::span<const MissileTarget> swarm,
                           std::span<const MissileStop> items, const SceneryBlast& scenery) {
    if (seconds <= 0) {
        return;
    }
    m_sim.ricochetIn = std::max(0.0f, m_sim.ricochetIn - seconds);
    stepBursts(seconds, collision, players, swarm, scenery);
    for (EnemyMissile& missile : m_sim.missiles) {
        f32 remaining = std::min(seconds, missile.secondsLeft);
        const f32 gravity = std::max(missile.kind.weight, 0.0f);
        const f32 radius = std::max(missile.kind.radius, 0.0f);
        const bool pierces = missile.kind.pierces();
        while (remaining > 0 && missile.secondsLeft > 0) {
            // Bound travel as well as time: the world collider tests overlaps, not segments.
            const f32 speedBound = glm::length(missile.velocity) + gravity * kSimulationStep;
            const f32 spatialStep = std::max(radius * 0.5f, kLeastSpatialStep);
            // Held where it started, it stays put for the while, striking what stands in it.
            const bool resting = missile.heldLeft > 0.0f;
            const f32 dt = resting ? std::min({remaining, kSimulationStep, missile.heldLeft})
                                   : std::min({remaining, kSimulationStep,
                                               spatialStep / std::max(speedBound, 1.0f)});
            const Vec3 from = missile.position;
            const Vec3 acceleration{0, resting ? 0.0f : -gravity, 0};
            const Vec3 to =
                resting ? from : from + missile.velocity * dt + acceleration * (0.5f * dt * dt);
            missile.velocity += acceleration * dt;
            missile.turned += missile.kind.spin * dt;
            missile.heldLeft = std::max(0.0f, missile.heldLeft - dt);
            missile.secondsLeft = std::max(0.0f, missile.secondsLeft - dt);
            missile.lived += dt;
            remaining = std::max(0.0f, remaining - dt);
            for (auto& [player, left] : missile.pierced) {
                left -= dt;
            }
            std::erase_if(missile.pierced, [](const auto& held) { return held.second <= 0.0f; });

            bool struckWorld = false;
            s32 worldObject = -1;
            bool liquid = false;
            Vec3 destination = to;
            if (collision != nullptr && !missile.kind.throughWorld) {
                std::vector<WallContact> contacts;
                const Vec3 pushed = collision->resolveWalls(
                    to, radius, to.y - radius, to.y + radius, &contacts, std::nullopt,
                    WorldCollision::WallPush::All, WorldCollision::kMissileQueryFlags);
                struckWorld = glm::distance(pushed, to) > 0.001f;
                if (struckWorld) {
                    destination = pushed;
                    if (!contacts.empty()) {
                        worldObject = contacts.front().object;
                    }
                } else if (const auto floor = collision->projectileFloorAt(
                               to, std::abs(to.y - from.y) + radius, radius + kFootClearance);
                           floor && to.y <= floor->y + radius &&
                           glm::dot(missile.velocity, floor->normal) < 0) {
                    struckWorld = true;
                    destination.y = floor->y + radius;
                    worldObject = floor->object;
                    liquid = (floor->objectFlags & WorldCollision::kLiquidSurface) != 0;
                }
            }
            if (!struckWorld && !missile.kind.throughWorld &&
                stopped(missile, items, from, to, radius)) {
                struckWorld = true;
                destination = from;
            }
            const EnemyView* victim = nullptr;
            f32 first = 1;
            if (!struckWorld) {
                for (const EnemyView& view : players) {
                    if (view.hidden) {
                        continue;
                    }
                    const auto contact = CombatantProjectile::contact(
                        from, to, radius, view.position, view.radius, view.height);
                    if (contact && (*contact < first || (victim == nullptr && *contact == first))) {
                        first = *contact;
                        victim = &view;
                    }
                }
            }
            // Sent back, it strikes the first of the swarm in its way.
            const MissileTarget* body = nullptr;
            if (!struckWorld && missile.reflected) {
                for (const MissileTarget& target : swarm) {
                    const auto contact = CombatantProjectile::contact(from, to, radius, target.base,
                                                                      target.radius, target.height);
                    if (contact && *contact < first) {
                        first = *contact;
                        body = &target;
                        victim = nullptr;
                    }
                }
            }
            if (victim != nullptr && victim->reflects) {
                // Armour that reflects turns it straight back, harmless to its wearer, with
                // less time and harm left (ProcessEffects, fn_8009EF7C).
                if (m_sim.ricochetIn <= 0.0f) {
                    m_sim.ricochetIn = kRicochetGap;
                    EnemyMissileHit ricochet;
                    ricochet.instance = missile.instance;
                    ricochet.ricochet = true;
                    ricochet.position = glm::mix(from, to, first);
                    m_sim.hits.push_back(ricochet);
                }
                ++missile.continuity;
                missile.velocity = -missile.velocity;
                missile.position = from;
                missile.reflected = true;
                missile.kind.damage = std::min(missile.kind.damage, kReflectedMost);
                missile.secondsLeft = missile.secondsLeft > kReflectedLife
                                          ? kReflectedLife
                                          : std::max(0.0f, missile.secondsLeft - 1.0f);
                if (missile.secondsLeft <= 0.0f && missile.kind.burstRadius > 0.0f) {
                    EnemyMissileHit hit;
                    hit.instance = missile.instance;
                    hit.shooter = missile.shooter;
                    hit.damage = missile.kind.damage;
                    hit.flags = missile.kind.hitFlags();
                    hit.burstRadius = missile.kind.burstRadius;
                    hit.position = missile.position;
                    m_sim.hits.push_back(hit);
                }
                continue;
            }
            if (victim != nullptr && pierces) {
                // A piercing missile hurts whoever it passes, each at most every quarter of a
                // second, and flies on.
                const bool held = std::ranges::any_of(missile.pierced, [&](const auto& pair) {
                    return pair.first == victim->player;
                });
                if (!held) {
                    missile.pierced.emplace_back(victim->player, kPierceGap);
                    EnemyMissileHit hit;
                    hit.instance = missile.instance;
                    hit.player = victim->player;
                    hit.shooter = missile.shooter;
                    hit.damage = missile.kind.damage;
                    hit.flags = missile.kind.hitFlags();
                    hit.position = glm::mix(from, to, first);
                    const f32 speed = glm::length(missile.velocity);
                    hit.direction = speed > 0.001f ? missile.velocity / speed : Vec3{0, 0, 1};
                    m_sim.hits.push_back(hit);
                }
                victim = nullptr;
            }
            missile.position =
                victim != nullptr || body != nullptr ? glm::mix(from, to, first) : destination;
            // A lob that runs out of time bursts where it is; a shot just goes.
            const bool expired = missile.secondsLeft <= 0;
            if (struckWorld || victim != nullptr || body != nullptr ||
                (expired && missile.kind.burstRadius > 0.0f)) {
                EnemyMissileHit hit;
                hit.instance = missile.instance;
                hit.worldContact = struckWorld;
                hit.worldObject = worldObject;
                hit.liquid = liquid;
                hit.player = victim != nullptr ? victim->player : -1;
                hit.target = body != nullptr ? body->id : -1;
                hit.shooter = missile.shooter;
                hit.damage = missile.kind.damage;
                hit.flags = missile.kind.hitFlags();
                hit.burstRadius = missile.kind.burstRadius;
                hit.position = missile.position;
                const f32 speed = glm::length(missile.velocity);
                hit.direction = speed > 0.001f ? missile.velocity / speed : Vec3{0, 0, 1};
                m_sim.hits.push_back(hit);
                missile.secondsLeft = 0;
            }
        }
    }
    std::erase_if(m_sim.missiles,
                  [](const EnemyMissile& missile) { return missile.secondsLeft <= 0.0f; });
}

std::vector<EnemyMissileHit> EnemyMissiles::takeHits() {
    return std::exchange(m_sim.hits, {});
}

Mat4 EnemyMissiles::transformOf(const EnemyMissile& missile) {
    // Pointed the way it flies, spinning as its kind does.
    const f32 yaw = portable::atan2(missile.velocity.x, missile.velocity.z);
    const f32 pitch = -portable::atan2(missile.velocity.y,
                                       flatDistance(Vec3{0.0f, 0.0f, 0.0f}, missile.velocity));
    Mat4 model = glm::translate(Mat4{1.0f}, missile.position);
    model = portable::rotate(model, yaw, Vec3{0.0f, 1.0f, 0.0f});
    model = portable::rotate(model, pitch, Vec3{1.0f, 0.0f, 0.0f});
    model = portable::rotate(model, missile.turned.y, Vec3{0.0f, 1.0f, 0.0f});
    model = portable::rotate(model, missile.turned.x, Vec3{1.0f, 0.0f, 0.0f});
    if (missile.kind.pierces() && missile.lived < kGrowth) {
        const f32 grown = kSmallest + (1.0f - kSmallest) * missile.lived / kGrowth;
        model = glm::scale(model, Vec3{grown});
    }
    if (missile.scale != 1.0f) {
        model = glm::scale(model, Vec3{missile.scale});
    }
    return model;
}

void EnemyMissiles::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera) const {
    for (const EnemyMissile& missile : m_sim.missiles) {
        if (missile.model == nullptr || !missile.model->bound()) {
            continue;
        }
        // SOR_FBALL's CFXO_H halo authors no depth test. Missiles draw after
        // the opaque scene here, so that flag would paint it through walls.
        // Constrain only this world-bound draw; keep its blend/depth-write
        // settings and the shared model's authored policy intact.
        missile.model->draw(device, clip, transformOf(missile), lighting, {}, camera, 1.0f,
                            TreeModel::Pass::All, TreeModel::Occlusion::SolidWorld);
    }
}

void EnemyMissiles::lights(std::vector<PointLight>& out) const {
    const auto light = [&out](const Vec3& position) {
        PointLight lit;
        lit.position = position;
        lit.color = DynamicLights::blast();
        lit.radius = kBombLightRadius;
        lit.intensity = DynamicLights::kEffectIntensity;
        out.push_back(lit);
    };
    for (const EnemyMissile& missile : m_sim.missiles) {
        if (missile.kind.slot == EnemyMissileKind::kBomb) {
            light(missile.position);
        }
    }
    for (const Burst& burst : m_sim.bursts) {
        if (burst.blast.lit) {
            light(burst.blast.position);
        }
    }
}

void EnemyMissiles::clear() {
    if (m_restoreGeneration != 0) {
        ++m_restoreGeneration;
        ++m_localEpoch;
    }
    m_sim.missiles.clear();
    m_sim.bursts.clear();
    m_sim.hits.clear();
    m_sim.rockHits.clear();
    m_sim.pickupBlasts.clear();
}

} // namespace gdl::game
