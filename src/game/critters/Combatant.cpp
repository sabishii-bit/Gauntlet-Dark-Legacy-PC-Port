#include "game/critters/Combatant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"
#include "engine/math/RandomDistribution.h"

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
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_sim.childCount = 0;
    m_sim.actor = Actor{};
    m_sim.pressure = {};
    m_sim.id = -1;
    m_sim.collision = nullptr;
    m_sim.blows.clear();
    m_sim.grabs.clear();
    m_sim.pushes.clear();
    m_sim.rams.clear();
    m_sim.tramples.clear();
    m_swarm = {};
    m_obstacles = {};
    m_sim.losses.clear();
    m_sim.cues.clear();
    m_sim.spews.clear();
    m_sim.shots.clear();
    m_sim.arenaActivations.clear();
    m_sim.assets.clear();
}
void Combatant::setArenaAnchors(std::span<const Mat4> anchors) {
    m_sim.actor.arenaAnchors.assign(anchors.begin(), anchors.end());
}
void Combatant::setArenaTargets(std::span<const CombatArenaTarget> targets) {
    m_sim.actor.arenaTargets.assign(targets.begin(), targets.end());
    if (!m_sim.actor.arenaCollected && !targets.empty()) {
        // The ordered native roster selects an index, not a library-specific
        // distribution. Its engine and accepted/rejected draw counts stay intact.
        m_sim.actor.lastArenaTarget = static_cast<s32>(
            random_distribution::boundedIndex(m_sim.arenaRandom, targets.size()).value());
        m_sim.actor.arenaCollected = true;
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
void Combatant::captureEncounterPlayers(std::span<const EnemyView> players) {
    if (m_sim.actor.parent != nullptr || m_sim.actor.definition == nullptr ||
        m_sim.actor.definition->rules().pressureReferenceDamage <= 0) {
        return;
    }
    m_sim.pressure.refresh(players);
    m_sim.pressure.capture(players, m_sim.actor.definition->rules().pressureReferenceDamage);
}
bool Combatant::spawn(CombatantAssets& stock, s32 id, const Vec3& position, f32 yaw,
                      const WorldCollision* collision, const EnemyScales& scales, char realm) {
    if (m_sim.incarnation == std::numeric_limits<u64>::max()) {
        return false;
    }
    for (auto& part : children()) {
        part->clear();
    }
    m_sim.childCount = 0;
    if (!spawnActor(stock, stock.data, id, position, yaw, collision, scales, realm)) {
        return false;
    }
    for (const auto& definition : stock.children) {
        if (m_sim.childCount == m_children.size()) {
            m_children.push_back(std::make_unique<Combatant>());
        }
        auto& part = m_children[m_sim.childCount];
        // A new branch has fresh clocks, RNG and output queues, just as a newly
        // allocated child did. Only its storage/lifetime identity is retained.
        part->m_sim = Simulation{};
        if (!part->spawnActor(stock, definition, id + 1 + static_cast<s32>(children().size()),
                              position, yaw, nullptr, scales, realm)) {
            clear();
            return false;
        }
        part->m_sim.actor.parent = this;
        part->m_sim.actor.branch =
            stock.tree->findNode(definition.rootNode(), kCombatantNodeNameLength);
        part->synchronizeChild();
        ++m_sim.childCount;
    }
    ++m_sim.incarnation;
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
bool Combatant::saveLocal(LocalSnapshot& snapshot) const {
    snapshot.m_owner.reset();
    if (!m_obstacles.empty() || !m_swarm.empty() ||
        std::ranges::any_of(m_sim.assets, [](const auto& asset) { return asset.expired(); })) {
        return false;
    }
    snapshot.m_state = m_sim;
    snapshot.m_epoch = m_localEpoch;
    // Keep warmed child snapshots even while the current incarnation has fewer
    // parts. Neither inactive retained slots nor their stale assets are visited.
    if (snapshot.m_children.size() < m_sim.childCount) {
        snapshot.m_children.resize(m_sim.childCount);
    }
    for (usize i = 0; i < m_sim.childCount; ++i) {
        if (!m_children[i]->saveLocal(snapshot.m_children[i])) {
            return false;
        }
    }
    snapshot.m_owner = m_localOwner;
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
bool Combatant::canRestoreLocal(const LocalSnapshot& snapshot) const {
    if (snapshot.m_owner.lock() != m_localOwner || snapshot.m_epoch != m_localEpoch ||
        !m_obstacles.empty() || !m_swarm.empty() ||
        snapshot.m_state.childCount > m_children.size() ||
        snapshot.m_state.childCount > snapshot.m_children.size() ||
        std::ranges::any_of(snapshot.m_state.assets,
                            [](const auto& asset) { return asset.expired(); })) {
        return false;
    }
    for (usize i = 0; i < snapshot.m_state.childCount; ++i) {
        if (!m_children[i]->canRestoreLocal(snapshot.m_children[i])) {
            return false;
        }
    }
    return true;
}

void Combatant::appendSimulationDigest(NativeStateDigest& digest, const Simulation& state,
                                       const Combatant* expectedParent) {
    // Bind the actual records in full so adding a native field requires updating
    // this visitor. No portable checkpoint representation participates here.
    const auto& [actor, incarnation, childCount, id, collision, hazards, scales, realm, fullHarm,
                 pressure, arenaRandom, cues, grabs, spews, shots, arenaActivations, tramples, rams,
                 pushes, blows, losses, assets] = state;
    const auto& [actorState, hidden, forcedPattern, childrenIntact, smoothValid, stock, definition,
                 parent, branch, health, burnGap, maxHealth, position, floor, yaw, initialYaw,
                 homePosition, initialRoot, push, target, targetDistance, targetYaw, targets,
                 playerDamage, move, moveTarget, pattern, finishedSeconds, age, moveTimes,
                 patternTimes, patternStep, struckThisMove, areas, arenaAnchors, arenaTargets,
                 patrol, lastArenaTarget, hurtPending, hurtFlags, hurtDirection, roarOwed,
                 sinceHurt, flashTicks, skin, skinAge, hitNodes, modelReplacements, alpha, tint,
                 scale, shrink, frozenTicks, blindTicks, curbSeconds, held, roarWanted, moveEffect,
                 moveDone, soundsGiven, arenaCollected, shotFrame, attackTarget, stepTarget,
                 patrolAim, grabbed, grabMove, grabNode, grabOffset, player, pose, smooth,
                 presentation, attachments, gaze] = actor;
    if (parent != expectedParent ||
        std::ranges::any_of(assets, [](const auto& asset) { return asset.expired(); })) {
        digest.invalidate();
        return;
    }
    digest.values(actorState, hidden, forcedPattern, childrenIntact, smoothValid);
    digest.resource(stock);
    digest.resource(definition);
    // The ordered recursive traversal identifies the parent structurally. Never
    // read its current state: a saved child's parent may be on a wrong branch.
    digest.flag(parent != nullptr);
    digest.values(branch, health, burnGap, maxHealth, position, floor, yaw, initialYaw,
                  homePosition, initialRoot, push, target, targetDistance, targetYaw, targets,
                  playerDamage, move, moveTarget, pattern, finishedSeconds, age, moveTimes,
                  patternTimes, patternStep, struckThisMove, areas, arenaAnchors, arenaTargets,
                  patrol, lastArenaTarget, hurtPending, hurtFlags, hurtDirection, roarOwed,
                  sinceHurt, flashTicks);
    digest.resource(skin);
    digest.values(skinAge, hitNodes, modelReplacements, alpha, tint, scale, shrink, frozenTicks,
                  blindTicks, curbSeconds, held, roarWanted, moveEffect, moveDone, soundsGiven,
                  arenaCollected, shotFrame, attackTarget, stepTarget, patrolAim, grabbed, grabMove,
                  grabNode, grabOffset, player, pose, smooth, presentation, attachments, gaze,
                  incarnation, childCount, id);
    digest.resource(collision);
    digest.resource(hazards);
    digest.values(scales, realm, fullHarm, pressure, arenaRandom, cues, grabs, spews, shots,
                  arenaActivations, tramples, rams, pushes, blows, losses);
    // assets holds lifetime fences, including old incarnations with pending
    // outputs. Validate them above; hash the actual bound definitions/resources
    // at their uses, never token allocation identity or retained cache history.
}

bool Combatant::appendNativeDigest(NativeStateDigest& digest) const {
    return appendNativeDigest(digest, nullptr);
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
bool Combatant::appendNativeDigest(NativeStateDigest& digest,
                                   const Combatant* expectedParent) const {
    if (!m_obstacles.empty() || !m_swarm.empty() || m_sim.childCount > m_children.size()) {
        digest.invalidate();
        return false;
    }
    appendSimulationDigest(digest, m_sim, expectedParent);
    for (usize i = 0; i < m_sim.childCount && digest.good(); ++i) {
        m_children[i]->appendNativeDigest(digest, this);
    }
    return digest.good();
}

bool Combatant::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& snapshot) const {
    if (!canRestoreLocal(snapshot)) {
        digest.invalidate();
        return false;
    }
    return appendLocalDigest(digest, snapshot, nullptr);
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
bool Combatant::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& snapshot,
                                  const Combatant* expectedParent) const {
    appendSimulationDigest(digest, snapshot.m_state, expectedParent);
    for (usize i = 0; i < snapshot.m_state.childCount && digest.good(); ++i) {
        m_children[i]->appendLocalDigest(digest, snapshot.m_children[i], this);
    }
    return digest.good();
}

bool Combatant::Actor::equalLocal(const Actor& other) const {
    // These select retained art, damage definitions and native family ownership.
    // Compare their identity, never the shared model's mutable draw caches.
    if (stock != other.stock || definition != other.definition || parent != other.parent ||
        skin != other.skin) {
        return false;
    }
    const auto placement = [](const Actor& a) {
        return std::tie(a.state, a.hidden, a.forcedPattern, a.childrenIntact, a.smoothValid,
                        a.branch, a.health, a.burnGap, a.maxHealth, a.position, a.floor, a.yaw,
                        a.initialYaw, a.homePosition, a.initialRoot, a.push, a.target,
                        a.targetDistance, a.targetYaw, a.targets, a.playerDamage);
    };
    const auto moves = [](const Actor& a) {
        return std::tie(a.move, a.moveTarget, a.pattern, a.finishedSeconds, a.age, a.moveTimes,
                        a.patternTimes, a.patternStep, a.struckThisMove, a.areas, a.arenaAnchors,
                        a.arenaTargets, a.lastArenaTarget);
    };
    const auto reactions = [](const Actor& a) {
        return std::tie(a.hurtPending, a.hurtFlags, a.hurtDirection, a.roarOwed, a.sinceHurt,
                        a.flashTicks, a.skinAge, a.hitNodes, a.modelReplacements, a.alpha, a.tint,
                        a.scale, a.shrink, a.frozenTicks, a.blindTicks, a.curbSeconds, a.held,
                        a.roarWanted, a.moveEffect, a.moveDone, a.soundsGiven, a.arenaCollected,
                        a.shotFrame, a.attackTarget, a.stepTarget, a.patrolAim, a.grabbed,
                        a.grabMove, a.grabNode, a.grabOffset);
    };
    const auto drawing = [](const Presentation& p) {
        return std::tie(p.valid, p.state, p.generation, p.sequence, p.position, p.yaw, p.frame);
    };
    return placement(*this) == placement(other) && moves(*this) == moves(other) &&
           reactions(*this) == reactions(other) && patrol.equalLocal(other.patrol) &&
           gaze.equalLocal(other.gaze) && player.equalLocal(other.player) &&
           pose.equalLocal(other.pose) && smooth.equalLocal(other.smooth) &&
           drawing(presentation) == drawing(other.presentation) &&
           presentation.pose.equalLocal(other.presentation.pose) &&
           std::ranges::equal(
               attachments, other.attachments, [](const Attachment& a, const Attachment& b) {
                   return std::tie(a.world, a.previousFrame, a.previousGeneration) ==
                              std::tie(b.world, b.previousFrame, b.previousGeneration) &&
                          a.player.equalLocal(b.player) && a.pose.equalLocal(b.pose);
               });
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
bool Combatant::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    if (!canRestoreLocal(left) || !canRestoreLocal(right)) {
        return false;
    }
    const auto& a = left.m_state;
    const auto& b = right.m_state;
    const auto values = [](const Simulation& s) {
        return std::tie(s.incarnation, s.childCount, s.id, s.collision, s.hazards, s.scales,
                        s.realm, s.fullHarm, s.pressure, s.arenaRandom, s.cues, s.grabs, s.spews,
                        s.shots, s.arenaActivations, s.tramples, s.rams, s.pushes, s.blows,
                        s.losses);
    };
    if (values(a) != values(b) || !a.actor.equalLocal(b.actor) ||
        !std::ranges::equal(a.assets, b.assets, [](const auto& first, const auto& second) {
            return first.lock() == second.lock();
        })) {
        return false;
    }
    for (usize i = 0; i < a.childCount; ++i) {
        if (!m_children[i]->equalLocal(left.m_children[i], right.m_children[i])) {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
void Combatant::restoreLocalValues(const LocalSnapshot& snapshot) {
    m_sim = snapshot.m_state;
    for (usize i = 0; i < m_sim.childCount; ++i) {
        m_children[i]->restoreLocalValues(snapshot.m_children[i]);
    }
}

bool Combatant::restoreLocal(const LocalSnapshot& snapshot) {
    if (!canRestoreLocal(snapshot)) {
        return false;
    }
    restoreLocalValues(snapshot);
    return true;
}

bool Combatant::spawnActor(CombatantAssets& stock, const CritterData& definition, s32 id,
                           const Vec3& position, f32 yaw, const WorldCollision* collision,
                           const EnemyScales& scales, char realm) {
    m_sim.actor = Actor{};
    m_sim.pressure = {};
    m_sim.id = -1;
    m_sim.collision = nullptr;
    if (stock.tree == nullptr) {
        return false;
    }
    if (std::ranges::none_of(m_sim.assets, [&](const auto& asset) {
            return asset.lock() == stock.m_localBinding;
        })) {
        m_sim.assets.push_back(stock.m_localBinding);
    }
    m_sim.id = id;
    m_sim.collision = collision;
    m_sim.scales = scales;
    m_sim.realm = realm;
    Actor& critter = m_sim.actor;
    critter.state = State::Active;
    critter.stock = &stock;
    critter.definition = &definition;
    critter.maxHealth = definition.maxHealth() * m_sim.scales.health;
    critter.health = critter.maxHealth;
    for (const auto& part : definition.parts()) {
        HitNode node;
        node.health = part.healthScale * critter.maxHealth;
        critter.hitNodes.push_back(node);
    }
    critter.position = position;
    if (m_sim.collision != nullptr) {
        if (const auto floor = m_sim.collision->floorAt(position, kDrop, kDrop)) {
            critter.position.y = floor->y;
        }
    }
    critter.yaw = yaw;
    rememberFloor();
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
    // CritterAddAnimInsts copies the initial orientation, then replaces the
    // translation with ADDA's offset. Unattached trees remain in the world;
    // attached ones inherit their named body node at draw time.
    if (&definition == &stock.data) {
        critter.attachments.resize(stock.attachments.size());
        for (usize j = 0; j < stock.attachments.size(); ++j) {
            critter.attachments[j].world =
                portable::rotate(glm::translate(Mat4{1}, stock.attachments[j].definition.offset),
                                 yaw, Vec3{0, 1, 0});
        }
    }
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
void Combatant::prepareEntrance() {
    Actor& actor = m_sim.actor;
    if (actor.stock == nullptr || !actor.stock->initialMoveBeforeWake) {
        return;
    }
    if (const auto initial = actor.definition->moveOfType(MoveDefinition::kInit)) {
        startMove(actor, *initial, false);
    }
    for (auto& part : children()) {
        part->prepareEntrance();
        part->synchronizeChild();
    }
}

void Combatant::beginEntrance() {
    Actor& actor = m_sim.actor;
    if (actor.stock == nullptr || !actor.stock->initialMoveBeforeWake) {
        return;
    }
    if (moveType() == MoveDefinition::kInit) {
        if (const auto start = actor.definition->moveOfType(MoveDefinition::kStart)) {
            startMove(actor, *start);
        }
    }
    for (auto& part : children()) {
        part->beginEntrance();
        part->synchronizeChild();
    }
}

void Combatant::advanceEntrance(f32 seconds) {
    Actor& actor = m_sim.actor;
    if (!present() || actor.stock == nullptr || !actor.stock->initialMoveBeforeWake ||
        moveType() != MoveDefinition::kInit || !actor.player.playing() || seconds <= 0) {
        return;
    }
    // N64 Boss.bin e08842e8 still calls e0882b38 while state0 waits for the
    // party. e0882f9c keeps INIT selected: a looping REDY (Chimera) animates,
    // while a one-frame INIT (Skorne) remains unchanged. Do not run combat AI
    // here: the deadline/distance gate still owns the transition to START.
    capturePresentation();
    const auto& sequence = actor.stock->tree->sequences[actor.player.sequence()];
    actor.player.advance(seconds, sequence.repeats);
    actor.pose.evaluate(*actor.stock->tree, actor.player.sequence(), actor.player.frame());
    actor.smooth = smoothPose(actor);
    actor.smoothValid = true;
    for (auto& part : children()) {
        part->synchronizeChild();
        part->m_sim.actor.smooth = actor.smooth;
        part->m_sim.actor.smoothValid = true;
    }
}

void Combatant::update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                       std::span<const Combatant> peers, bool timeStopped) {
    if (m_sim.actor.parent == nullptr) {
        capturePresentation();
    }
    updateActor(ticks, seconds, players, peers, timeStopped);
    if (m_sim.actor.parent == nullptr) {
        // Sample fractional animation once per simulation update, not once per
        // draw, shadow, attachment or monitor refresh.
        m_sim.actor.smooth = smoothPose(m_sim.actor);
        m_sim.actor.smoothValid = true;
        for (auto& child : children()) {
            child->m_sim.actor.smooth = smoothPose(child->m_sim.actor);
            child->m_sim.actor.smoothValid = true;
        }
    }
}

void Combatant::updateActor(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                            std::span<const Combatant> peers, bool timeStopped) {
    syncFloor();
    if (ticks <= 0) {
        settleOnFloor();
        return;
    }
    Actor& critter = m_sim.actor;
    if (!present()) {
        return;
    }
    if (critter.parent == nullptr) {
        if (critter.definition != nullptr &&
            critter.definition->rules().pressureReferenceDamage > 0) {
            m_sim.pressure.refresh(players);
        }
        // Linked heads keep their hit-feedback clock even when the body drives their pose.
        updateHitFlashes(critter, ticks);
        for (auto& child : children()) {
            updateHitFlashes(child->m_sim.actor, ticks);
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
        m_sim.scales.players = static_cast<s32>(std::ranges::count(standing, true));
        for (auto& child : children()) {
            child->m_sim.scales.players = m_sim.scales.players;
        }
    }
    const s32 i = m_sim.id;
    const CritterData& data = *critter.definition;
    critter.age += seconds;
    critter.burnGap = std::max(0.0f, critter.burnGap - seconds);
    critter.sinceHurt += seconds;
    for (CritterArea& area : critter.areas) {
        area.secondsLeft -= seconds;
        area.displacement += area.velocity * seconds;
    }
    std::erase_if(critter.areas, [](const CritterArea& area) { return area.secondsLeft <= 0; });
    // Frozen, it stands as it is: no move, no step, no one in its sights.
    if (critter.frozenTicks > 0) {
        critter.presentation.valid = false;
        critter.frozenTicks = std::max(critter.frozenTicks - ticks, 0);
        settleOnFloor();
        aimGaze(critter, seconds, players);
        holdBrokenPoses(critter);
        updateAreas(critter, i, players);
        carryGrab(critter, players);
        return;
    }
    const bool stopped = timeStopped && critter.state == State::Active;
    if (stopped && critter.move >= 0 &&
        data.moves()[static_cast<usize>(critter.move)].type != MoveDefinition::kStart) {
        critter.presentation.valid = false;
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
            m_sim.grabs.push_back({critter.grabbed, i, std::nullopt, Vec3{0}, 0});
            critter.grabbed = -1;
        }
    }
    const MoveDefinition* move =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // CritterMoveSetup retains +0x124 until the move changes or has no target.
    // A fresh roster (including hidden/departed-player filtering) must not redirect
    // an ongoing volley or wind-up. The next move selects from that fresh roster.
    if (move != nullptr && (critter.moveTarget < 0 || critter.moveDone)) {
        critter.moveTarget =
            critter.blindTicks > 0 ? -1 : attackTarget(critter, move->target, players, true);
    }
    // The move plays; over its harmful frames its part strikes.
    critter.skinAge += seconds;
    if (move != nullptr && critter.player.playing()) {
        const bool wasFinished = critter.player.finished();
        critter.player.advance(seconds, false);
        for (usize j = 0; j < critter.attachments.size(); ++j) {
            auto& attachment = critter.attachments[j];
            const auto& tree = *critter.stock->attachments[j].tree;
            attachment.player.advance(seconds,
                                      tree.sequences[attachment.player.sequence()].repeats);
            attachment.pose.evaluate(tree, attachment.player.sequence(), attachment.player.frame());
        }
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
                if (targeted && (harm->flags & damage::kSticky) != 0) {
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
                m_sim.spews.push_back(CombatSpew{i, partPosition(critter, move->colnode),
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
        rememberFloor();
    }
    // CritterBossAI probes its floor (FloorCollide, 0x8003a2d4) after its step
    // (CritterTranslate, 0x8003a218), before what it holds, its areas and its heads follow it.
    settleOnFloor();
    carryGrab(critter, players);
    updateAreas(critter, i, players);
    critter.push *= portable::pow(kPushDecay, seconds * kPushFrameRate);
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
               critter.stock->definition.departure == CombatantDefinition::Departure::Drops) {
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
            drop.critter = m_sim.id;
            drop.kind = data.kind();
            drop.form = form();
            drop.drop = true;
            drop.position = critter.position;
            m_sim.losses.push_back(std::move(drop));
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
    for (auto& part : children()) {
        if (part->id() == partId) {
            const f32 before = part->health();
            const f32 credited = part->hurtActor(hit);
            // A lethal branch hit removes that branch, without forwarding its final hit.
            if (part->alive()) {
                loseHealth(before - part->health());
            }
            m_sim.actor.childrenIntact =
                std::ranges::all_of(children(), [](const auto& p) { return p->alive(); });
            collectChildEvents(*part);
            return credited;
        }
    }
    return hurtActor(hit);
}

f32 Combatant::hurtActor(const EnemyHit& hit) {
    Actor& critter = m_sim.actor;
    const s32 id = m_sim.id;
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
        Damage::modify(amount, flags, data.shieldFlags(), data.armor(), m_sim.scales.bossEncounter);
    flags = modified.flags;
    amount = modified.amount;
    // Shrunk, a great one takes double (CritterDamage); a boss never shrinks.
    const CombatantDefinition& family = critter.stock->definition;
    if (family.shrinks) {
        amount = EnemyShrink::harmTaken(critter.shrink, amount);
    }
    if (amount <= 0.0f) {
        return 0;
    }
    // A boss takes less the more there are to fight it, outside a legend item's rite
    // (CritterDamage's damage_mul).
    const f32 counterDamage = amount;
    critter.roarOwed += counterDamage;
    const bool partyScaled = family.harmScale == CombatantDefinition::HarmScale::PartySize;
    const s32 players =
        std::clamp(m_sim.scales.players, 0, static_cast<s32>(kBossShares.size()) - 1);
    if (partyScaled && !m_sim.fullHarm) {
        amount *= kBossShares[static_cast<usize>(players)];
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
        if (partyScaled) {
            share *= static_cast<f32>(players);
        }
        if (!partyScaled && m_sim.scales.playerLevel > 0.0f &&
            static_cast<f32>(hit.level) < m_sim.scales.playerLevel) {
            const f32 under = m_sim.scales.playerLevel - static_cast<f32>(hit.level);
            amount *= std::max(1.0f - kUnderLevelLoss * under, 0.1f);
        }
        CombatLoss loss;
        loss.critter = id;
        loss.kind = data.kind();
        loss.player = hit.player;
        loss.experience = share;
        loss.position = critter.position;
        m_sim.losses.push_back(loss);
    }
    amount = damageNode(hit.node, amount, flags);
    if (amount <= 0) {
        return credited;
    }
    critter.patrol.end();
    critter.patrolAim.reset();
    critter.hurtPending += counterDamage;
    critter.hurtFlags |= flags;
    // CritterDamage accumulates the caller's unnormalized knockback input only after
    // the struck node accepts damage. Concurrent hits contribute to the same shove.
    critter.hurtDirection += hit.direction;
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
        m_sim.cues.push_back(std::move(burst));
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
    if (alive() && !children().empty()) {
        const auto count =
            std::ranges::count_if(children(), [](const auto& part) { return part->alive(); });
        if (count > 0) {
            for (auto& part : children()) {
                if (part->alive()) {
                    part->loseHealth(0.5f * amount / static_cast<f32>(count));
                    collectChildEvents(*part);
                }
            }
        }
        m_sim.actor.childrenIntact =
            std::ranges::all_of(children(), [](const auto& part) { return part->alive(); });
    }
    return credited;
}

void Combatant::damagedPlayer(s32 player, f32 amount, s32 partId) {
    Actor* actor = &m_sim.actor;
    if (partId >= 0 && partId != m_sim.id) {
        actor = nullptr;
        for (auto& child : children()) {
            if (child->id() == partId) {
                actor = &child->m_sim.actor;
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
    Actor& critter = m_sim.actor;
    if (!alive() || amount <= 0.0f) {
        return;
    }
    critter.health -= amount;
    if (critter.health <= 0.0f) {
        critter.state = State::Dying;
        CombatLoss fall;
        fall.critter = m_sim.id;
        fall.kind = data()->kind();
        fall.form = form();
        fall.player = -1;
        fall.experience = kKillShare * data()->experience();
        fall.killed = true;
        fall.position = critter.position;
        m_sim.losses.push_back(fall);
        for (auto& part : children()) {
            if (part->alive()) {
                part->m_sim.actor.health = 1.0f;
            }
        }
    }
}

std::vector<CombatBlow> Combatant::takeBlows() {
    return std::exchange(m_sim.blows, {});
}
std::vector<CombatGrab> Combatant::takeGrabs() {
    return std::exchange(m_sim.grabs, {});
}

std::vector<CombatLoss> Combatant::takeLosses() {
    return std::exchange(m_sim.losses, {});
}

std::vector<CombatCue> Combatant::takeCues() {
    return std::exchange(m_sim.cues, {});
}

std::vector<CombatSpew> Combatant::takeSpews() {
    return std::exchange(m_sim.spews, {});
}

std::vector<CombatShot> Combatant::takeShots() {
    return std::exchange(m_sim.shots, {});
}
std::vector<CombatArenaActivation> Combatant::takeArenaActivations() {
    return std::exchange(m_sim.arenaActivations, {});
}

void Combatant::startPatrol(const LookoutRoute* route, f32 sight) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->patrol.start(route, critter->position, sight);
        critter->patrolAim = critter->patrol.aim(critter->position);
    }
}

void Combatant::freeze(s32 ticks) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->frozenTicks = std::max(ticks, 0);
        critter->roarWanted = false;
    }
}

void Combatant::blind(s32 ticks) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->blindTicks = std::max(ticks, 0);
        critter->target = -1;
    }
}

void Combatant::curb(f32 seconds) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->curbSeconds = std::max(seconds, 0.0f);
    }
}

void Combatant::resize(f32 scale) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr && scale > 0.0f) {
        critter->scale = scale;
    }
}

void Combatant::setShrink(f32 scale) {
    if (scale <= 0.0f) {
        return;
    }
    m_sim.actor.shrink = scale;
    for (auto& child : children()) {
        child->m_sim.actor.shrink = scale;
    }
}

f32 Combatant::dealt(f32 amount) const {
    const CombatantAssets* stock = m_sim.actor.stock;
    return stock != nullptr && !stock->definition.shrinks
               ? amount
               : EnemyShrink::harmDealt(m_sim.actor.shrink, amount);
}

void Combatant::hold(bool held) {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->held = held;
    }
}

f32 Combatant::roarThreshold(s32 players) {
    const auto count =
        static_cast<usize>(std::clamp(players, 0, static_cast<s32>(kRoarShares.size()) - 1));
    return std::floor(kRoarAfter * kRoarShares[count]);
}

void Combatant::roar() {
    if (Actor* critter = present() ? &m_sim.actor : nullptr; critter != nullptr) {
        critter->roarWanted = true;
    }
}

} // namespace gdl::game
