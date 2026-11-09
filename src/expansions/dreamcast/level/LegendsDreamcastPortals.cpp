#include "expansions/dreamcast/level/LegendsDreamcastPortals.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {

// 1ST_READ.BIN 8c065400 groups Town and Castle into one tower branch.
s32 realmOf(s32 world) {
    switch (world) {
    case 1:
    case 7: return 1;
    case 2: return 0;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4;
    case 6: return 6;
    case 8: return 5;
    default: return -1;
    }
}

s32 stageOrder(s32 world, s32 stage) {
    const s32 realm = realmOf(world);
    if (realm < 0) {
        return -1;
    }
    // 8c065500 deliberately reorders these entries; ASCII suffix order is
    // not progression order (notably Dungeon/Chimera and Battlefield).
    if (world == 1) {
        if (stage == 0)
            stage = 1;
        else if (stage == 5)
            stage = 4;
        else if (stage == 4)
            stage = 5;
    } else if (world == 7 && stage == 3) {
        stage = 6;
    } else if (world == 8) {
        constexpr std::array<s32, 5> order{0, 2, 4, 1, 3};
        if (stage < static_cast<s32>(order.size())) {
            stage = order[static_cast<usize>(stage)];
        }
    }
    return (realm << 8) | (stage + 1);
}

bool finite(Vec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

} // namespace

void LegendsDreamcastPortalPolicy::addWorld(std::span<const u8> bytes) {
    const auto require = [](bool valid) {
        if (!valid)
            throw FormatError("Dreamcast portal roster has invalid worlddata records");
    };
    require(bytes.size() >= 8);
    const usize directory = readU32LE(bytes, 0), count = readU32LE(bytes, 4);
    require(directory >= 8 && directory <= bytes.size() && count <= 4096 &&
            count <= (bytes.size() - directory) / 12);
    struct Section {
        usize offset;
        usize count;
    };
    std::map<u32, Section> sections;
    for (usize i = 0; i < count; ++i) {
        const usize at = directory + i * 12;
        const Section section{readU32LE(bytes, at + 4), readU32LE(bytes, at + 8)};
        require(section.offset >= 8 && section.offset <= directory && section.count <= 4096);
        require(sections.emplace(readU32LE(bytes, at), section).second);
    }
    const auto records = [&](u32 tag, usize stride) {
        const auto found = sections.find(tag);
        require(found != sections.end());
        const auto section = found->second;
        usize end = directory;
        for (const auto& [unused, other] : sections) {
            if (other.offset > section.offset)
                end = std::min(end, other.offset);
        }
        require(section.count <= (end - section.offset) / stride);
        return section;
    };
    const auto worlds = records(0x57524c44, 56), levels = records(0x4c45564c, 144);
    require(worlds.count == 1 && levels.count <= 256);
    const s32 world = readS32LE(bytes, worlds.offset);
    std::vector<LegendsDreamcastStage> additions;
    for (usize i = 0; i < levels.count; ++i) {
        const usize at = levels.offset + i * 144 + 8;
        const u8 letter = bytes[at], number = bytes[at + 1];
        require(letter >= 'A' && letter <= 'Z' && number >= '1' && number <= '9' &&
                bytes[at + 2] == 0);
        std::string name = "level";
        name += static_cast<char>(letter + ('a' - 'A'));
        name += static_cast<char>(number);
        // Castle's first two LEVL records both say A1. Native lookup takes
        // the first, so do not silently replace it with the later Barracks row.
        if (!find(name) &&
            std::ranges::none_of(additions, [&](const auto& stage) { return stage.map == name; })) {
            // SetItem's exit parser (8c0a2000) uses the numeric suffix, not
            // the row's ordinal within LEVL. The table still admits the code.
            const s32 stage = number - '1';
            additions.push_back({std::move(name), world, stage, stageOrder(world, stage)});
        }
    }
    m_stages.insert(m_stages.end(), additions.begin(), additions.end());
}

const LegendsDreamcastStage* LegendsDreamcastPortalPolicy::find(std::string_view map) const {
    const auto name = LegendsProgress::mapIdentity(map);
    if (!name)
        return nullptr;
    const auto found = std::ranges::find(m_stages, *name, &LegendsDreamcastStage::map);
    return found == m_stages.end() ? nullptr : &*found;
}

bool LegendsDreamcastPortalPolicy::allows(std::string_view map,
                                          const LegendsProgress& progress) const {
    const auto* destination = find(map);
    if (!destination || destination->order < 0)
        return false;
    u32 realms = 1;
    constexpr std::array<u64, 3> obelisks{0x38, 7, 0x1c0};
    for (usize i = 0; i < obelisks.size(); ++i) {
        if ((progress.obelisks & obelisks[i]) == obelisks[i])
            realms |= 1U << (i + 1);
    }
    if ((progress.shards & 0x20) != 0)
        realms |= 1U << 5;
    s32 completed = 0;
    for (const auto& stage : m_stages) {
        if (progress.hasCompleted(stage.map))
            completed = std::max(completed, stage.order);
    }
    // Tower admission 8c0655c0: the first stage of an open realm or any
    // stage up to the party's highest completion plus one. Special realms
    // are unlocked by their own collective relic masks.
    const s32 realm = realmOf(destination->world);
    return ((realms & (1U << realm)) != 0 &&
            (destination->order <= completed + 1 || (destination->order & 255) == 1)) ||
           (destination->world == 5 && (progress.shards & 0x1e) == 0x1e) ||
           (destination->world == 6 && (progress.runestones & 0x91fff) == 0x91fff);
}

std::optional<LegendsDreamcastPortal>
dreamcastLegendsPortal(const formats::ItemInfoRecord& info,
                       const formats::ItemInstanceRecord& instance, u32 index,
                       const LegendsDreamcastPortalPolicy& policy) {
    if (info.type != 9 || info.subtype != 0 || instance.params[0] != 0 || instance.params[1] != 0 ||
        instance.params[2] != 0 || instance.params[3] != 0 || !finite(instance.position) ||
        !std::isfinite(info.radius) || info.radius <= 0 || instance.minPlayers < 0 ||
        instance.minPlayers > 4 || (instance.flags & 2U) != 0)
        return {};
    const u8 letter = instance.params[4], number = instance.params[5];
    if (letter < 'A' || letter > 'Z' || number < '1' || number > '9')
        return {};
    std::string map = "level";
    map += static_cast<char>(letter + ('a' - 'A'));
    map += static_cast<char>(number);
    if (!policy.find(map))
        return {};
    return LegendsDreamcastPortal{index, std::move(map), instance.position, info.radius,
                                  instance.minPlayers};
}

bool dreamcastContinuesRun(std::string_view source, std::string_view destination) {
    const auto from = LegendsProgress::mapIdentity(source);
    const auto to = LegendsProgress::mapIdentity(destination);
    return from && to &&
           ((*from == "levele1" && *to == "levele2") || (*from == "levelf1" && *to == "levelf2") ||
            (*from == "levelh1" && *to == "levelh4") || (*from == "levelh2" && *to == "levelh5") ||
            (*from == "levelh3" && *to == "levelh6"));
}

bool touchesDreamcastPortal(const LegendsDreamcastPortal& portal, Vec3 feet, f32 radius,
                            usize partySize) {
    if (!finite(feet) || !finite(portal.position) || !std::isfinite(radius) || radius < 0 ||
        !std::isfinite(portal.radius) || portal.radius <= 0 || partySize == 0 || partySize > 4) {
        return false;
    }
    const Vec3 away = feet - portal.position;
    const f32 reach = portal.radius + radius + static_cast<f32>(partySize - 1);
    return std::abs(away.y) <= 3 && away.x * away.x + away.z * away.z <= reach * reach;
}

s32 LegendsDreamcastPortalMotion::sequenceTicks(s32 frames, s32 frameRate) {
    if (frames <= 0) {
        return 0;
    }
    // InitAnim 8c069170 scales by rate/30; do_items 8c0ab9f0 rounds
    // frames*scale + .5 before doubling. ACTIVE1/3 are 15 frames at rate15:
    // their pose takes .25 seconds, but their native timer holds for 16/60.
    const s64 rate = frameRate > 0 ? frameRate : 30;
    return static_cast<s32>(std::min<s64>(3600, ((frames * rate + 15) / 30) * 2));
}

LegendsDreamcastPortalMotion::LegendsDreamcastPortalMotion(std::array<s32, 5> sequenceTicks)
    : m_sequenceTicks(sequenceTicks) {
    for (auto& ticks : m_sequenceTicks)
        ticks = std::clamp(ticks, 0, 3600);
}

void LegendsDreamcastPortalMotion::enter(Phase phase) {
    m_phase = phase;
    m_ticks = m_sequenceTicks[static_cast<usize>(phase)];
    ++m_generation;
}

void LegendsDreamcastPortalMotion::reset() {
    m_remainder = 0;
    m_departed = false;
    enter(Phase::Idle);
}

bool LegendsDreamcastPortalMotion::tick(u8 occupants, u8 living) {
    const bool ready = m_ticks == 0;
    if (occupants != 0 && occupants == living) {
        // Native 8c0acbf0..8c0acc08 waits for phase 4 (ACTIVE3) to
        // finish. ACTIVE2 is not the departure boundary: skipping the last
        // sequence transports a solo player before the rings finish moving.
        if (m_phase == Phase::Closing && ready) {
            m_departed = true;
            return true;
        }
        if ((ready || m_phase == Phase::Idle) && m_phase < Phase::Closing) {
            enter(static_cast<Phase>(static_cast<u8>(m_phase) + 1));
        }
    } else if (occupants != 0) {
        if (ready && m_phase < Phase::Active) {
            enter(static_cast<Phase>(static_cast<u8>(m_phase) + 1));
        } else if (ready && m_phase > Phase::Active) {
            enter(Phase::Idle);
        }
        // 8c0acd48..5c reloads this hold only while a partial party is on
        // ACTIVE2. Otherwise use the actual authored animation duration.
        if (m_phase == Phase::Active)
            m_ticks = 45;
    } else if (m_phase != Phase::Idle && (ready || m_phase == Phase::Active)) {
        enter(m_phase == Phase::Closing ? Phase::Idle
                                        : static_cast<Phase>(static_cast<u8>(m_phase) + 1));
    }
    m_ticks = std::max(0, m_ticks - 1);
    return false;
}

bool LegendsDreamcastPortalMotion::advance(f32 seconds, u8 occupants, u8 living, bool enabled) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1 || (living & 0xf0U) != 0 ||
        (occupants & ~living) != 0)
        return false;
    if (!enabled || living == 0) {
        reset();
        return false;
    }
    if (m_departed)
        return false;
    m_remainder += static_cast<f64>(seconds) * 60;
    while (m_remainder >= 1) {
        m_remainder -= 1;
        if (tick(occupants, living))
            return true;
    }
    return false;
}

} // namespace gdl::game
