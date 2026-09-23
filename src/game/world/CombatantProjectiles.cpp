#include "game/world/CombatantProjectiles.h"

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
constexpr u32 kReflect = 0x200000;
constexpr f32 kBounceLift = 0.4f;
constexpr f32 kBounceLifetime = 10.0f;
constexpr f32 kBounceTimeLoss = 1.0f;
constexpr f32 kFloorImpactLift = 2.0f;
} // namespace

u32 CombatantProjectiles::show(Flying& flying, s32 index, RenderDevice& device,
                               EffectTrees& effects, const PlaySound& sound, f32 life) {
    const CombatEffectDefinition* cue = flying.shot.data->sound(index);
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
    const u32 effect =
        effects.startSet(device, *flying.archive, cue->tree, flying.position, setting);
    if (effect != 0) {
        m_emittedEffects.push_back(effect);
    }
    return effect;
}

void CombatantProjectiles::place(const Flying& flying, EffectTrees& effects) {
    Mat4 transform = glm::translate(Mat4{1.0f}, flying.position);
    transform = glm::rotate(transform, flying.rotation.y, Vec3{0, 1, 0});
    transform = glm::rotate(transform, flying.rotation.x, Vec3{1, 0, 0});
    transform = glm::rotate(transform, flying.rotation.z, Vec3{0, 0, 1});
    effects.placeAt(flying.effect, transform);
}

void CombatantProjectiles::launch(const CombatShot& shot, ItemArchive& archive,
                                  RenderDevice& device, EffectTrees& effects,
                                  const PlaySound& sound) {
    const AttackDefinition* damage =
        shot.data != nullptr ? shot.data->damage(shot.damageIndex) : nullptr;
    if (damage == nullptr || damage->type != AttackDefinition::kProjectile) {
        return;
    }
    const CombatEffectDefinition* cue = shot.data->sound(damage->sound);
    if (cue == nullptr) {
        return;
    }
    if ((cue->flags & kCustomEffect) != 0) {
        log::warn("combatant {}: custom projectile effect {} is not implemented", shot.data->name(),
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
    flying.velocity = CombatantProjectile::velocity(*damage, flying.shot, spread(m_random));
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

void CombatantProjectiles::update(f32 seconds, const WorldCollision* collision,
                                  std::span<const EnemyView> players, RenderDevice& device,
                                  EffectTrees& effects, const PlaySound& sound) {
    if (seconds <= 0.0f) {
        return;
    }
    for (Flying& flying : m_flying) {
        const AttackDefinition& damage = *flying.shot.data->damage(flying.shot.damageIndex);
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
            (damage.behaviorFlags & CombatantProjectile::kWaitForMorph) != 0) {
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
            Vec3 normal{0};
            Vec3 destination = to;
            if (collision != nullptr &&
                (damage.behaviorFlags & CombatantProjectile::kIgnoreWorld) == 0) {
                // WeaponWallCollide uses half the player-contact radius.
                const f32 worldRadius = 0.5f * radius;
                const Vec3 pushed = collision->resolveWalls(to, worldRadius, to.y - worldRadius,
                                                            to.y + worldRadius);
                wall = glm::length(pushed - to) > kWallTolerance;
                if (wall) {
                    normal = glm::normalize(pushed - to);
                    destination = pushed;
                }
                const auto floor = collision->floorAt(to, std::abs(to.y - from.y) + worldRadius,
                                                      worldRadius + kFloorClearance);
                if (floor.has_value() && to.y <= floor->y + worldRadius &&
                    glm::dot(flying.velocity, floor->normal) < 0) {
                    wall = true;
                    normal = floor->normal;
                    destination.y = floor->y + worldRadius;
                    // Floor objects lift effect impacts off the hit plane. Without this
                    // clearance, a shallow rebound hits again on every physics substep.
                    if ((floor->objectFlags & WorldObject::kFloor) != 0) {
                        destination.y = floor->y + kFloorImpactLift;
                    }
                }
            }
            f32 nearest = 1.0f;
            const EnemyView* victim = nullptr;
            if ((damage.behaviorFlags & CombatantProjectile::kNoPlayerDamage) == 0) {
                for (const EnemyView& player : players) {
                    if (player.hidden) {
                        continue;
                    }
                    const auto at = CombatantProjectile::contact(from, to, radius, player.position,
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
                if (wall && (damage.flags & kReflect) != 0) {
                    if (glm::dot(flying.velocity, normal) < 0) {
                        flying.velocity = glm::reflect(flying.velocity, normal);
                        if (flying.velocity.y > 0) {
                            flying.velocity.y *= kBounceLift;
                        }
                        effects.shortenLifetime(flying.effect, kBounceTimeLoss, kBounceLifetime);
                    }
                    continue;
                }
                if (!wall) {
                    const f32 speed = glm::length(flying.velocity);
                    m_hits.push_back({victim->player, damage.damage * flying.shot.damageScale,
                                      damage.flags,
                                      speed > 0.0f ? flying.velocity / speed : Vec3{0.0f}});
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
    std::erase_if(m_emittedEffects, [&](u32 effect) { return !effects.playing(effect); });
}

void CombatantProjectiles::clear(EffectTrees& effects) {
    for (const u32 effect : m_emittedEffects) {
        effects.stop(effect);
    }
    m_emittedEffects.clear();
    m_flying.clear();
    m_hits.clear();
}

std::vector<CombatantProjectileHit> CombatantProjectiles::takeHits() {
    return std::exchange(m_hits, {});
}
} // namespace gdl::game
