#include "game/world/Traps.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

bool Traps::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                 const WorldCollision* collision, u32 seed, f32 timeScale, f32 damageScale,
                 ItemArchive* realmItems) {
    clear();
    m_random.seed(seed);
    m_timeScale = timeScale;
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kTrap) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        auto trap = std::make_unique<Trap>();
        trap->instance = static_cast<s32>(index);
        trap->minPlayers = instance.minPlayers;
        // An instance may set its own damage and its own rest.
        s16 ownDamage = 0;
        s16 ownRest = 0;
        std::memcpy(&ownDamage, instance.params.data(), sizeof(ownDamage));
        std::memcpy(&ownRest, &instance.params[2], sizeof(ownRest));
        trap->damage = static_cast<f32>(ownDamage != 0 ? ownDamage : info.value) * damageScale;
        trap->subtype = info.subtype;
        trap->properties = info.properties;
        trap->offTime = ownRest != 0 ? -ownRest * 3 : info.activeOff;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        ItemArchive& source =
            !items.trees.find(name).has_value() && realmItems != nullptr ? *realmItems : items;
        // SetItem preserves placements with collision flag 1 (the Courtyard tentacles
        // emerge from below the floor); their collision offset rotates with the item.
        const auto* floor = (info.collisionFlags & 1U) != 0 ? nullptr : collision;
        if (!trap->figure.place(device, source, name, instance, floor)) {
            log::warn("Traps: no figure {} in the item archive", name);
        }
        trap->box = trap->figure.obstacle(info);
        trap->box.centre = Vec3{trap->figure.transform() * Vec4{info.collisionOffset, 1}};
        trap->figure.gateParticlesOnSequence(true);
        trap->box.solid = false;
        trap->ticksLeft = restTicks(*trap);
        m_traps.push_back(std::move(trap));
    }
    return !m_traps.empty();
}

void Traps::clear() {
    m_traps.clear();
    m_wakes.clear();
    m_gaps.clear();
}

void Traps::setPlayerCount(s32 players) {
    for (const std::unique_ptr<Trap>& trap : m_traps) {
        trap->shown = !trap->gone && shownToParty(trap->minPlayers, players);
    }
}

bool Traps::stop(usize index) {
    if (index >= m_traps.size() || !m_traps[index]->shown || m_traps[index]->disarmed) {
        return false;
    }
    Trap& trap = *m_traps[index];
    const bool shows = trap.ticksLeft < kStopShownUnder;
    if (trap.action != kResting) {
        trap.action = kResting;
        trap.figure.play(kResting, true);
    }
    trap.ticksLeft = kStoppedRest;
    return shows;
}

bool Traps::disarm(usize index, RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                   const WorldCollision* collision, ItemArchive* realmItems) {
    if (index >= m_traps.size() || !m_traps[index]->shown || m_traps[index]->disarmed) {
        return false;
    }
    Trap& trap = *m_traps[index];
    const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(trap.instance)];
    std::string name = layout.itemInfos()[static_cast<usize>(instance.info)].name;
    name += kDisarmedSuffix;
    ItemArchive& source =
        !items.trees.find(name).has_value() && realmItems != nullptr ? *realmItems : items;
    trap.disarmed = true;
    trap.action = kResting;
    if (source.trees.find(name).has_value() &&
        trap.figure.place(device, source, name, instance, collision)) {
        trap.figure.play(kResting, true);
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
    if (time < 0) {
        const s32 span = -time;
        time = static_cast<s32>(m_random() % static_cast<u32>(span)) + span / 2;
    }
    return static_cast<s32>(static_cast<f32>(time) * m_timeScale);
}

std::vector<TrapHit> Traps::update(s32 ticks, f32 seconds, std::span<const TrapVictim> party,
                                   bool timeStopped) {
    std::vector<TrapHit> hits;
    m_wakes.clear();
    m_gaps.resize(party.size(), 0.0f);
    for (f32& gap : m_gaps) {
        gap = std::max(gap - seconds, 0.0f);
    }
    for (usize index = 0; index < m_traps.size(); ++index) {
        Trap& trap = *m_traps[index];
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
                trap.figure.play(kResting, true);
            }
            trap.ticksLeft = kStopTimeRest;
            trap.figure.update(seconds);
            continue;
        }
        trap.figure.update(seconds);
        trap.ticksLeft -= ticks;
        if (trap.ticksLeft <= 0) {
            if (trap.action == kResting) {
                m_wakes.push_back(index);
            }
            // On to the next sequence, and from the last back to the rest.
            const auto count = static_cast<s32>(std::max<usize>(trap.figure.sequenceCount(), 2));
            trap.action = trap.action + 1 >= count ? kResting : trap.action + 1;
            trap.figure.play(trap.action, trap.action == kResting);
            trap.ticksLeft = trap.action == kResting
                                 ? restTicks(trap)
                                 : std::max(trap.figure.ticksOf(trap.action), 1);
        }
        if (trap.action == kResting) {
            continue;
        }
        for (usize v = 0; v < party.size(); ++v) {
            if (m_gaps[v] <= 0.0f && trap.box.touchedBy(party[v].position, party[v].radius, 0.0f)) {
                m_gaps[v] = static_cast<f32>(trap.ticksLeft + 1) * kSecondsPerTickLeft;
                const bool pierces =
                    trap.subtype == kSpikes || trap.subtype == kBlade || trap.subtype == kBlades;
                const u32 flags = trap.properties | PlayerImpact::kStun;
                const Vec3 direction =
                    (flags & (PlayerImpact::kKnockBack | PlayerImpact::kKnockDown)) != 0
                        ? -Vec3{trap.figure.transform()[2]}
                        : Vec3{0};
                hits.push_back(TrapHit{index, v, trap.damage, trap.subtype, pierces,
                                       trap.figure.position(), PlayerImpact{flags, direction}});
            }
        }
    }
    return hits;
}

void Traps::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                 const CameraFrame* camera, TreeModel::Pass pass) const {
    for (const std::unique_ptr<Trap>& trap : m_traps) {
        if (trap->shown) {
            trap->figure.draw(device, clip, lighting, 1, 1, camera, pass);
        }
    }
}

} // namespace gdl::game
