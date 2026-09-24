#include "game/enemies/Combatant.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

#include "engine/core/Types.h"

#include "game/combat/Damage.h"
namespace gdl::game {
namespace {
constexpr f32 kDrop = 6.0f;
constexpr f32 kDeathFade = 1.0f;
constexpr f32 kPushDecay = 0.8f;
constexpr f32 kGravity = 100.0f;
} // namespace
void Combatant::clear() {
    m_actor = Actor{};
    m_id = -1;
    m_collision = nullptr;
    m_blows.clear();
    m_grabs.clear();
    m_losses.clear();
    m_cues.clear();
    m_spews.clear();
    m_shots.clear();
    m_arenaActivations.clear();
}
void Combatant::setArenaAnchors(std::span<const Mat4> anchors) {
    m_actor.arenaAnchors.assign(anchors.begin(), anchors.end());
}
void Combatant::setArenaTargets(std::span<const CombatArenaTarget> targets) {
    m_actor.arenaTargets.assign(targets.begin(), targets.end());
    if (!m_actor.arenaCollected && !targets.empty()) {
        std::uniform_int_distribution<s32> choose{0, static_cast<s32>(targets.size()) - 1};
        m_actor.lastArenaTarget = choose(m_arenaRandom);
        m_actor.arenaCollected = true;
    }
}
bool Combatant::raisesArenaRocks() const {
    if (const CritterData* definition = data()) {
        return std::ranges::any_of(definition->moves(), [&](const MoveDefinition& move) {
            return std::ranges::any_of(
                std::initializer_list<s32>{move.damage0, move.damage1}, [&](s32 index) {
                    const AttackDefinition* damage = definition->damage(index);
                    return damage != nullptr && damage->type == AttackDefinition::kArenaEruption;
                });
        });
    }
    return false;
}
bool Combatant::spawn(CombatantAssets& stock, s32 id, const Vec3& position, f32 yaw,
                      const WorldCollision* collision, const EnemyScales& scales, char realm) {
    m_actor = Actor{};
    m_id = -1;
    m_collision = nullptr;
    if (stock.tree == nullptr) {
        return false;
    }
    m_id = id;
    m_collision = collision;
    m_scales = scales;
    m_realm = realm;
    Actor& critter = m_actor;
    critter.state = State::Active;
    critter.stock = &stock;
    critter.maxHealth = stock.data.maxHealth() * m_scales.health;
    critter.health = critter.maxHealth;
    critter.position = position;
    if (m_collision != nullptr) {
        if (const auto floor = m_collision->floorAt(position, kDrop, kDrop)) {
            critter.position.y = floor->y;
        }
    }
    critter.yaw = yaw;
    critter.initialYaw = yaw;
    critter.initialRoot = critter.position + Vec3{0.0f, stock.data.floorOffset(), 0.0f};
    // The table's explicit home is in model-root space; public positions are floors.
    critter.homePosition = critter.position;
    if (const auto& home = stock.data.movement().home; home.has_value()) {
        critter.homePosition = *home - Vec3{0.0f, stock.data.floorOffset(), 0.0f};
    }
    critter.cooldowns.assign(stock.data.moves().size(), 0.0f);
    // Unused attacks are ready on arrival, independent of this instance's clock.
    // Subsequent cooldowns count from their recorded use, not the spawn time.
    const f32 unused = -std::numeric_limits<f32>::infinity();
    critter.moveTimes.assign(stock.data.moves().size(), unused);
    critter.patternTimes.assign(stock.data.patterns().size(), unused);
    // It comes in by its entrance, or its stance when it has none.
    const auto start = stock.data.moveOfType(MoveDefinition::kStart);
    const auto ready = stock.data.moveOfType(MoveDefinition::kReady);
    if (!(start.has_value() && startMove(critter, *start)) &&
        !(ready.has_value() && startMove(critter, *ready))) {
        critter = Actor{};
        return false;
    }

    return true;
}
void Combatant::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                       std::span<const Combatant> peers) {
    if (ticks <= 0) {
        return;
    }
    Actor& critter = m_actor;
    if (!present()) {
        return;
    }
    const s32 i = m_id;
    const CritterData& data = critter.stock->data;
    critter.age += seconds;
    for (CritterArea& area : critter.areas) {
        area.secondsLeft -= seconds;
    }
    std::erase_if(critter.areas, [](const CritterArea& area) { return area.secondsLeft <= 0; });
    for (f32& cooldown : critter.cooldowns) {
        cooldown = std::max(cooldown - seconds, 0.0f);
    }
    // Frozen, it stands as it is: no move, no step, no one in its sights.
    if (critter.frozenTicks > 0) {
        critter.frozenTicks = std::max(critter.frozenTicks - ticks, 0);
        updateAreas(critter, i, players);
        carryGrab(critter, players);
        return;
    }
    if (critter.state == State::Active) {
        chooseTarget(critter, players);
        if (critter.blindTicks > 0) {
            critter.blindTicks = std::max(critter.blindTicks - ticks, 0);
            critter.target = -1;
        }
    }
    chooseMove(critter, players);
    if (critter.grabbed >= 0 &&
        (critter.state != State::Active || critter.grabMove != critter.move)) {
        m_grabs.push_back({critter.grabbed, i, std::nullopt, Vec3{0}, 0});
        critter.grabbed = -1;
    }
    const MoveDefinition* move =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // The move plays; over its harmful frames its part strikes.
    if (move != nullptr && critter.player.playing()) {
        const bool wasFinished = critter.player.finished();
        critter.player.advance(seconds, false);
        if (wasFinished) {
            critter.finishedSeconds += seconds;
        }
        critter.moveDone = critter.player.finished() && critter.finishedSeconds >= move->hold;
        critter.pose.evaluate(*critter.stock->tree, critter.player.sequence(),
                              critter.player.frame());
        const auto frame = static_cast<s32>(std::floor(critter.player.frame()));
        const auto active = [&](s32 start, s32 end) {
            const s32 last = end < start ? start : end;
            return start >= 0 && frame >= start && (frame <= last || critter.shotFrame < start);
        };
        // SFXX frames start sound and visuals together. The effect's own sequence
        // contains its wind-up; delaying it until the damage frame delays that twice.
        const auto giveOnce = [&](u32 bit, s32 sound, const Vec3& where) {
            if (sound >= 0 && (critter.soundsGiven & bit) == 0) {
                critter.soundsGiven |= bit;
                const auto node = (bit == 1U || bit == 2U)
                                      ? std::optional<std::string_view>{move->colnode}
                                      : std::nullopt;
                cue(critter, i, sound, where, node);
            }
        };
        if (frame >= move->soundFrame) {
            giveOnce(1U, move->sound, critter.position);
        }
        if (frame >= move->sound2Frame) {
            giveOnce(2U, move->sound2, critter.position);
        }
        if (critter.state == State::Active) {
            // Retail move 0x88 captures Player.effectpos at its first damage frame.
            // Both the falling rock and its later impact use that same world point.
            if (move->type == MoveDefinition::kTargetArea && !critter.attackTarget.has_value() &&
                move->frameStart >= 0 && frame >= move->frameStart) {
                if (const EnemyView* target = viewOf(players, critter.target)) {
                    critter.attackTarget =
                        target->position + Vec3{0.0f, 0.5f * target->height, 0.0f};
                }
            }
            const auto projectile = [&](s32 index, bool second) {
                const AttackDefinition* harm = data.damage(index);
                if (harm == nullptr || harm->type != AttackDefinition::kProjectile) {
                    return false;
                }
                const s32 count = move->projectileTriggers(critter.shotFrame, frame, second);
                for (s32 shot = 0; shot < count; ++shot) {
                    shoot(critter, i, *move, index, players);
                }
                return true;
            };
            const bool shot0 = projectile(move->damage0, false);
            const bool shot1 = projectile(move->damage1, true);
            const auto contact = [&](s32 index, s32 start, s32 end, u32 bit) {
                const AttackDefinition* harm = data.damage(index);
                if (harm == nullptr) {
                    return;
                }
                const bool targeted = harm->type == AttackDefinition::kTargetArea;
                const bool crossed = start >= 0 && critter.shotFrame < start && frame >= start;
                if ((!active(start, end) && !(targeted && crossed)) ||
                    (targeted && !critter.attackTarget.has_value())) {
                    return;
                }
                if (harm->type == AttackDefinition::kArenaEruption) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        eruptArena(critter, i, *harm, players);
                    }
                    return;
                }
                if (harm->type == AttackDefinition::kGrab) {
                    // First window catches continuously; second is a single release edge.
                    if (bit != 8U || (critter.soundsGiven & bit) == 0) {
                        grab(critter, *move, *harm, bit == 8U, players);
                        if (bit == 8U) {
                            critter.soundsGiven |= bit;
                        }
                    }
                    return;
                }
                if (harm->type == AttackDefinition::kArenaAreas) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        for (const Mat4& anchor : critter.arenaAnchors) {
                            startArea(critter, i, *harm, {}, anchor);
                        }
                    }
                    return;
                }
                if (harm->type == AttackDefinition::kAttachedArea ||
                    (harm->type == AttackDefinition::kRing &&
                     supportsArea(*harm, data.sound(harm->sound)))) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        startArea(critter, i, *harm, move->colnode);
                    }
                    return;
                }
                const Vec3 where = targeted ? *critter.attackTarget + Vec3{modelTransform(critter) *
                                                                           Vec4{harm->offset, 0.0f}}
                                            : partPosition(critter, move->colnode) + harm->offset;
                giveOnce(bit, harm->sound, where);
                strikeWith(critter, i, *move, index, players);
            };
            if (!shot0) {
                contact(move->damage0, move->frameStart, move->frameEnd, 4U);
            }
            if (!shot1) {
                contact(move->damage1, move->frameStart2, move->frameEnd2, 8U);
            }
        } else if (critter.state == State::Dying && active(move->frameStart, move->frameEnd) &&
                   (critter.soundsGiven & 32U) == 0) {
            // The death's harm is not a strike but a throw: what it spews goes out
            // once, the moment its frame comes.
            if (const AttackDefinition* harm = data.damage(move->damage0);
                harm != nullptr && harm->type == AttackDefinition::kSpew) {
                critter.soundsGiven |= 32U;
                m_spews.push_back(CombatSpew{i, critter.position, harm->spewVelocity(critter.yaw),
                                             harm->spewHalfAngle()});
            }
        }
        critter.shotFrame = frame;
    } else {
        critter.moveDone = true;
    }
    carry(critter, seconds, move, players, peers);
    carryGrab(critter, players);
    updateAreas(critter, i, players);
    critter.push *= std::pow(kPushDecay, static_cast<f32>(ticks));
    critter.push.y = std::max(critter.push.y - kGravity * seconds, 0.0f);
    if (glm::length(critter.push) < 0.01f) {
        critter.push = Vec3{0.0f, 0.0f, 0.0f};
    }
    // The fallen fades once its death has played out, and is gone.
    if (critter.state == State::Dying && move != nullptr && move->type == MoveDefinition::kDeath &&
        move->hold > 0.0f) {
        constexpr f32 kBossDeathFade = 0.5f;
        const f32 remaining = move->hold - critter.finishedSeconds;
        critter.alpha = std::clamp(remaining / kBossDeathFade, 0.0f, 1.0f);
        if (critter.moveDone) {
            critter = Actor{};
        }
    } else if (critter.state == State::Dying &&
               (move == nullptr || move->type != MoveDefinition::kDeath || critter.moveDone)) {
        critter.alpha -= seconds / kDeathFade;
        if (critter.alpha <= 0.0f) {
            critter = Actor{};
        }
    }
}

void Combatant::hurt(const EnemyHit& hit) {
    Actor& critter = m_actor;
    const s32 id = m_id;
    if (critter.state != State::Active) {
        return;
    }
    const CritterData& data = critter.stock->data;
    f32 amount = hit.damage;
    // A block lets a quarter through and shrugs off the throw.
    u32 flags = hit.flags;
    if (critter.move >= 0 &&
        data.moves()[static_cast<usize>(critter.move)].type == MoveDefinition::kBlock) {
        amount *= kBlockShare;
        flags &= ~(EnemyHit::kFloors | EnemyHit::kKnockBack);
    }
    const Damage modified = Damage::modify(amount, flags, data.shieldFlags(), data.armor(),
                                           data.kind() == CombatantKind::Boss);
    flags = modified.flags;
    amount = std::max(modified.amount, hit.player >= 0 ? 1.0f : 0.0f);
    if (amount <= 0.0f) {
        return;
    }
    critter.health -= amount;
    critter.hurtPending += amount;
    critter.hurtFlags |= flags;
    critter.roarOwed += amount;
    if (const f32 length = glm::length(hit.direction); length > 0.001f) {
        critter.hurtDirection = hit.direction / length;
    }
    // Where it was struck, its own mark of a hit: a blow's or a missile's.
    const s32 mark =
        hit.close && data.hitSoundClose() >= 0 ? data.hitSoundClose() : data.hitSoundFar();
    cue(critter, id, mark, hit.where.value_or(partPosition(critter, {})));
    // Every hit is worth its share of the creature's value to the one who dealt it, less a
    // fiftieth a level under the level the place is meant for.
    if (hit.player >= 0) {
        f32 share = amount / (1.0f + critter.maxHealth) * data.experience();
        if (m_scales.playerLevel > 0.0f && static_cast<f32>(hit.level) < m_scales.playerLevel) {
            const f32 under = m_scales.playerLevel - static_cast<f32>(hit.level);
            share *= std::max(1.0f - kUnderLevelLoss * under, 0.1f);
        }
        CombatLoss loss;
        loss.critter = id;
        loss.kind = data.kind();
        loss.player = hit.player;
        loss.experience = share;
        loss.position = critter.position;
        m_losses.push_back(loss);
    }
    if (critter.health <= 0.0f) {
        critter.state = State::Dying;
        CombatLoss fall;
        fall.critter = id;
        fall.kind = data.kind();
        fall.form = form();
        fall.player = -1;
        fall.experience = kKillShare * data.experience();
        fall.killed = true;
        fall.position = critter.position;
        m_losses.push_back(fall);
    }
}

std::vector<CombatBlow> Combatant::takeBlows() {
    return std::exchange(m_blows, {});
}
std::vector<CombatGrab> Combatant::takeGrabs() {
    return std::exchange(m_grabs, {});
}

std::vector<CombatLoss> Combatant::takeLosses() {
    return std::exchange(m_losses, {});
}

std::vector<CombatCue> Combatant::takeCues() {
    return std::exchange(m_cues, {});
}

std::vector<CombatSpew> Combatant::takeSpews() {
    return std::exchange(m_spews, {});
}

std::vector<CombatShot> Combatant::takeShots() {
    return std::exchange(m_shots, {});
}
std::vector<CombatArenaActivation> Combatant::takeArenaActivations() {
    return std::exchange(m_arenaActivations, {});
}

void Combatant::freeze(s32 ticks) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->frozenTicks = std::max(ticks, 0);
        critter->roarWanted = false;
    }
}

void Combatant::blind(s32 ticks) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->blindTicks = std::max(ticks, 0);
        critter->target = -1;
    }
}

void Combatant::curb(f32 seconds) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->curbSeconds = std::max(seconds, 0.0f);
    }
}

void Combatant::resize(f32 scale) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr && scale > 0.0f) {
        critter->scale = scale;
    }
}

void Combatant::hold(bool held) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->held = held;
    }
}

void Combatant::roar() {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->roarWanted = true;
    }
}

} // namespace gdl::game
