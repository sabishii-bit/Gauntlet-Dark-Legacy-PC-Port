#include "game/enemies/EnemyMissiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "engine/core/Types.h"

#include "game/enemies/CombatantProjectile.h"

namespace gdl::game {

namespace {

constexpr f32 kFootClearance = 0.1f;
constexpr f32 kSimulationStep = 1.0f / 30.0f;
constexpr f32 kLeastSpatialStep = 0.05f;

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

std::string_view EnemyMissileHit::effect() const {
    if (burstRadius > 0) {
        return "EXPSMALL";
    }
    constexpr std::array<std::string_view, 5> kWorldHits{"SPARKS", "FIREHIT", "HITCOL", "HITCOL",
                                                         "HITCOL"};
    const u32 element = flags & 0xF;
    return worldContact && element < kWorldHits.size() ? kWorldHits[element] : std::string_view{};
}

std::string_view EnemyMissileHit::sound() const {
    // Ordinary enemy bolts have no wall sound; the lobber's bomb supplies its own.
    return burstRadius > 0 ? "S_LOBBER_BOMB" : std::string_view{};
}

EnemyMissileKind EnemyMissileKind::arrow() {
    EnemyMissileKind kind;
    kind.damage = 10.0f;
    kind.speed = 25.0f;
    kind.radius = 0.5f;
    kind.weight = 30.0f;
    return kind;
}

EnemyMissileKind EnemyMissileKind::bomb() {
    EnemyMissileKind kind;
    kind.flags = kKnockBack;
    kind.damage = 10.0f;
    kind.speed = 20.0f;
    kind.radius = 0.2f;
    kind.burstRadius = 3.0f;
    kind.spin = Vec3{0.0f, 1.0f, 0.0f};
    kind.weight = 35.0f;
    return kind;
}

EnemyMissileKind EnemyMissileKind::bolt(f32 damage, f32 speed, f32 radius, u32 flags) {
    EnemyMissileKind kind;
    kind.flags = flags;
    kind.damage = damage;
    kind.speed = speed;
    kind.radius = radius;
    kind.weight = 0.0f;
    return kind;
}

std::optional<EnemyMissileKind> enemyMissileOf(s32 kind, s32 slot) {
    // The original's table (0x80119128): the medium kinds share the arrow and the bomb, and
    // a few have a bolt of their own; the worm has three of its own.
    const bool medium = kind == 1 || kind == 4 || kind == 7 || kind == 10 || kind == 13 ||
                        kind == 14 || kind == 16 || kind == 19 || kind == 23 || kind == 24;
    if (kind == 17) { // the worm
        switch (slot) {
        case 0: return EnemyMissileKind::bolt(5.0f, 15.0f, 0.3f);
        case 1: return EnemyMissileKind::bolt(10.0f, 20.0f, 0.3f);
        case 2: return EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f);
        default: return std::nullopt;
        }
    }
    if (slot == EnemyMissileKind::kArrow && medium) {
        return EnemyMissileKind::arrow();
    }
    if (slot == EnemyMissileKind::kBomb && medium) {
        return EnemyMissileKind::bomb();
    }
    if (slot == 2) {
        switch (kind) {
        case 2:  // the demon
        case 20: // the ghost
            return EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f, 0x1);
        case 14: // the plague
            return EnemyMissileKind::bolt(15.0f, 25.0f, 0.3f);
        case 7:  // the sorcerer
        case 24: // the warlock
            return EnemyMissileKind::bolt(20.0f, 20.0f, 0.2f, 0x3);
        case 27: // the garm
            return EnemyMissileKind::bolt(25.0f, 80.0f, 2.0f, 0x2);
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
    default: return 2;
    }
}

Vec3 EnemyMissiles::lobVelocity(const Vec3& from, const Vec3& to, f32 speed) {
    const f32 across = flatDistance(from, to);
    const f32 flight = std::max(across / std::max(speed, 0.001f), kLeastFlight);
    Vec3 velocity{(to.x - from.x) / flight, 0.0f, (to.z - from.z) / flight};
    velocity.y = (to.y - from.y) / flight + 0.5f * kGravity * flight;
    return velocity;
}

void EnemyMissiles::launch(const EnemyMissileKind& kind, const Vec3& from, const Vec3& aim,
                           f32 speedScale, const TreeModel* model, s32 shooter) {
    EnemyMissile missile;
    missile.kind = kind;
    missile.position = from;
    missile.model = model;
    missile.shooter = shooter;
    missile.secondsLeft = kLife;
    const f32 speed = kind.speed * std::max(speedScale, 0.01f);
    if (kind.burstRadius > 0.0f) {
        missile.velocity = lobVelocity(from, aim, speed);
    } else {
        const Vec3 way = aim - from;
        const f32 length = glm::length(way);
        missile.velocity = length > 0.001f ? way * (speed / length) : Vec3{0.0f, 0.0f, speed};
    }
    m_missiles.push_back(missile);
}

void EnemyMissiles::update(f32 seconds, const WorldCollision* collision,
                           std::span<const EnemyView> players) {
    if (seconds <= 0) {
        return;
    }
    for (EnemyMissile& missile : m_missiles) {
        f32 remaining = std::min(seconds, missile.secondsLeft);
        const f32 gravity = missile.kind.burstRadius > 0 ? kGravity : 0;
        const f32 radius = std::max(missile.kind.radius, 0.0f);
        while (remaining > 0 && missile.secondsLeft > 0) {
            // Bound travel as well as time: the world collider tests overlaps, not segments.
            const f32 speedBound = glm::length(missile.velocity) + gravity * kSimulationStep;
            const f32 spatialStep = std::max(radius * 0.5f, kLeastSpatialStep);
            const f32 dt =
                std::min({remaining, kSimulationStep, spatialStep / std::max(speedBound, 1.0f)});
            const Vec3 from = missile.position;
            const Vec3 acceleration{0, -gravity, 0};
            const Vec3 to = from + missile.velocity * dt + acceleration * (0.5f * dt * dt);
            missile.velocity += acceleration * dt;
            missile.turned += missile.kind.spin * dt;
            missile.secondsLeft = std::max(0.0f, missile.secondsLeft - dt);
            remaining = std::max(0.0f, remaining - dt);

            bool struckWorld = false;
            Vec3 destination = to;
            if (collision != nullptr) {
                const Vec3 pushed =
                    collision->resolveWalls(to, radius, to.y - radius, to.y + radius);
                struckWorld = glm::distance(pushed, to) > 0.001f;
                if (struckWorld) {
                    destination = pushed;
                } else if (const auto floor = collision->floorAt(
                               to, std::abs(to.y - from.y) + radius, radius + kFootClearance);
                           floor && to.y <= floor->y + radius &&
                           glm::dot(missile.velocity, floor->normal) < 0) {
                    struckWorld = true;
                    destination.y = floor->y + radius;
                }
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
            missile.position = victim != nullptr ? glm::mix(from, to, first) : destination;
            if (struckWorld || victim != nullptr || missile.secondsLeft <= 0) {
                EnemyMissileHit hit;
                hit.worldContact = struckWorld;
                hit.player = victim != nullptr ? victim->player : -1;
                hit.shooter = missile.shooter;
                hit.damage = missile.kind.damage;
                hit.flags = missile.kind.flags;
                hit.burstRadius = missile.kind.burstRadius;
                hit.position = missile.position;
                const f32 speed = glm::length(missile.velocity);
                hit.direction = speed > 0.001f ? missile.velocity / speed : Vec3{0, 0, 1};
                m_hits.push_back(hit);
                missile.secondsLeft = 0;
            }
        }
    }
    std::erase_if(m_missiles,
                  [](const EnemyMissile& missile) { return missile.secondsLeft <= 0.0f; });
}

std::vector<EnemyMissileHit> EnemyMissiles::takeHits() {
    return std::exchange(m_hits, {});
}

void EnemyMissiles::draw(RenderDevice& device, const Mat4& clip,
                         const WorldLighting& lighting) const {
    for (const EnemyMissile& missile : m_missiles) {
        if (missile.model == nullptr || !missile.model->bound()) {
            continue;
        }
        // Pointed the way it flies, spinning as its kind does.
        const f32 yaw = std::atan2(missile.velocity.x, missile.velocity.z);
        const f32 pitch =
            -std::atan2(missile.velocity.y, flatDistance(Vec3{0.0f, 0.0f, 0.0f}, missile.velocity));
        Mat4 model = glm::translate(Mat4{1.0f}, missile.position);
        model = glm::rotate(model, yaw, Vec3{0.0f, 1.0f, 0.0f});
        model = glm::rotate(model, pitch, Vec3{1.0f, 0.0f, 0.0f});
        model = glm::rotate(model, missile.turned.y, Vec3{0.0f, 1.0f, 0.0f});
        model = glm::rotate(model, missile.turned.x, Vec3{1.0f, 0.0f, 0.0f});
        missile.model->draw(device, clip, model, lighting);
    }
}

void EnemyMissiles::clear() {
    m_missiles.clear();
    m_hits.clear();
}

} // namespace gdl::game
