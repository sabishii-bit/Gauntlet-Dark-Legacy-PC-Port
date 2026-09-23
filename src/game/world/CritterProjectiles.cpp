#include "game/world/CritterProjectiles.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr f32 kMorphLife = 15.0f;
constexpr f32 kSimulationStep = 1.0f / 120.0f;
constexpr f32 kWallTolerance = 0.001f;
constexpr f32 kFloorClearance = 0.1f;
constexpr u32 kSpin = 8;
constexpr u32 kCustomEffect = 0xF000000;
} // namespace

u32 CritterProjectiles::show(Flying& flying, s32 index, RenderDevice& device, EffectTrees& effects,
                             const PlaySound& sound, f32 life) {
    const CritterSound* cue = flying.shot.data->sound(index);
    if (cue == nullptr) {
        return 0;
    }
    if (sound) {
        const std::string name = cue->soundFor(flying.shot.realm);
        if (!name.empty()) {
            sound(name);
        }
    }
    if (!cue->shows()) {
        return 0;
    }
    EffectTrees::Setting setting;
    setting.scale = cue->scale * flying.shot.scale;
    setting.yaw = std::atan2(flying.velocity.x, flying.velocity.z);
    setting.seconds = life > 0.0f ? life : cue->life;
    return effects.startSet(device, *flying.archive, cue->tree, flying.position, setting);
}

void CritterProjectiles::place(const Flying& flying, EffectTrees& effects) {
    Mat4 transform = glm::translate(Mat4{1.0f}, flying.position);
    transform = glm::rotate(transform, flying.rotation.y, Vec3{0, 1, 0});
    transform = glm::rotate(transform, flying.rotation.x, Vec3{1, 0, 0});
    transform = glm::rotate(transform, flying.rotation.z, Vec3{0, 0, 1});
    effects.placeAt(flying.effect, transform);
}

void CritterProjectiles::launch(const CritterShot& shot, ItemArchive& archive, RenderDevice& device,
                                EffectTrees& effects, const PlaySound& sound) {
    const CritterDamage* damage =
        shot.data != nullptr ? shot.data->damage(shot.damageIndex) : nullptr;
    if (damage == nullptr || damage->type != CritterDamage::kProjectile) {
        return;
    }
    const CritterSound* cue = shot.data->sound(damage->sound);
    if (cue == nullptr) {
        return;
    }
    if ((cue->flags & kCustomEffect) != 0) {
        log::warn("critter {}: custom projectile effect {} is not implemented", shot.data->name(),
                  cue->tree);
        return;
    }
    std::uniform_real_distribution<f32> spread{-1.0f, 1.0f};
    Flying flying;
    flying.shot = shot;
    // Unattached SFXX offsets are world-axis offsets, scaled by the creature.
    flying.shot.origin += cue->offset * shot.scale;
    flying.archive = &archive;
    flying.position = flying.shot.origin;
    flying.velocity = CritterProjectile::velocity(*damage, flying.shot, spread(m_random));
    flying.rotation.y = std::atan2(flying.velocity.x, flying.velocity.z);
    if ((cue->flags & kSpin) != 0) {
        constexpr f32 kSpinRate = std::numbers::pi_v<f32> / 2.0f;
        std::uniform_real_distribution<f32> spin{0.0f, kSpinRate};
        flying.spin = Vec3{spin(m_random), 0.0f, spin(m_random)};
    }
    flying.effect = show(flying, damage->sound, device, effects, sound, shot.birthLife);
    if (flying.effect != 0) {
        place(flying, effects);
        m_flying.push_back(flying);
    }
}

void CritterProjectiles::update(f32 seconds, const WorldCollision* collision,
                                std::span<const EnemyView> players, RenderDevice& device,
                                EffectTrees& effects, const PlaySound& sound) {
    if (seconds <= 0.0f) {
        return;
    }
    for (Flying& flying : m_flying) {
        const CritterDamage& damage = *flying.shot.data->damage(flying.shot.damageIndex);
        if (!effects.playing(flying.effect)) {
            if (!flying.morphed && damage.morph >= 0) {
                flying.morphed = true;
                flying.effect = show(flying, damage.morph, device, effects, sound,
                                     damage.morphLife > 0.0f ? damage.morphLife : kMorphLife);
            } else {
                if (flying.morphed) {
                    show(flying, damage.morphEnd, device, effects, sound);
                }
                flying.effect = 0;
            }
        }
        if (flying.effect == 0) {
            continue;
        }
        if (!flying.morphed && damage.morph >= 0 &&
            (damage.behaviorFlags & CritterProjectile::kWaitForMorph) != 0) {
            place(flying, effects);
            continue;
        }
        const f32 radius = std::max(0.0f, damage.radius * flying.shot.scale);
        // Small steps also cover thin walls with the world's overlap-based collider.
        const f32 spatialStep =
            std::max(radius * 0.5f, kFloorClearance) / std::max(glm::length(flying.velocity), 1.0f);
        const auto steps =
            static_cast<s32>(std::ceil(seconds / std::min(kSimulationStep, spatialStep)));
        const f32 dt = seconds / static_cast<f32>(steps);
        for (s32 step = 0; step < steps; ++step) {
            const Vec3 from = flying.position;
            const Vec3 acceleration{0.0f, -damage.gravity, 0.0f};
            const Vec3 to = from + flying.velocity * dt + acceleration * (0.5f * dt * dt);
            flying.velocity += acceleration * dt;
            flying.rotation += flying.spin * dt;
            bool wall = false;
            Vec3 destination = to;
            if (collision != nullptr &&
                (damage.behaviorFlags & CritterProjectile::kIgnoreWorld) == 0) {
                const Vec3 pushed =
                    collision->resolveWalls(to, radius, to.y - radius, to.y + radius);
                wall = glm::length(pushed - to) > kWallTolerance;
                const auto floor = collision->floorAt(to, std::abs(to.y - from.y) + radius,
                                                      radius + kFloorClearance);
                if (floor.has_value() && to.y <= floor->y + radius) {
                    wall = true;
                    destination.y = floor->y + radius;
                }
            }
            f32 nearest = 1.0f;
            const EnemyView* victim = nullptr;
            if ((damage.behaviorFlags & CritterProjectile::kNoPlayerDamage) == 0) {
                for (const EnemyView& player : players) {
                    if (player.hidden) {
                        continue;
                    }
                    const auto at = CritterProjectile::contact(from, to, radius, player.position,
                                                               player.radius, player.height);
                    if (at.has_value() &&
                        (*at < nearest || (victim == nullptr && *at == nearest))) {
                        nearest = *at;
                        victim = &player;
                    }
                }
            }
            // A blocking world overlap owns this small step; do not hit through its wall.
            if (wall || victim != nullptr) {
                flying.position = wall ? destination : glm::mix(from, to, nearest);
                if (!wall) {
                    m_hits.push_back(
                        {victim->player, damage.damage * flying.shot.damageScale, damage.flags});
                }
                effects.stop(flying.effect);
                show(flying, damage.hitSound, device, effects, sound);
                flying.effect = 0;
                break;
            }
            flying.position = to;
        }
        if (flying.effect != 0) {
            place(flying, effects);
        }
    }
    std::erase_if(m_flying, [](const Flying& flying) { return flying.effect == 0; });
}

void CritterProjectiles::clear(EffectTrees& effects) {
    for (const Flying& flying : m_flying) {
        effects.stop(flying.effect);
    }
    m_flying.clear();
    m_hits.clear();
}

std::vector<CritterProjectileHit> CritterProjectiles::takeHits() {
    return std::exchange(m_hits, {});
}
} // namespace gdl::game
