#include "game/world/Traps.h"

#include <algorithm>
#include <cstring>

#include "engine/core/Log.h"

namespace gdl::game {

bool Traps::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                 const WorldCollision* collision, u32 seed, f32 timeScale, f32 damageScale) {
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
        trap->offTime = ownRest != 0 ? -ownRest * 3 : info.activeOff;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        if (!trap->figure.place(device, items, name, instance, collision)) {
            log::warn("Traps: no figure {} in the item archive", name);
        }
        trap->box = trap->figure.obstacle(info);
        trap->box.solid = false;
        trap->ticksLeft = restTicks(*trap);
        m_traps.push_back(std::move(trap));
    }
    return !m_traps.empty();
}

void Traps::clear() {
    m_traps.clear();
    m_gaps.clear();
}

void Traps::setPlayerCount(s32 players) {
    for (const std::unique_ptr<Trap>& trap : m_traps) {
        trap->shown = shownToParty(trap->minPlayers, players);
    }
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

std::vector<TrapHit> Traps::update(s32 ticks, f32 seconds, std::span<const TrapVictim> party) {
    std::vector<TrapHit> hits;
    m_gaps.resize(party.size(), 0.0f);
    for (f32& gap : m_gaps) {
        gap = std::max(gap - seconds, 0.0f);
    }
    for (usize index = 0; index < m_traps.size(); ++index) {
        Trap& trap = *m_traps[index];
        if (!trap.shown) {
            continue;
        }
        trap.figure.update(seconds);
        trap.ticksLeft -= ticks;
        if (trap.ticksLeft <= 0) {
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
                const bool pierces = trap.subtype == kSpikes || trap.subtype == kBlade ||
                                     trap.subtype == kBlades;
                hits.push_back(TrapHit{index, v, trap.damage, trap.subtype, pierces,
                                       trap.figure.position()});
            }
        }
    }
    return hits;
}

void Traps::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const std::unique_ptr<Trap>& trap : m_traps) {
        if (trap->shown) {
            trap->figure.draw(device, clip, lighting);
        }
    }
}

} // namespace gdl::game
