#include "game/level/Traps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/combat/DamageTypes.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool Traps::appendSimulationDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("Traps.native.1");
    // These immutable binding inputs remain fixed until clear/bind invalidates
    // history. Mutable collision values are visited by the enclosing world owner.
    digest.resource(m_layout);
    digest.resource(m_collision);
    digest.wide(m_scope.archives().size());
    for (const auto* archive : m_scope.archives()) {
        digest.resource(archive);
    }
    digest.fields<5>(state);
    return digest.good();
}

bool Traps::appendNativeDigest(NativeStateDigest& digest) const {
    return appendSimulationDigest(digest, m_sim);
}

bool Traps::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendSimulationDigest(digest, saved.m_state);
}

bool Traps::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& trap : m_sim.traps) {
        if (!trap.figure.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void Traps::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool Traps::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding;
}

bool Traps::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool Traps::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

bool Traps::bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
                 const WorldCollision* collision, u32 seed, f32 timeScale, f32 damageScale) {
    clear();
    m_collision = collision;
    m_layout = &layout;
    m_scope = scope;
    const auto authored = itemSupportWorld(layout, collision);
    m_sim.random.seed(seed);
    m_sim.timeScale = timeScale;
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kTrap) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        Trap trap;
        trap.instance = static_cast<s32>(index);
        trap.minPlayers = instance.minPlayers;
        // An instance may set its own damage and its own rest.
        s16 ownDamage = 0;
        s16 ownRest = 0;
        std::memcpy(&ownDamage, instance.params.data(), sizeof(ownDamage));
        std::memcpy(&ownRest, &instance.params[2], sizeof(ownRest));
        trap.damage = static_cast<f32>(ownDamage != 0 ? ownDamage : info.value) * damageScale;
        trap.subtype = info.subtype;
        trap.name = info.name;
        trap.properties = info.properties;
        trap.rectangularContact = info.collisionType == 3;
        trap.offTime = ownRest != 0 ? -ownRest * 3 : info.activeOff;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        // SetItem preserves placements with collision flag 1 (the Courtyard tentacles
        // emerge from below the floor); their collision offset rotates with the item.
        const auto* floor =
            (info.collisionFlags & ItemInfo::kKeepsHeight) != 0 ? nullptr : collision;
        if (!trap.figure.place(device, scope, name, instance, floor)) {
            log::warn("Traps: no figure {} in the item archive", name);
        }
        trap.box = trap.figure.obstacle(info);
        // SetItem adds one to coloffset.y. OBJGRP flag 2 leaves vertical-only
        // offsets in world space; offsets with an X/Z component rotate with it.
        const Vec3 offset = info.collisionOffset + Vec3{0, 1, 0};
        const bool rotates = std::abs(offset.x) + std::abs(offset.z) >= 0.01f;
        trap.box.centre = rotates ? Vec3{trap.figure.transform() * Vec4{offset, 1}}
                                  : trap.figure.position() + offset;
        trap.figure.gateParticlesOnSequence(true);
        trap.box.solid = false;
        trap.support.bind(instance, info, authored ? &*authored : nullptr, trap.figure, trap.box,
                          collision);
        trap.support.sync(collision, trap.figure, trap.box);
        trap.ticksLeft = restTicks(trap);
        m_sim.traps.push_back(std::move(trap));
    }
    return !m_sim.traps.empty();
}

void Traps::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_layout = nullptr;
    m_scope = {};
    m_collision = nullptr;
    m_sim.traps.clear();
    m_sim.wakes.clear();
    m_sim.gaps.clear();
}

void Traps::syncFloors() {
    for (auto& trap : m_sim.traps) {
        if (!trap.gone) {
            trap.support.sync(m_collision, trap.figure, trap.box);
        }
    }
}

void Traps::setPlayerCount(s32 players) {
    for (auto& trap : m_sim.traps) {
        trap.shown = !trap.gone && shownToParty(trap.minPlayers, players);
    }
}

bool Traps::stop(usize index) {
    if (index >= m_sim.traps.size() || !m_sim.traps[index].shown || m_sim.traps[index].disarmed) {
        return false;
    }
    Trap& trap = m_sim.traps[index];
    const bool shows = trap.ticksLeft < kStopShownUnder;
    if (trap.action != kResting) {
        trap.action = kResting;
        trap.figure.play(kResting, ItemFigure::Playback::Loop);
    }
    trap.ticksLeft = kStoppedRest;
    return shows;
}

bool Traps::disarm(usize index, RenderDevice& device, const WorldLayout& layout,
                   const AssetScope& scope, const WorldCollision* collision) {
    if (index >= m_sim.traps.size() || !m_sim.traps[index].shown || m_sim.traps[index].disarmed) {
        return false;
    }
    Trap& trap = m_sim.traps[index];
    const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(trap.instance)];
    std::string name = layout.itemInfos()[static_cast<usize>(instance.info)].name;
    name += kDisarmedSuffix;
    trap.disarmed = true;
    const Mat4 placement = trap.figure.transform();
    trap.action = kResting;
    if (scope.findTree(name).has_value() &&
        trap.figure.place(device, scope, name, instance, collision)) {
        trap.figure.play(kResting, ItemFigure::Playback::Loop);
        trap.figure.placeAt(placement);
    } else {
        trap.gone = true;
        trap.shown = false;
    }
    return true;
}

/** How long a trap rests this round: its off time, or when that is negative somewhere at
 * random from half of it to one and a half. */
s32 Traps::restTicks(const Trap& trap) {
    s32 time = trap.offTime * kTicksPerTimeUnit;
    // A trap of more than four sequences (the Forest's blades) does not rest at random at
    // all (ProcessItems: its negative off time is taken as nought).
    if (time < 0 && trap.figure.sequenceCount() > 4) {
        return 0;
    }
    if (time < 0) {
        const s32 span = -time;
        time = static_cast<s32>(m_sim.random() % static_cast<u32>(span)) + span / 2;
    }
    return static_cast<s32>(static_cast<f32>(time) * m_sim.timeScale);
}

std::vector<TrapHit> Traps::update(s32 ticks, f32 seconds, std::span<const TrapVictim> party,
                                   bool timeStopped) {
    std::vector<TrapHit> hits;
    syncFloors();
    m_sim.wakes.clear();
    m_sim.gaps.resize(party.size(), 0.0f);
    for (f32& gap : m_sim.gaps) {
        gap = std::max(gap - seconds, 0.0f);
    }
    for (usize index = 0; index < m_sim.traps.size(); ++index) {
        Trap& trap = m_sim.traps[index];
        if (!trap.shown) {
            continue;
        }
        if (trap.disarmed) {
            trap.figure.update(seconds);
            continue;
        }
        if (timeStopped) {
            if (trap.action != kResting) {
                trap.action = kResting;
                trap.figure.play(kResting, ItemFigure::Playback::Loop);
            }
            trap.ticksLeft = kStopTimeRest;
            trap.figure.update(seconds);
            continue;
        }
        trap.figure.update(seconds);
        trap.ticksLeft -= ticks;
        if (trap.ticksLeft <= 0) {
            if (trap.action == kResting) {
                m_sim.wakes.push_back(index);
            }
            // On to the next sequence, and from the last back to the rest.
            const auto count = static_cast<s32>(std::max<usize>(trap.figure.sequenceCount(), 3));
            trap.action = trap.action + 1 >= count ? kResting : trap.action + 1;
            trap.figure.play(trap.action, trap.action == kResting ? ItemFigure::Playback::Loop
                                                                  : ItemFigure::Playback::Once);
            trap.ticksLeft = trap.action == kResting
                                 ? restTicks(trap)
                                 : std::max(trap.figure.ticksOf(trap.action), 1);
        }
        // PlayerCollideItem admits traps only in action 2 or 4. ONA is a
        // wind-up: charging fxhittime there can mask the short actual strike.
        if (trap.action != 2 && trap.action != 4) {
            continue;
        }
        for (usize v = 0; v < party.size(); ++v) {
            if (m_sim.gaps[v] <= 0.0f && touches(trap, party[v])) {
                m_sim.gaps[v] = static_cast<f32>(trap.ticksLeft + 1) * kSecondsPerTickLeft;
                const bool pierces =
                    trap.subtype == kSpikes || trap.subtype == kBlade || trap.subtype == kBlades;
                const u32 flags = trap.properties | damage::kStun;
                const Vec3 direction = (flags & (damage::kKnockBack | damage::kKnockDown)) != 0
                                           ? -Vec3{trap.figure.transform()[2]}
                                           : Vec3{0};
                hits.push_back(TrapHit{index, v, trap.damage, trap.subtype, pierces,
                                       trap.figure.position(), PlayerImpact{flags, direction}});
            }
        }
    }
    return hits;
}

bool Traps::touches(const Trap& trap, const TrapVictim& victim) {
    // fn_8005F0F4 compares body centres with the sum of the two half-heights.
    // The Courtyard platform can lower a player below the tentacle's origin:
    // testing feet against the trap's height alone loses that overlap.
    if (std::abs(victim.position.y - trap.box.centre.y) > trap.box.height + victim.halfHeight) {
        return false;
    }
    const Vec3 horizontal{victim.position.x, trap.box.centre.y, victim.position.z};
    if (!trap.rectangularContact) {
        return trap.box.touchedBy(horizontal, victim.radius, 0.0f);
    }
    // fn_8005F0F4 coltype 3 compares each projected axis to extent + radius.
    // A circle-versus-box distance test wrongly rounds off the accepted corners.
    Obstacle expanded = trap.box;
    expanded.halfAcross += victim.radius;
    expanded.halfAlong += victim.radius;
    return expanded.touchedBy(horizontal, 0.0f, 0.0f);
}

void Traps::capturePresentation() {
    for (auto& trap : m_sim.traps) {
        trap.figure.capturePresentation();
    }
}

void Traps::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                 const CameraFrame* camera, TreeModel::Pass pass, f32 presentationAlpha) const {
    for (const auto& trap : m_sim.traps) {
        if (trap.shown) {
            trap.figure.draw(device, clip, lighting, 1, 1, camera, pass, presentationAlpha);
        }
    }
}

} // namespace gdl::game
