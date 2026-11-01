#include "expansions/n64/level/LegendsLootSelectors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <set>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void require(bool valid, std::string_view reason) {
    if (!valid) {
        throw FormatError("N64 Legends loot selectors: " + std::string(reason));
    }
}
void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated native table");
}
std::string fourcc(std::span<const u8> bytes, usize at) {
    if (readU32BE(bytes, at) == 0) {
        return {};
    }
    return {std::bit_cast<const char*>(bytes.data() + at), 4};
}
} // namespace

LegendsLootSelectors::LegendsLootSelectors(std::span<const u8> powerups,
                                           std::span<const u8> randoms,
                                           std::span<const formats::LegendsN64Fixture> fixtures,
                                           s32 players) {
    require(players >= 1 && players <= 4, "invalid party size");
    range(powerups, 0, 4);
    const usize count = readU32BE(powerups, 0);
    require(count > 0 && count <= 4096, "template budget exceeded");
    range(powerups, 4, count * 28);
    std::set<u16> keys;
    for (usize i = 0; i < count; ++i) {
        const usize at = 4 + i * 28;
        formats::LegendsN64Fixture item;
        item.kind = formats::LegendsN64FixtureKind::Powerup;
        item.templateKey = readU16BE(powerups, at);
        require(keys.insert(item.templateKey).second, "duplicate pickup identity");
        item.resolvedTemplateKey = item.templateKey;
        item.templateFlags = readU32BE(powerups, at + 4);
        item.resolvedTemplateFlags = item.templateFlags;
        item.templateAmount = std::bit_cast<s16>(readU16BE(powerups, at + 2));
        item.group = "POWERUP";
        item.sequence = fourcc(powerups, at + 8);
        item.deathSequence = fourcc(powerups, at + 16);
        item.pickupLimit = readU32BE(powerups, at + 20);
        item.pickupSound = readU32BE(powerups, at + 24);
        item.pickupRadius = (item.templateKey >> 8U) == 0x21 ? 51.0f / 32 : .5f;
        item.pickupHeight = 3;
        if ((item.templateKey >> 8U) == 0x22) {
            m_selectorGroups.emplace(item.templateKey, readU32BE(powerups, at + 8));
            m_cursors.emplace(item.templateKey, item.templateAmount);
        }
        m_templates.push_back(std::move(item));
    }
    if (!randoms.empty()) {
        range(randoms, 0, 4);
        const usize rows = readU32BE(randoms, 0);
        require(rows <= 4096, "selector table budget exceeded");
        range(randoms, 4, rows * 8);
        for (usize row = 0; row < rows;) {
            const u32 name = readU32BE(randoms, 4 + row * 8);
            if (name == 0x454e4420U) {
                break;
            }
            require(name != 0 && name != 0x53544f50U, "missing selector group name");
            std::vector<u16> entries;
            bool stopped = false;
            for (; row < rows; ++row) {
                const usize at = 4 + row * 8;
                const u32 tag = readU32BE(randoms, at);
                if (tag == 0x53544f50U) {
                    ++row;
                    stopped = true;
                    break;
                }
                require(entries.empty() ? tag == name : tag == 0, "unterminated selector");
                const u16 kind = readU16BE(randoms, at + 4);
                const u16 subtype = readU16BE(randoms, at + 6);
                require(kind <= 255 && subtype <= 255, "invalid selector identity");
                const auto key = static_cast<u16>((kind << 8U) | subtype);
                entries.push_back(key);
            }
            require(stopped && !entries.empty(), "selector has no STOP or entries");
            require(m_groups.emplace(name, std::move(entries)).second, "duplicate selector group");
        }
    }
    require(fixtures.size() <= 32768, "placement budget exceeded");
    // Native pool order: placed floor powers first, then every container's
    // concealed placeholder, even for containers gated by multiplayer count.
    for (const bool contained : {false, true}) {
        for (const auto& fixture : fixtures) {
            const auto kind = contained ? formats::LegendsN64FixtureKind::Container
                                        : formats::LegendsN64FixtureKind::Powerup;
            const u16 key = contained ? fixture.containedPowerup : fixture.templateKey;
            if (fixture.kind != kind || (key >> 8U) != 0x22 ||
                (!contained && fixture.minimumPlayers > players)) {
                continue;
            }
            Slot slot{fixture, contained, true, key, std::nullopt, contained ? 20 : 0};
            if (contained) {
                // e006d080 initially caches the signed placement quantity as a
                // PPower index; it is not a first random selection. Native maps
                // use zero or -1 here. Keep -1 unset until the timer resolves it,
                // rather than emulating retail's unchecked pre-table pointer.
                require(fixture.quantity >= -1 &&
                            (fixture.quantity < 0 || static_cast<usize>(fixture.quantity) < count),
                        "invalid initial concealed pickup index");
                if (fixture.quantity >= 0) {
                    slot.selected = static_cast<usize>(fixture.quantity);
                }
            } else {
                slot.selected = resolve(indexOf(key));
            }
            m_slots.push_back(std::move(slot));
        }
    }
    // Validate every reachable branch before any frame can run its selector.
    // Outcomes are prebound as figures, so the product of placements and choices
    // needs its own bound rather than only separate table-size limits.
    constexpr usize kOutcomeBudget = 8192;
    usize outcomesRemaining = kOutcomeBudget;
    for (usize i = 0; i < m_slots.size(); ++i) {
        const usize outcomeCount = outcomes(i).size();
        require(outcomeCount <= outcomesRemaining, "prebound outcome budget exceeded");
        outcomesRemaining -= outcomeCount;
    }
}

usize LegendsLootSelectors::indexOf(u16 key, bool nativeFallback) const {
    const auto found =
        std::ranges::find(m_templates, key, &formats::LegendsN64Fixture::templateKey);
    // e0065f78 returns index zero for an absent identity. The US RND2 table
    // actually refers to absent 0x021b, so this fallback is observable retail data.
    if (found == m_templates.end() && nativeFallback) {
        return 0;
    }
    require(found != m_templates.end(), "selected pickup template is absent");
    return static_cast<usize>(found - m_templates.begin());
}

usize LegendsLootSelectors::resolve(usize index) {
    std::set<usize> visited;
    while ((m_templates[index].templateKey >> 8U) == 0x22) {
        require(visited.size() < 32 && visited.insert(index).second, "cyclic selector");
        const u16 key = m_templates[index].templateKey;
        const auto group = m_groups.find(m_selectorGroups.at(key));
        require(group != m_groups.end(), "selector group is absent");
        auto& cursor = m_cursors.at(key);
        require(cursor >= -1, "invalid selector cursor");
        ++cursor;
        if (static_cast<usize>(cursor) >= group->second.size()) {
            cursor = 0;
        }
        index = indexOf(group->second[static_cast<usize>(cursor)], true);
    }
    return index;
}

std::optional<usize> LegendsLootSelectors::find(u32 nativeIndex, bool contained) const {
    for (usize i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].contained == contained && m_slots[i].placement.nativeIndex == nativeIndex) {
            return i;
        }
    }
    return {};
}

formats::LegendsN64Fixture LegendsLootSelectors::choice(const Slot& slot, usize index) const {
    auto result = m_templates[index];
    result.nativeIndex = slot.placement.nativeIndex;
    result.position = slot.placement.position;
    // Random slots reuse +0x14 as their cached PPower index. Native amount reads
    // only use this for templates explicitly opting into placement quantity.
    if ((result.templateFlags & 0x10000000U) != 0) {
        require(index <= 127, "selected quantity exceeds placement representation");
        result.quantity = static_cast<s8>(index);
    } else {
        result.quantity = 0;
    }
    result.minimumPlayers = slot.contained ? 0 : slot.placement.minimumPlayers;
    return result;
}

std::optional<formats::LegendsN64Fixture> LegendsLootSelectors::selected(usize slot) const {
    require(slot < m_slots.size(), "invalid selector slot");
    if (!m_slots[slot].selected) {
        return {};
    }
    return choice(m_slots[slot], *m_slots[slot].selected);
}

std::vector<formats::LegendsN64Fixture> LegendsLootSelectors::outcomes(usize slot) const {
    require(slot < m_slots.size(), "invalid selector slot");
    std::set<usize> path;
    std::set<usize> completed;
    std::set<usize> emitted;
    std::vector<formats::LegendsN64Fixture> result;
    const auto emit = [&](usize index) {
        if (emitted.insert(index).second) {
            result.push_back(choice(m_slots[slot], index));
        }
    };
    if (m_slots[slot].selected) {
        emit(*m_slots[slot].selected);
    }
    const std::function<void(usize)> visit = [&](usize index) {
        const u16 key = m_templates[index].templateKey;
        if ((key >> 8U) != 0x22) {
            emit(index);
            return;
        }
        require(path.size() < 32 && path.insert(index).second, "cyclic selector");
        if (completed.contains(index)) {
            path.erase(index);
            return;
        }
        const auto group = m_groups.find(m_selectorGroups.at(key));
        require(group != m_groups.end(), "selector group is absent");
        for (const u16 entry : group->second) {
            visit(indexOf(entry, true));
        }
        path.erase(index);
        completed.insert(index);
    };
    visit(indexOf(m_slots[slot].selector));
    return result;
}

void LegendsLootSelectors::query(usize slot) {
    require(slot < m_slots.size(), "invalid selector slot");
    auto& value = m_slots[slot];
    if (value.active && value.waiting == 0) {
        value.selected = resolve(indexOf(value.selector));
    }
}

void LegendsLootSelectors::freeze(usize slot) {
    query(slot);
    disable(slot);
}

void LegendsLootSelectors::disable(usize slot) {
    require(slot < m_slots.size(), "invalid selector slot");
    m_slots[slot].active = false;
}

void LegendsLootSelectors::tick() {
    for (usize i = 0; i < m_slots.size(); ++i) {
        auto& slot = m_slots[i];
        if (!slot.active) {
            continue;
        }
        if (slot.waiting == 0) {
            query(i);
            slot.waiting = 50;
        } else {
            --slot.waiting;
        }
    }
}

void LegendsLootSelectors::advance(f32 seconds) {
    if (m_slots.empty() || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    // The adapter receives bounded gameplay steps. Refuse pathological caller
    // intervals rather than spending unbounded work or silently dropping ticks.
    require(seconds <= 10, "update interval exceeds budget");
    m_fraction += static_cast<f64>(seconds) * 30;
    while (m_fraction >= 1 - 1e-6) {
        tick();
        m_fraction = std::max(0.0, m_fraction - 1);
    }
}

} // namespace gdl::game
