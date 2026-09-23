#include "game/world/Chests.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

std::int16_t paramS16(const ItemInstance& instance, std::size_t at) {
    std::int16_t value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

} // namespace

int Chests::resolveContents(std::span<const ItemInfo> infos, int record, std::size_t itemIndex,
                            unsigned int& seed) {
    // A list may name a list; each pick moves the seed on.
    for (int hops = 0; hops < 8; ++hops) {
        if (record < 0 || static_cast<std::size_t>(record) >= infos.size()) {
            return -1;
        }
        const ItemInfo& info = infos[static_cast<std::size_t>(record)];
        if (info.type != ItemInfo::kChoiceList) {
            return record;
        }
        if (info.choices.empty()) {
            return -1;
        }
        const auto pick = ((seed >> 5U) + static_cast<unsigned int>(itemIndex)) %
                          static_cast<unsigned int>(info.choices.size());
        seed += static_cast<unsigned int>(kSeedStep);
        record = info.choices[pick];
    }
    return -1;
}

bool Chests::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                  const WorldCollision* collision) {
    clear();
    m_infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= m_infos.size()) {
            continue;
        }
        const ItemInfo& info = m_infos[static_cast<std::size_t>(instance.info)];
        if (info.type != ItemInfo::kContainer || info.subtype == kBarrel) {
            continue;
        }
        auto chest = std::make_unique<Chest>();
        chest->instance = static_cast<int>(index);
        chest->info = instance.info;
        chest->subtype = info.subtype;
        chest->contents = paramS16(instance, 0);
        chest->count = paramS16(instance, 4);
        chest->minPlayers = instance.minPlayers;
        chest->locked = (static_cast<unsigned int>(info.activeType) & kLocked) != 0;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        if (!chest->figure.place(device, items, name, instance, collision)) {
            log::warn("Chests: no figure {} in the item archive", name);
        }
        chest->box = chest->figure.obstacle(info);
        m_chests.push_back(std::move(chest));
    }
    return !m_chests.empty();
}

void Chests::clear() {
    m_chests.clear();
    m_infos.clear();
    m_seed = 0;
}

void Chests::setPlayerCount(int players) {
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        chest->shown = shownToParty(chest->minPlayers, players);
    }
}

std::vector<ChestEvent> Chests::update(float seconds, std::span<const ChestVisitor> party) {
    std::vector<ChestEvent> events;
    for (std::size_t index = 0; index < m_chests.size(); ++index) {
        Chest& chest = *m_chests[index];
        if (!chest.shown || chest.gone) {
            continue;
        }
        chest.figure.update(seconds);
        chest.refusalLeft = std::max(chest.refusalLeft - seconds, 0.0f);
        if (chest.state == kShut) {
            for (std::size_t v = 0; v < party.size(); ++v) {
                if (!chest.box.touchedBy(party[v].position, party[v].radius)) {
                    continue;
                }
                ChestEvent event;
                event.chest = index;
                event.visitor = v;
                event.position = chest.figure.position();
                if (chest.locked && party[v].keys <= 0) {
                    if (chest.refusalLeft <= 0.0f) {
                        chest.refusalLeft = kRefusalSeconds;
                        event.kind = ChestEvent::Kind::Refused;
                        events.push_back(event);
                    }
                    continue;
                }
                chest.state = kOpening;
                chest.opener = static_cast<int>(v);
                chest.figure.play(kOpening, false);
                event.kind = ChestEvent::Kind::Unlocked;
                events.push_back(event);
                break;
            }
        } else if (chest.state == kOpening && chest.figure.finished()) {
            chest.state = kOpen;
            chest.figure.play(kOpen, true);
            ChestEvent event;
            event.kind = ChestEvent::Kind::Opened;
            event.chest = index;
            event.visitor = static_cast<std::size_t>(std::max(chest.opener, 0));
            event.position = chest.figure.position();
            event.explodes = chest.subtype == kTrappedChest;
            const int inside = resolveContents(m_infos, chest.contents,
                                               static_cast<std::size_t>(chest.instance), m_seed);
            if (!event.explodes && inside >= 0) {
                const ItemInfo& record = m_infos[static_cast<std::size_t>(inside)];
                if (chest.subtype == kGoldChest) {
                    event.gold = record.value;
                } else {
                    event.contents = inside;
                }
            }
            events.push_back(event);
        }
    }
    return events;
}

void Chests::hold(std::size_t chest, int item) {
    if (chest < m_chests.size()) {
        m_chests[chest]->held = item;
    }
}

int Chests::holdingTouchedBy(const ChestVisitor& visitor) const {
    for (std::size_t index = 0; index < m_chests.size(); ++index) {
        const Chest& chest = *m_chests[index];
        if (chest.shown && !chest.gone && chest.state == kOpen && chest.held >= 0 &&
            chest.box.touchedBy(visitor.position, visitor.radius)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void Chests::remove(std::size_t chest) {
    if (chest < m_chests.size()) {
        m_chests[chest]->gone = true;
        m_chests[chest]->held = -1;
    }
}

std::vector<Obstacle> Chests::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        if (chest->shown && !chest->gone) {
            boxes.push_back(chest->box);
        }
    }
    return boxes;
}

void Chests::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        if (chest->shown && !chest->gone) {
            chest->figure.draw(device, clip, lighting);
        }
    }
}

} // namespace gdl::game
