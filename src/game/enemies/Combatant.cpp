#include "game/enemies/Combatant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

#include "engine/core/Types.h"

#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/players/EnemyShrink.h"
namespace gdl::game {
namespace {
constexpr f32 kDrop = 6.0f;
constexpr f32 kDeathFade = 1.0f;
constexpr f32 kPushDecay = 0.8f;
constexpr f32 kPushFrameRate = 30.0f; ///< knock-back decays once per game frame
constexpr f32 kGravity = 100.0f;
} // namespace
void Combatant::clear() {
    m_children.clear();
    m_actor = Actor{};
    m_id = -1;
    m_collision = nullptr;
    m_blows.clear();
    m_grabs.clear();
    m_pushes.clear();
    m_rams.clear();
    m_tramples.clear();
    m_swarm = {};
    m_obstacles = {};
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
    m_children.clear();
    if (!spawnActor(stock, stock.data, id, position, yaw, collision, scales, realm)) {
        return false;
    }
    for (const auto& definition : stock.children) {
        auto part = std::make_unique<Combatant>();
        if (!part->spawnActor(stock, definition, id + 1 + static_cast<s32>(m_children.size()),
                              position, yaw, nullptr, scales, realm)) {
            clear();
            return false;
        }
        part->m_actor.parent = this;
        part->m_actor.branch = stock.tree->findNode(definition.rootNode());
        part->synchronizeChild();
        m_children.push_back(std::move(part));
    }
    return true;
}

bool Combatant::spawnActor(CombatantAssets& stock, const CritterData& definition, s32 id,
                           const Vec3& position, f32 yaw, const WorldCollision* collision,
                           const EnemyScales& scales, char realm) {
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
    critter.definition = &definition;
    critter.maxHealth = definition.maxHealth() * m_scales.health;
    critter.health = critter.maxHealth;
    for (const auto& part : definition.parts()) {
        HitNode node;
        node.health = part.healthScale * critter.maxHealth;
        critter.hitNodes.push_back(node);
    }
    critter.position = position;
    if (m_collision != nullptr) {
        if (const auto floor = m_collision->floorAt(position, kDrop, kDrop)) {
            critter.position.y = floor->y;
        }
    }
    critter.yaw = yaw;
    critter.initialYaw = yaw;
    critter.initialRoot = critter.position + Vec3{0.0f, definition.floorOffset(), 0.0f};
    // The table's explicit home is in model-root space; public positions are floors.
    critter.homePosition = critter.position;
    if (const auto& home = definition.movement().home; home.has_value()) {
        critter.homePosition = *home - Vec3{0.0f, definition.floorOffset(), 0.0f};
    }
    // Unused attacks are ready on arrival, independent of this instance's clock.
    // Subsequent cooldowns count from their recorded use, not the spawn time.
    const f32 unused = -std::numeric_limits<f32>::infinity();
    critter.moveTimes.assign(definition.moves().size(), unused);
    critter.patternTimes.assign(definition.patterns().size(), unused);
    // It comes in by its entrance, or its stance when it has none.
    const auto start = definition.moveOfType(MoveDefinition::kStart);
    const auto ready = definition.moveOfType(MoveDefinition::kReady);
    if (!(start.has_value() && startMove(critter, *start)) &&
        !(ready.has_value() && startMove(critter, *ready))) {
        critter = Actor{};
        return false;
    }

    return true;
}
void Combatant::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                       std::span<const Combatant> peers, bool timeStopped) {
    if (ticks <= 0) {
        return;
    }
    Actor& critter = m_actor;
    if (!present()) {
        return;
    }
    if (critter.parent == nullptr) {
        // Linked heads keep their hit-feedback clock even when the body drives their pose.
        updateHitFlashes(critter, ticks);
        for (auto& child : m_children) {
            updateHitFlashes(child->m_actor, ticks);
        }
        // CritterInitPlayerData recounts state-1 slots before processing the list.
        // This affects damage, experience and roar thresholds, not spawn population.
        // Until the first update, direct callers retain their supplied spawn count.
        std::array<bool, kPlayerSlots> standing{};
        for (const EnemyView& player : players) {
            if (!player.hidden && player.player >= 0 &&
                static_cast<usize>(player.player) < standing.size()) {
                standing[static_cast<usize>(player.player)] = true;
            }
        }
        m_scales.players = static_cast<s32>(std::ranges::count(standing, true));
        for (auto& child : m_children) {
            child->m_scales.players = m_scales.players;
        }
    }
    const s32 i = m_id;
    const CritterData& data = *critter.definition;
    critter.age += seconds;
    critter.sinceHurt += seconds;
    for (CritterArea& area : critter.areas) {
        area.secondsLeft -= seconds;
        area.displacement += area.velocity * seconds;
    }
    std::erase_if(critter.areas, [](const CritterArea& area) { return area.secondsLeft <= 0; });
    // Frozen, it stands as it is: no move, no step, no one in its sights.
    if (critter.frozenTicks > 0) {
        critter.frozenTicks = std::max(critter.frozenTicks - ticks, 0);
        aimGaze(critter, seconds, players);
        holdBrokenPoses(critter);
        updateAreas(critter, i, players);
        carryGrab(critter, players);
        return;
    }
    const bool stopped = timeStopped && critter.state == State::Active;
    if (stopped && critter.move >= 0 &&
        data.moves()[static_cast<usize>(critter.move)].type != MoveDefinition::kStart) {
        updateAreas(critter, i, players); // Already-created effects keep their own clock.
        carryGrab(critter, players);
        return;
    }
    if (critter.state == State::Active) {
        // The lookout it makes for, gone on from any reached (CritterGetTarget).
        critter.patrolAim = critter.patrol.aim(critter.position);
        if (critter.parent == nullptr) {
            chooseFamilyTargets(players);
        }
        if (critter.blindTicks > 0) {
            critter.blindTicks = std::max(critter.blindTicks - ticks, 0);
            critter.target = -1;
        }
    }
    // CritterGolemAI still animates START/DEATH during Stop Time. Follow an
    // entrance's explicit continuation, without running ready/attack selection.
    if (!stopped || critter.move < 0) {
        chooseMove(critter, players);
    } else if (critter.moveDone) {
        const s32 link = data.moves()[static_cast<usize>(critter.move)].link;
        if (link >= 0) {
            startMove(critter, static_cast<usize>(link));
        }
        updateAreas(critter, i, players);
        return;
    }
    if (critter.grabbed >= 0 &&
        (critter.state != State::Active || critter.grabMove != critter.move)) {
        if (critter.state == State::Active && critter.move >= 0 &&
            data.moves()[static_cast<usize>(critter.move)].type == MoveDefinition::kGrab) {
            critter.grabMove = critter.move;
        } else {
            m_grabs.push_back({critter.grabbed, i, std::nullopt, Vec3{0}, 0});
            critter.grabbed = -1;
        }
    }
    const MoveDefinition* move =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // CritterMoveSetup retains +0x124 until the move changes or has no target.
    // A fresh roster (including hidden/departed-player filtering) must not redirect
    // an ongoing volley or wind-up. The next move selects from that fresh roster.
    if (move != nullptr && (critter.moveTarget < 0 || critter.moveDone)) {
        critter.moveTarget = attackTarget(critter, move->target, players, true);
    }
    // The move plays; over its harmful frames its part strikes.
    critter.skinAge += seconds;
    if (move != nullptr && critter.player.playing()) {
        const bool wasFinished = critter.player.finished();
        critter.player.advance(seconds, false);
        if (wasFinished) {
            critter.finishedSeconds += seconds;
        }
        critter.moveDone = critter.player.finished() && critter.finishedSeconds >= move->hold;
        critter.pose.evaluate(*critter.stock->tree, critter.player.sequence(),
                              critter.player.frame());
        inheritBodyPose();
        aimGaze(critter, seconds, players);
        holdBrokenPoses(critter);
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
        if (!stopped && frame >= move->soundFrame) {
            giveOnce(1U, move->sound, critter.position);
        }
        if (!stopped && frame >= move->sound2Frame) {
            giveOnce(2U, move->sound2, critter.position);
        }
        if (critter.state == State::Active && !stopped) {
            // Retail move 0x88 captures Player.effectpos at its first damage frame.
            // Both the falling rock and its later impact use that same world point.
            if (move->type == MoveDefinition::kTargetArea && !critter.attackTarget.has_value() &&
                move->frameStart >= 0 && frame >= move->frameStart) {
                if (const EnemyView* target = viewOf(players, critter.moveTarget)) {
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
                constexpr u32 kSticky = 0x4000000;
                if (targeted && (harm->flags & kSticky) != 0) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        plant(critter, i, index);
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
                    ((harm->type == AttackDefinition::kRing || targeted) &&
                     supportsArea(*harm, data.sound(harm->sound)))) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        if (targeted) {
                            startArea(critter, i, *harm, {},
                                      glm::translate(Mat4{1}, *critter.attackTarget));
                        } else {
                            startArea(critter, i, *harm, move->colnode);
                        }
                    }
                    return;
                }
                if (harm->type == AttackDefinition::kBreath) {
                    if ((critter.soundsGiven & bit) == 0) {
                        critter.soundsGiven |= bit;
                        cue(critter, i, harm->sound, partPosition(critter, move->colnode),
                            move->colnode, harm);
                    }
                    strikeWith(critter, i, *move, index, players);
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
                // CritterDoDamage passes the active move node's world origin, not the floor.
                m_spews.push_back(CombatSpew{i, partPosition(critter, move->colnode),
                                             harm->spewVelocity(critter.yaw),
                                             harm->spewHalfAngle()});
            }
        }
        critter.shotFrame = frame;
    } else {
        critter.moveDone = true;
    }
    if (critter.parent == nullptr && !stopped) {
        carry(critter, seconds, move, players, peers);
    }
    carryGrab(critter, players);
    updateAreas(critter, i, players);
    critter.push *= std::pow(kPushDecay, seconds * kPushFrameRate);
    critter.push.y = std::max(critter.push.y - kGravity * seconds, 0.0f);
    if (glm::length(critter.push) < 0.01f) {
        critter.push = Vec3{0.0f, 0.0f, 0.0f};
    }
    // Great ones drop their carried item at the end of the authored death;
    // bosses keep their separate death hold for the victory sequence.
    if (critter.parent != nullptr) {
        return; // Dead branches retain their attachment for persistent stump effects.
    }
    updateChildren(ticks, seconds, players);
    if (critter.state == State::Dying && move != nullptr && move->type == MoveDefinition::kDeath &&
        move->hold > 0.0f) {
        constexpr f32 kBossDeathFade = 0.5f;
        const f32 remaining = move->hold - critter.finishedSeconds;
        critter.alpha = std::clamp(remaining / kBossDeathFade, 0.0f, 1.0f);
        if (critter.moveDone) {
            critter = Actor{};
        }
    } else if (critter.state == State::Dying &&
               critter.stock->definition.kind != CombatantKind::Boss) {
        if (move != nullptr && move->type == MoveDefinition::kDeath && !critter.moveDone) {
            const auto& sequence = critter.stock->tree->sequences[critter.player.sequence()];
            if (move->frameStart > 0 && sequence.frames > move->frameStart) {
                critter.alpha = std::clamp(
                    1.0f - (critter.player.frame() - static_cast<f32>(move->frameStart)) /
                               static_cast<f32>(sequence.frames - move->frameStart),
                    0.0f, 1.0f);
            }
        } else {
            CombatLoss drop;
            drop.critter = m_id;
            drop.kind = data.kind();
            drop.form = form();
            drop.drop = true;
            drop.position = critter.position;
            m_losses.push_back(std::move(drop));
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

f32 Combatant::hurt(const EnemyHit& hit, s32 partId) {
    if (!alive()) {
        return 0;
    }
    for (auto& part : m_children) {
        if (part->id() == partId) {
            const f32 before = part->health();
            const f32 credited = part->hurtActor(hit);
            // A lethal branch hit removes that branch, without forwarding its final hit.
            if (part->alive()) {
                loseHealth(before - part->health());
            }
            m_actor.childrenIntact =
                std::ranges::all_of(m_children, [](const auto& p) { return p->alive(); });
            collectChildEvents(*part);
            return credited;
        }
    }
    return hurtActor(hit);
}

f32 Combatant::hurtActor(const EnemyHit& hit) {
    Actor& critter = m_actor;
    const s32 id = m_id;
    if (critter.state != State::Active) {
        return 0;
    }
    const CritterData& data = *critter.definition;
    f32 amount = hit.damage;
    // A block lets a quarter through and shrugs off the throw.
    u32 flags = hit.flags;
    if (critter.move >= 0 &&
        data.moves()[static_cast<usize>(critter.move)].type == MoveDefinition::kBlock) {
        const MoveDefinition& block = data.moves()[static_cast<usize>(critter.move)];
        amount *= kBlockShare;
        flags &= ~(EnemyHit::kFloors | EnemyHit::kKnockBack);
        // A sound frame outside the animation is an on-hit cue, once per block.
        constexpr s32 kOnBlockedHit = 1000;
        constexpr u32 kMoveSoundGiven = 1U;
        if (block.soundFrame >= kOnBlockedHit && block.sound >= 0 &&
            (critter.soundsGiven & kMoveSoundGiven) == 0) {
            critter.soundsGiven |= kMoveSoundGiven;
            cue(critter, id, block.sound, critter.position);
        }
    }
    const Damage modified =
        Damage::modify(amount, flags, data.shieldFlags(), data.armor(), m_scales.bossEncounter);
    flags = modified.flags;
    amount = modified.amount;
    // Shrunk, a great one takes double (CritterDamage); a boss never shrinks.
    const bool boss = data.kind() == CombatantKind::Boss;
    if (!boss) {
        amount = EnemyShrink::harmTaken(critter.shrink, amount);
    }
    if (amount <= 0.0f) {
        return 0;
    }
    // A boss takes less the more there are to fight it, outside a legend item's rite
    // (CritterDamage's damage_mul).
    const f32 counterDamage = amount;
    critter.roarOwed += counterDamage;
    const s32 players = std::clamp(m_scales.players, 0, static_cast<s32>(kBossShares.size()) - 1);
    if (boss && !m_fullHarm) {
        amount *= kBossShares[static_cast<usize>(players)];
    }
    if (const f32 length = glm::length(hit.direction); length > 0.001f) {
        critter.hurtDirection = hit.direction / length;
    }
    // Every hit is worth its share of the creature's value to the one who dealt it (a boss's
    // times the players); then a character under the level the place is meant for does a
    // fiftieth less a level to anything but a boss (CritterDamage).
    const f32 credited = std::clamp(amount, 0.0f, critter.health);
    if (hit.player >= 0) {
        if (static_cast<usize>(hit.player) < kPlayerSlots) {
            PlayerDamage& memory = critter.playerDamage[static_cast<usize>(hit.player)];
            rememberDamage(memory.dealt, memory.dealtTime, targetClock(critter), credited);
        }
        f32 share = std::min(credited / (1.0f + critter.maxHealth), 1.0f) * data.experience();
        if (boss) {
            share *= static_cast<f32>(players);
        }
        if (!boss && m_scales.playerLevel > 0.0f &&
            static_cast<f32>(hit.level) < m_scales.playerLevel) {
            const f32 under = m_scales.playerLevel - static_cast<f32>(hit.level);
            amount *= std::max(1.0f - kUnderLevelLoss * under, 0.1f);
        }
        CombatLoss loss;
        loss.critter = id;
        loss.kind = data.kind();
        loss.player = hit.player;
        loss.experience = share;
        loss.position = critter.position;
        m_losses.push_back(loss);
    }
    amount = damageNode(hit.node, amount, flags);
    if (amount <= 0) {
        return credited;
    }
    critter.patrol.end();
    critter.patrolAim.reset();
    critter.hurtPending += counterDamage;
    critter.hurtFlags |= flags;
    critter.sinceHurt = 0.0f;
    const Vec3 where = hit.where.value_or(partPosition(critter, {}));
    const u32 element = damage::element(flags);
    if (damage::marks(flags) && element != 0) {
        CombatCue burst;
        burst.critter = id;
        burst.tree = damage::hitEffect(element, false);
        burst.position = where;
        burst.yaw = critter.yaw;
        burst.scale = damage::kHitEffectScale * data.radius() * critter.scale;
        m_cues.push_back(std::move(burst));
    } else if (damage::marks(flags)) {
        const s32 mark =
            !hit.close && data.hitSoundFar() >= 0 ? data.hitSoundFar() : data.hitSoundClose();
        cue(critter, id, mark, where);
    }
    loseHealth(amount);
    // A lethal blow goes straight to death, without starting a hit flash. Heavy surviving
    // hits flash the whole body; light hits flash only the struck collision mesh.
    if (alive() && damage::marks(flags) && (flags & kFlashesWhole) != 0) {
        critter.flashTicks = kFlashTicks;
    } else if (alive() && damage::marks(flags) && hit.node >= 0 &&
               static_cast<usize>(hit.node) < critter.hitNodes.size()) {
        critter.hitNodes[static_cast<usize>(hit.node)].flashTicks = kFlashTicks;
    }
    if (alive() && !m_children.empty()) {
        const auto count =
            std::ranges::count_if(m_children, [](const auto& part) { return part->alive(); });
        if (count > 0) {
            for (auto& part : m_children) {
                if (part->alive()) {
                    part->loseHealth(0.5f * amount / static_cast<f32>(count));
                    collectChildEvents(*part);
                }
            }
        }
        m_actor.childrenIntact =
            std::ranges::all_of(m_children, [](const auto& part) { return part->alive(); });
    }
    return credited;
}

void Combatant::damagedPlayer(s32 player, f32 amount, s32 partId) {
    Actor* actor = &m_actor;
    if (partId >= 0 && partId != m_id) {
        actor = nullptr;
        for (auto& child : m_children) {
            if (child->id() == partId) {
                actor = &child->m_actor;
                break;
            }
        }
    }
    if (actor == nullptr || actor->state != State::Active || player < 0 ||
        static_cast<usize>(player) >= kPlayerSlots || amount <= 0) {
        return;
    }
    PlayerDamage& memory = actor->playerDamage[static_cast<usize>(player)];
    rememberDamage(memory.received, memory.receivedTime, targetClock(*actor), amount);
}

void Combatant::loseHealth(f32 amount) {
    Actor& critter = m_actor;
    if (!alive() || amount <= 0.0f) {
        return;
    }
    critter.health -= amount;
    if (critter.health <= 0.0f) {
        critter.state = State::Dying;
        CombatLoss fall;
        fall.critter = m_id;
        fall.kind = data()->kind();
        fall.form = form();
        fall.player = -1;
        fall.experience = kKillShare * data()->experience();
        fall.killed = true;
        fall.position = critter.position;
        m_losses.push_back(fall);
        for (auto& part : m_children) {
            if (part->alive()) {
                part->m_actor.health = 1.0f;
            }
        }
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

void Combatant::startPatrol(const LookoutRoute* route, f32 sight) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->patrol.start(route, critter->position, sight);
        critter->patrolAim = critter->patrol.aim(critter->position);
    }
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

void Combatant::setShrink(f32 scale) {
    if (scale <= 0.0f) {
        return;
    }
    m_actor.shrink = scale;
    for (auto& child : m_children) {
        child->m_actor.shrink = scale;
    }
}

f32 Combatant::dealt(f32 amount) const {
    return kind() == CombatantKind::Boss ? amount : EnemyShrink::harmDealt(m_actor.shrink, amount);
}

void Combatant::hold(bool held) {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->held = held;
    }
}

f32 Combatant::roarThreshold(s32 players) {
    const auto count =
        static_cast<usize>(std::clamp(players, 0, static_cast<s32>(kRoarShares.size()) - 1));
    return std::floor(kRoarAfter * kRoarShares[count]);
}

void Combatant::roar() {
    if (Actor* critter = present() ? &m_actor : nullptr; critter != nullptr) {
        critter->roarWanted = true;
    }
}

} // namespace gdl::game
