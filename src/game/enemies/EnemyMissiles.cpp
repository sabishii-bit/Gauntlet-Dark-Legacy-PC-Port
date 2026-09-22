#include "game/enemies/EnemyMissiles.h"

#include <cmath>
#include <utility>

namespace gdl::game {

namespace {

constexpr f32 kFootClearance = 0.1f;

f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

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
    for (EnemyMissile& missile : m_missiles) {
        if (missile.kind.burstRadius > 0.0f) {
            missile.velocity.y -= kGravity * seconds;
        }
        const Vec3 from = missile.position;
        const Vec3 to = from + missile.velocity * seconds;
        missile.turned += missile.kind.spin * seconds;
        missile.secondsLeft -= seconds;
        // The first player its body meets along the step takes it.
        bool ended = false;
        for (const EnemyView& view : players) {
            if (view.hidden) {
                continue;
            }
            const Vec3 centre = view.position + Vec3{0.0f, 0.5f * view.height, 0.0f};
            const Vec3 sweep = to - from;
            const f32 length = glm::length(sweep);
            const f32 t = length > 0.001f
                              ? std::clamp(glm::dot(centre - from, sweep) / (length * length), 0.0f, 1.0f)
                              : 0.0f;
            const Vec3 nearest = from + sweep * t;
            if (flatDistance(nearest, centre) <= view.radius + missile.kind.radius &&
                std::abs(nearest.y - centre.y) <= 0.5f * view.height + missile.kind.radius) {
                EnemyMissileHit hit;
                hit.player = view.player;
                hit.shooter = missile.shooter;
                hit.damage = missile.kind.damage;
                hit.flags = missile.kind.flags;
                hit.burstRadius = missile.kind.burstRadius;
                hit.position = nearest;
                hit.direction = length > 0.001f ? sweep / length : Vec3{0.0f, 0.0f, 1.0f};
                m_hits.push_back(hit);
                ended = true;
                break;
            }
        }
        if (ended) {
            missile.secondsLeft = 0.0f;
            continue;
        }
        // Else the world: a wall in the way, or the floor reached, ends it there.
        bool struckWorld = false;
        if (collision != nullptr) {
            const Vec3 pushed = collision->resolveWalls(to, missile.kind.radius, to.y - missile.kind.radius,
                                                        to.y + missile.kind.radius);
            struckWorld = flatDistance(pushed, to) > 0.001f;
            if (!struckWorld) {
                if (const auto floor = collision->floorAt(to, 0.0f, 2.0f * missile.kind.radius + kFootClearance)) {
                    struckWorld = to.y <= floor->y + missile.kind.radius;
                }
            }
        }
        if (struckWorld || missile.secondsLeft <= 0.0f) {
            EnemyMissileHit hit;
            hit.player = -1;
            hit.shooter = missile.shooter;
            hit.damage = missile.kind.damage;
            hit.flags = missile.kind.flags;
            hit.burstRadius = missile.kind.burstRadius;
            hit.position = to;
            m_hits.push_back(hit);
            missile.secondsLeft = 0.0f;
            continue;
        }
        missile.position = to;
    }
    std::erase_if(m_missiles, [](const EnemyMissile& missile) { return missile.secondsLeft <= 0.0f; });
}

std::vector<EnemyMissileHit> EnemyMissiles::takeHits() {
    return std::exchange(m_hits, {});
}

void EnemyMissiles::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const EnemyMissile& missile : m_missiles) {
        if (missile.model == nullptr || !missile.model->bound()) {
            continue;
        }
        // Pointed the way it flies, spinning as its kind does.
        const f32 yaw = std::atan2(missile.velocity.x, missile.velocity.z);
        const f32 pitch = -std::atan2(missile.velocity.y, flatDistance(Vec3{0.0f, 0.0f, 0.0f}, missile.velocity));
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
