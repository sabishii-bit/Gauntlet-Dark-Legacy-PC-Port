#include "game/world/CombatantProjectiles.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/ParticleSystem.h"

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
constexpr u32 kPassThrough = 0x100000;
constexpr u32 kSticky = 0x4000000;
constexpr f32 kStickyLife = 20.0f;
constexpr f32 kProjectileHitGap = 0.25f;

f32 projectileHitGap(const ItemArchive& archive, const CombatEffectDefinition* impact,
                     bool pierces) {
    // ProcessEffects (0x80094be0) gives ordinary projectiles immunity for the
    // impact's first sequence frame count / 30, not SFXX life or playback rate.
    // Piercing attacks and absent/zero-frame impacts retain the 0.25s default.
    if (!pierces && impact != nullptr) {
        if (const auto tree = archive.trees.find(impact->tree)) {
            const auto& sequences = archive.trees.tree(*tree).sequences;
            if (!sequences.empty() && sequences.front().frames > 0) {
                return static_cast<f32>(sequences.front().frames) / 30.0f;
            }
        }
    }
    return kProjectileHitGap;
}

ParticleDescriptor fireTrail(const CombatEffectDefinition& cue) {
    // CritterDoParticle's kind 2 modifies allocPsys defaults, not a preset.
    ParticleDescriptor trail;
    trail.texture = cue.tree;
    trail.emitFrames = cue.life < 0
                           ? ParticleDescriptor::kEndless
                           : static_cast<u32>(std::clamp(cue.life * 30.0f, 1.0f, 65535.0f));
    trail.fadeFrames = 1;
    trail.particleLife = 6;
    trail.particleFade = 6;
    trail.angle = ParticleDescriptor::kSphere;
    trail.rate.fill(std::max(cue.particleRate, 0.0f));
    trail.speed = cue.particleSpeed / ParticleDescriptor::kFrameRate;
    trail.red = trail.green = trail.blue = {255, 255, 255, 255};
    trail.alpha = {255, 255, 255, 0};
    trail.width = {0.5f, 0.5f, 0.5f, 0.5f};
    trail.additive = true;
    trail.depthWrite = false;
    return trail;
}
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
        // The only shipped projectile SFXX link is a custom emitter parented
        // to the preceding effect. Keep malformed chains from cycling forever.
        std::vector<s32> visited{index};
        for (s32 at = cue->link; at >= 0;) {
            if (std::ranges::find(visited, at) != visited.end()) {
                log::warn("combatant {}: cyclic projectile effect link {}",
                          flying.shot.data->name(), at);
                break;
            }
            visited.push_back(at);
            const auto* linked = flying.shot.data->sound(at);
            if (linked == nullptr) {
                break;
            }
            if ((linked->flags & (kCustomEffect | 0x4000U)) == 0x02004000U &&
                linked->offset == Vec3{0}) {
                if (const auto texture = flying.archive->textures.find(linked->tree)) {
                    effects.attachTrail(effect, fireTrail(*linked),
                                        flying.archive->textures.texture(device, *texture));
                } else {
                    log::warn("combatant {}: missing projectile trail texture {}",
                              flying.shot.data->name(), linked->tree);
                }
            } else {
                log::warn("combatant {}: unsupported linked projectile effect {}",
                          flying.shot.data->name(), linked->tree);
            }
            if (sound) {
                const std::string name = linked->soundFor(flying.shot.realm);
                if (!name.empty()) {
                    sound(name);
                }
            }
            at = linked->link;
        }
    }
    return effect;
}

void CombatantProjectiles::place(const Flying& flying, EffectTrees& effects) {
    Mat4 transform = glm::translate(Mat4{1.0f}, flying.position);
    transform = glm::rotate(transform, flying.rotation.y, Vec3{0, 1, 0});
    transform = glm::rotate(transform, flying.rotation.x, Vec3{1, 0, 0});
    transform = glm::rotate(transform, flying.rotation.z, Vec3{0, 0, 1});
    const auto* damage = flying.shot.data->damage(flying.shot.damageIndex);
    constexpr u32 kArrow = 0x20000;
    // ProcessEffects aligns arrow effects with velocity every frame, including
    // its vertical component. Garm's eye ribbons use this, not a yaw-only pose.
    const bool directed = !flying.stuck && !flying.settled && (damage->flags & kArrow) != 0;
    effects.placeAt(flying.effect, transform,
                    directed ? std::optional<Vec3>{flying.velocity} : std::nullopt);
}

void CombatantProjectiles::launch(const CombatShot& shot, ItemArchive& archive,
                                  RenderDevice& device, EffectTrees& effects,
                                  const PlaySound& sound, const WorldCollision* collision) {
    const AttackDefinition* damage =
        shot.data != nullptr ? shot.data->damage(shot.damageIndex) : nullptr;
    const bool planted = damage != nullptr && damage->type == AttackDefinition::kTargetArea &&
                         (damage->flags & kSticky) != 0;
    if (damage == nullptr || (!planted && damage->type != AttackDefinition::kProjectile)) {
        return;
    }
    const CombatEffectDefinition* cue = shot.data->sound(damage->sound);
    if (cue == nullptr) {
        return;
    }
    if ((cue->flags & kCustomEffect) != 0) {
        // CritterDoParticle returns no effect handle for CritterDoDamageFX to
        // endow with projectile physics or damage. Shipped particles are links.
        log::warn("combatant {}: particle-only cue {} cannot carry projectile damage",
                  shot.data->name(), cue->tree);
        return;
    }
    std::uniform_real_distribution<f32> spread{-1.0f, 1.0f};
    Flying flying;
    flying.shot = shot;
    // Unattached SFXX offsets are world-axis offsets, scaled by the creature.
    flying.shot.origin += cue->offset * shot.scale;
    flying.archive = &archive;
    flying.leavesGenerator = (cue->flags & 0x20000U) != 0;
    flying.summonsEnemies = (cue->flags & 0x400000U) != 0;
    flying.position = flying.shot.origin;
    flying.impactRadius = damage->maxDistance * shot.scale;
    if (planted) {
        flying.planted = true;
        flying.stuck = true;
        if (collision != nullptr && (cue->flags & 0x10U) != 0) {
            const f32 radius = std::max(0.0f, damage->radius * shot.scale);
            if (const auto floor =
                    collision->floorAt(flying.position, 1 + radius * 0.5f, 5 + radius * 0.5f)) {
                flying.position.y = floor->y + kFloorClearance;
                flying.rotation.x = std::atan2(floor->normal.z, floor->normal.y);
                flying.rotation.z =
                    -std::atan2(floor->normal.x, std::hypot(floor->normal.y, floor->normal.z));
            }
        }
        flying.effect = show(flying, damage->sound, device, effects, sound, shot.birthLife);
        flying.phaseSeconds = effects.remaining(flying.effect).value_or(0);
        if (flying.effect != 0) {
            place(flying, effects);
            m_flying.push_back(flying);
        }
        return;
    }
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
                                  EffectTrees& effects, const PlaySound& sound,
                                  std::span<const MissileStop> items,
                                  std::span<const MissileTarget> opponents) {
    if (seconds <= 0.0f) {
        return;
    }
    m_ricochetIn = std::max(0.0f, m_ricochetIn - seconds);
    for (Flying& flying : m_flying) {
        const AttackDefinition& damage = *flying.shot.data->damage(flying.shot.damageIndex);
        if (flying.planted) {
            // Damage ends with the hold, not with the last lingering particle of
            // the disappearance effect. Carry coarse updates across both phases.
            f32 remaining = seconds;
            while (remaining > 0 && flying.effect != 0) {
                const f32 active = std::min(remaining, flying.phaseSeconds);
                stickyContacts(flying, active, players);
                remaining -= active;
                flying.phaseSeconds -= active;
                if (flying.phaseSeconds > 0) {
                    break;
                }
                effects.finish(flying.effect);
                if (!flying.morphed && damage.morph >= 0) {
                    flying.morphed = true;
                    flying.phaseSeconds = damage.morphLife > 0 ? damage.morphLife : kMorphLife;
                    flying.effect =
                        show(flying, damage.morph, device, effects, sound, flying.phaseSeconds);
                    place(flying, effects);
                } else {
                    flying.effect = show(flying, damage.morphEnd, device, effects, sound);
                    place(flying, effects);
                    flying.effect = 0;
                }
            }
            continue;
        }
        if (!effects.playing(flying.effect)) {
            if (!flying.stuck && !flying.settled && !flying.morphed && damage.morph >= 0) {
                flying.morphed = true;
                // ProcessEffects clears damageradius when no second morph remains.
                // Plague's ACID_BALL0 can splash; ACID_BALL1 still hurts on contact
                // but must not create another damaging area or explode on timeout.
                if (damage.morphEnd < 0) {
                    flying.impactRadius = 0;
                }
                flying.effect = show(flying, damage.morph, device, effects, sound,
                                     damage.morphLife > 0.0f ? damage.morphLife : kMorphLife);
            } else if (!flying.settled && !flying.stuck && flying.impactRadius > 0 &&
                       damage.hitSound >= 0 &&
                       settleImpact(flying, show(flying, damage.hitSound, device, effects, sound),
                                    effects, collision)) {
                place(flying, effects);
            } else {
                if (flying.morphed) {
                    show(flying, damage.morphEnd, device, effects, sound);
                } else if (flying.shot.endVisual && !flying.settled && !flying.stuck) {
                    // The family can make a silent flight expiry visible without
                    // starting an impact area or changing its completion callbacks.
                    show(flying, damage.hitSound, device, effects, sound);
                }
                if (flying.leavesGenerator) {
                    Mat4 placement = glm::translate(Mat4{1}, flying.position);
                    placement = glm::rotate(placement, flying.rotation.y, Vec3{0, 1, 0});
                    m_generators.push_back(placement);
                }
                summon(flying);
                flying.effect = 0;
            }
        }
        if (flying.effect == 0) {
            continue;
        }
        if (flying.settled) {
            impactContacts(flying, seconds, players);
            continue;
        }
        if (!flying.morphed && damage.morph >= 0 &&
            (damage.behaviorFlags & CombatantProjectile::kWaitForMorph) != 0) {
            place(flying, effects);
            continue;
        }
        const f32 radius = std::max(0.0f, damage.radius * flying.shot.scale);
        if (flying.stuck) {
            stickyContacts(flying, seconds, players);
            continue;
        }
        std::vector<s32> contacted;
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
            s32 worldObject = -1;
            bool liquid = false;
            Vec3 normal{0};
            Vec3 destination = to;
            if (collision != nullptr &&
                (damage.behaviorFlags & CombatantProjectile::kIgnoreWorld) == 0) {
                // WeaponWallCollide uses half the player-contact radius.
                const f32 worldRadius = 0.5f * radius;
                std::vector<WallContact> contacts;
                const Vec3 pushed = collision->resolveWalls(to, worldRadius, to.y - worldRadius,
                                                            to.y + worldRadius, &contacts);
                wall = glm::length(pushed - to) > kWallTolerance;
                if (wall) {
                    normal = glm::normalize(pushed - to);
                    destination = pushed;
                    if (!contacts.empty()) {
                        worldObject = contacts.front().object;
                    }
                }
                const auto floor = collision->projectileFloorAt(
                    to, std::abs(to.y - from.y) + worldRadius, worldRadius + kFloorClearance);
                if (floor.has_value() && to.y <= floor->y + worldRadius &&
                    glm::dot(flying.velocity, floor->normal) < 0) {
                    wall = true;
                    normal = floor->normal;
                    destination.y = floor->y + worldRadius;
                    worldObject = floor->object;
                    liquid = (floor->objectFlags & WorldCollision::kLiquidSurface) != 0;
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
                    if (player.hidden ||
                        std::ranges::find(contacted, player.player) != contacted.end()) {
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
            const MissileTarget* returnedTarget = nullptr;
            if (!wall && flying.reflected) {
                for (const MissileTarget& target : opponents) {
                    const auto at = CombatantProjectile::contact(from, to, radius, target.base,
                                                                 target.radius, target.height);
                    if (at && *at < nearest) {
                        nearest = *at;
                        returnedTarget = &target;
                        victim = nullptr;
                    }
                }
            }
            const auto item = hitItems(flying, from, wall ? destination : to, nearest, items);
            if (!wall && !item && victim != nullptr && victim->reflects) {
                // ProcessEffects (80094BE0), mode 0: reverse a direct missile,
                // cap its harm at 15, shorten its life, and enable critter hits.
                // Planted/sticky/area effects above deliberately do not use this branch.
                if (m_ricochetIn <= 0) {
                    CombatantProjectileHit cue;
                    cue.ricochet = true;
                    cue.position = glm::mix(from, to, nearest);
                    m_hits.push_back(cue);
                    m_ricochetIn = 1;
                }
                flying.velocity = -flying.velocity;
                flying.position = from;
                flying.reflected = true;
                flying.rotation.y = std::atan2(flying.velocity.x, flying.velocity.z);
                const f32 amount = damage.damage * flying.shot.damageScale;
                if (amount > 15) {
                    flying.shot.damageScale *= 15 / amount;
                }
                effects.shortenLifetime(flying.effect, kBounceTimeLoss, kBounceLifetime);
                effects.snapPresentation(flying.effect);
                contacted.push_back(victim->player);
                continue;
            }
            // Item impacts are not reflective world contacts (ProcessEffects mode 0).
            // Query only the travelled segment so cover cannot be damaged through a wall.
            if (wall || item || victim != nullptr || returnedTarget != nullptr) {
                if (item) {
                    flying.position = glm::mix(from, wall ? destination : to, item->fraction);
                } else {
                    flying.position = wall ? destination : glm::mix(from, to, nearest);
                }
                if (wall && !item) {
                    m_worldHits.push_back(
                        {flying.position, worldObject, liquid && (damage.flags & kReflect) == 0});
                }
                summon(flying);
                if (!wall && !item && returnedTarget != nullptr) {
                    CombatantProjectileHit hit;
                    hit.target = returnedTarget->id;
                    hit.damage = damage.damage * flying.shot.damageScale;
                    hit.flags = damage.flags;
                    hit.direction = glm::length(flying.velocity) > 0
                                        ? glm::normalize(flying.velocity)
                                        : Vec3{0};
                    hit.position = flying.position;
                    m_hits.push_back(hit);
                }
                if (item && item->suppressEffect) {
                    // Surviving cover ends the piercing flight. An optional end
                    // visual changes only its presentation, never its damage.
                    if (flying.shot.endVisual) {
                        show(flying, damage.hitSound, device, effects, sound);
                    } else if (const auto* cue = flying.shot.data->sound(damage.hitSound);
                               cue != nullptr && sound) {
                        const std::string name = cue->soundFor(flying.shot.realm);
                        if (!name.empty()) {
                            sound(name);
                        }
                    }
                    effects.stop(flying.effect);
                    flying.effect = 0;
                    break;
                }
                if (wall && !item && (damage.flags & kReflect) != 0) {
                    effects.snapPresentation(flying.effect);
                    if (glm::dot(flying.velocity, normal) < 0) {
                        flying.velocity = glm::reflect(flying.velocity, normal);
                        if (flying.velocity.y > 0) {
                            flying.velocity.y *= kBounceLift;
                        }
                        effects.shortenLifetime(flying.effect, kBounceTimeLoss, kBounceLifetime);
                    }
                    continue;
                }
                if (!wall && !item && victim != nullptr) {
                    if (flying.piercedPlayer == victim->player) {
                        effects.stop(flying.effect);
                        flying.effect = 0;
                        break;
                    }
                    const f32 speed = glm::length(flying.velocity);
                    const f32 hitDamage = damage.damage * flying.shot.damageScale;
                    const bool pierces =
                        (damage.flags & kPassThrough) != 0 && flying.piercedPlayer < 0;
                    const f32 hitGap =
                        hitDamage > 2
                            ? projectileHitGap(*flying.archive,
                                               flying.shot.data->sound(damage.hitSound), pierces)
                            : 0;
                    m_hits.push_back({victim->player, hitDamage, damage.flags,
                                      speed > 0.0f ? flying.velocity / speed : Vec3{0.0f}, hitGap,
                                      flying.shot.critter, flying.shot.data->kind()});
                    if ((damage.flags & kPassThrough) != 0 && flying.piercedPlayer < 0) {
                        // Reflecting super shots leave an impact and spend their
                        // pass-through bit; ordinary super shots keep travelling.
                        if ((damage.flags & kReflect) != 0 && damage.hitSound >= 0 &&
                            damage.damage * flying.shot.damageScale > 2) {
                            show(flying, damage.hitSound, device, effects, sound);
                            flying.piercedPlayer = victim->player;
                        }
                        contacted.push_back(victim->player);
                        flying.position = to;
                        continue;
                    }
                }
                const PlaySound impactSound = liquid && !item ? PlaySound{} : sound;
                effects.stop(flying.effect);
                if ((damage.flags & kSticky) != 0 && damage.hitSound >= 0) {
                    flying.stuck = true;
                    flying.velocity = Vec3{0};
                    flying.spin = Vec3{0};
                    flying.rotation = Vec3{0};
                    const auto* impact = flying.shot.data->sound(damage.hitSound);
                    if (collision != nullptr && impact != nullptr && (impact->flags & 0x10U) != 0) {
                        if (const auto floor = collision->floorAt(
                                flying.position, 1 + radius * 0.5f, 5 + radius * 0.5f)) {
                            flying.position.y = floor->y + kFloorClearance;
                            flying.rotation.x = std::atan2(floor->normal.z, floor->normal.y);
                            flying.rotation.z = -std::atan2(
                                floor->normal.x, std::hypot(floor->normal.y, floor->normal.z));
                        }
                    }
                    flying.effect =
                        show(flying, damage.hitSound, device, effects, impactSound, kStickyLife);
                    break;
                }
                const u32 impact = show(flying, damage.hitSound, device, effects, impactSound);
                if (settleImpact(flying, impact, effects, collision)) {
                    // The impact owns a new damage lifetime. It is not an
                    // immediate full-radius second hit on the contact frame.
                } else if (flying.leavesGenerator) {
                    flying.settled = true;
                    flying.effect = impact;
                    if (impact == 0) {
                        m_generators.push_back(glm::translate(Mat4{1}, flying.position));
                    }
                } else {
                    flying.effect = 0;
                }
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

bool CombatantProjectiles::settleImpact(Flying& flying, u32 effect, EffectTrees& effects,
                                        const WorldCollision* collision) {
    const AttackDefinition& damage = *flying.shot.data->damage(flying.shot.damageIndex);
    const auto life = effects.remaining(effect);
    if (!life.has_value() || *life <= 0) {
        return false;
    }
    const auto* impact = flying.shot.data->sound(damage.hitSound);
    const bool alignToFloor = impact != nullptr && (impact->flags & 0x10U) != 0;
    if (flying.impactRadius <= 0 && !alignToFloor) {
        return false; // ordinary visual-only impacts keep the pose chosen by show()
    }
    flying.velocity = flying.spin = Vec3{0};
    flying.rotation = Vec3{0};
    if (collision != nullptr && alignToFloor) {
        const f32 radius = damage.radius * flying.shot.scale * 0.5f;
        if (const auto floor = collision->floorAt(flying.position, 1 + radius, 5 + radius)) {
            flying.position.y = floor->y + kFloorClearance;
            flying.rotation.x = std::atan2(floor->normal.z, floor->normal.y);
            flying.rotation.z =
                -std::atan2(floor->normal.x, std::hypot(floor->normal.y, floor->normal.z));
        }
    }
    // Floor alignment belongs to the impact's SFXX, even after the flight's
    // blast radius has been cleared. It must not inherit arrow-facing either.
    Mat4 placement = glm::translate(Mat4{1}, flying.position);
    placement = glm::rotate(placement, flying.rotation.x, Vec3{1, 0, 0});
    placement = glm::rotate(placement, flying.rotation.z, Vec3{0, 0, 1});
    effects.placeAt(effect, placement);
    if (flying.impactRadius <= 0) {
        return false;
    }
    CritterArea area;
    area.radius = flying.impactRadius;
    area.minDot = damage.minDot;
    area.damage = damage.damage * flying.shot.damageScale;
    area.flags = damage.flags;
    area.lifetime = area.secondsLeft = *life;
    area.expanding = true;
    flying.impactArea = area;
    flying.effect = effect;
    flying.settled = true;
    return true;
}

void CombatantProjectiles::impactContacts(Flying& flying, f32 seconds,
                                          std::span<const EnemyView> players) {
    if (!flying.impactArea.has_value()) {
        return;
    }
    CritterArea& area = *flying.impactArea;
    area.secondsLeft -= seconds;
    const auto& damage = *flying.shot.data->damage(flying.shot.damageIndex);
    if ((damage.behaviorFlags & CombatantProjectile::kNoPlayerDamage) != 0) {
        return;
    }
    Mat4 parent = glm::translate(Mat4{1}, flying.position);
    parent = glm::rotate(parent, flying.rotation.x, Vec3{1, 0, 0});
    parent = glm::rotate(parent, flying.rotation.z, Vec3{0, 0, 1});
    for (const EnemyView& player : players) {
        if (!area.touches(parent, player)) {
            continue;
        }
        const Vec3 away = player.position - flying.position;
        const f32 length = std::hypot(away.x, away.z);
        const Vec3 push = length > 0 ? Vec3{away.x, 0, away.z} * (0.25f / length) : Vec3{0};
        u32 flags = area.flags;
        if (area.currentDamage() < 5) {
            constexpr u32 kHeavyHitFlags = 0x170;
            constexpr u32 kNoHitEffect = 0x1000000;
            flags = (flags & ~kHeavyHitFlags) | kNoHitEffect;
        }
        m_hits.push_back({player.player, area.currentDamage(), flags, push, area.hitGap(),
                          flying.shot.critter, flying.shot.data->kind()});
    }
}

void CombatantProjectiles::stickyContacts(Flying& flying, f32 seconds,
                                          std::span<const EnemyView> players) {
    constexpr f32 kContactStep = 1.0f / 30.0f;
    const AttackDefinition& damage = *flying.shot.data->damage(flying.shot.damageIndex);
    flying.contactSeconds += seconds;
    const f32 reach = std::max(0.0f, damage.maxDistance * flying.shot.scale);
    while (flying.contactSeconds >= kContactStep) {
        flying.contactSeconds -= kContactStep;
        for (const EnemyView& player : players) {
            if (!player.hidden &&
                CombatantProjectile::contact(flying.position, flying.position, reach,
                                             player.position, player.radius, player.height)) {
                m_hits.push_back({player.player, damage.damage * flying.shot.damageScale,
                                  damage.flags, Vec3{0}, 0, flying.shot.critter,
                                  flying.shot.data->kind()});
            }
        }
    }
}

void CombatantProjectiles::clear(EffectTrees& effects) {
    for (const u32 effect : m_emittedEffects) {
        effects.stop(effect);
    }
    m_emittedEffects.clear();
    m_flying.clear();
    m_hits.clear();
    m_worldHits.clear();
    m_rockHits.clear();
    m_generators.clear();
    m_summons.clear();
    m_ricochetIn = 0;
}

std::optional<CombatantProjectiles::ItemImpact>
CombatantProjectiles::hitItems(const Flying& flying, const Vec3& from, const Vec3& to, f32 limit,
                               std::span<const MissileStop> items) {
    const AttackDefinition& damage = *flying.shot.data->damage(flying.shot.damageIndex);
    if (items.empty() || (damage.behaviorFlags & CombatantProjectile::kIgnoreWorld) != 0) {
        return std::nullopt;
    }
    std::vector<std::pair<f32, const MissileStop*>> contacts;
    for (const MissileStop& item : items) {
        if (const auto at = item.box.contact(from, to, damage.radius * flying.shot.scale);
            at && *at <= limit) {
            contacts.emplace_back(*at, &item);
        }
    }
    std::ranges::stable_sort(contacts, {}, &decltype(contacts)::value_type::first);
    const bool pierces = (damage.flags & kPassThrough) != 0 && flying.piercedPlayer < 0;
    for (const auto& [at, item] : contacts) {
        if (item->rock < 0) {
            if (!pierces) {
                return ItemImpact{at, false};
            }
            continue;
        }
        // Queued damage matters for subsequent iceballs in the same update. The
        // scene applies these blows to the actual rock before the next update.
        f32 health = static_cast<f32>(item->rockHealth);
        for (const RockHit& hit : m_rockHits) {
            if (hit.rock == static_cast<usize>(item->rock)) {
                health -=
                    std::max(1.0f, std::round(hit.damage - static_cast<f32>(item->rockArmor)));
            }
        }
        if (health <= 0) {
            continue;
        }
        // SfxSkipItem: hostile effects can hit safe rocks, but heal/gas and
        // unbreakable armour do not take damage from these attacks.
        constexpr u32 kHealOrGas = 0x200 | 0x800;
        if (item->rockArmor >= 0 && (damage.flags & kHealOrGas) == 0) {
            const f32 power = damage.damage * flying.shot.damageScale;
            m_rockHits.push_back({static_cast<usize>(item->rock), power});
            health -= std::max(1.0f, std::round(power - static_cast<f32>(item->rockArmor)));
        }
        // DMG_SUPER only passes cover its hit destroyed. Surviving cover clears
        // fxhit, terminating even a reflective shot without its player-hit burst.
        if (!pierces || health > 0) {
            return ItemImpact{at, pierces};
        }
    }
    return std::nullopt;
}

void CombatantProjectiles::summon(Flying& flying) {
    if (flying.summonsEnemies) {
        m_summons.push_back(glm::rotate(glm::translate(Mat4{1}, flying.position), flying.rotation.y,
                                        Vec3{0, 1, 0}));
        flying.summonsEnemies = false;
    }
}

std::vector<Mat4> CombatantProjectiles::takeSummons() {
    return std::exchange(m_summons, {});
}

std::vector<Mat4> CombatantProjectiles::takeGenerators() {
    return std::exchange(m_generators, {});
}

std::vector<CombatantProjectileHit> CombatantProjectiles::takeHits() {
    return std::exchange(m_hits, {});
}
} // namespace gdl::game
