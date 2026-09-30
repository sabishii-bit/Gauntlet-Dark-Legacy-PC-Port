#include "game/world/TowerAccess.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/world/LevelTriggers.h"

namespace gdl::game {

namespace {

constexpr std::string_view kGlowPrefix = "L1NSNC";
constexpr std::string_view kGlowSuffix = "_ACTIVE";
constexpr usize kGlowNameLength = kGlowPrefix.size() + 2 + kGlowSuffix.size();
constexpr s32 kMostLetteredWorld = 11; ///< the sky: the worlds whose portals carry a letter
constexpr u8 kEveryLevel = 0xFF;

bool holds(u16 held, u16 wanted) {
    return (held & wanted) == wanted;
}

bool worldInRange(s32 world) {
    return world >= 0 && world < TowerAccess::kWorldCount;
}

} // namespace

TowerAccess::TowerAccess(std::span<const CharacterSave> party) {
    for (const CharacterSave& save : party) {
        add(save.progress(), save.character == kSumnerClass);
    }
}

TowerAccess::TowerAccess(std::span<const PartyMember> party) {
    for (const PartyMember& member : party) {
        add(member.save.progress(), member.save.character == kSumnerClass);
    }
}

TowerAccess::TowerAccess(std::span<const ClassProgress> party) {
    for (const ClassProgress& progress : party) {
        add(progress, false);
    }
}

/** One joined character's part of the answers: its shards and runestones are pooled, its
 * beaten levels too (a Sumner counts as having beaten every one), and its crystal counts
 * raise the party's best or, marked complete, meet the gate outright. */
void TowerAccess::add(const ClassProgress& progress, bool sumner) {
    m_anyone = true;
    m_sumner = m_sumner || sumner;
    m_shards = static_cast<u16>(m_shards | progress.relics.shards);
    m_runes = static_cast<u16>(m_runes | progress.relics.runes);
    for (usize world = 0; world < m_beaten.size(); ++world) {
        m_beaten[world] = static_cast<u8>(m_beaten[world] |
                                          (sumner ? kEveryLevel : progress.levels.beaten[world]));
    }
    for (usize gate = 0; gate < m_crystals.size(); ++gate) {
        const s32 held = progress.crystals[gate];
        if (held < 0) {
            m_gatesDone[gate] = true;
        }
        m_crystals[gate] = std::max(m_crystals[gate], held);
    }
}

/** Whether the party meets a crystal gate: a Sumner, a count marked complete, or the most
 * anyone holds reaching what the gate wants (towerLevelStatus, then the best). */
bool TowerAccess::gateMet(s32 gate) const {
    if (gate < 0 || static_cast<usize>(gate) >= m_crystals.size()) {
        return false;
    }
    const auto index = static_cast<usize>(gate);
    if (m_anyone && (m_sumner || m_gatesDone[index])) {
        return true;
    }
    return m_crystals[index] >= LevelTriggers::crystalsNeeded(gate);
}

bool TowerAccess::worldOpen(s32 world) const {
    switch (world) {
    case kTowerWorld: return true;
    case kTempleWorld: return holds(m_shards, kTempleShards);
    case kUnderworldWorld:
        return holds(m_shards, kUnderworldShards) && holds(m_runes, kUnderworldRunes);
    case kBattlefieldWorld: return holds(m_shards, kBattlefieldShards);
    default: break;
    }
    return worldInRange(world) && gateMet(kWorldGates[static_cast<usize>(world)]);
}

bool TowerAccess::portalOpen(s32 world, s32 gate) const {
    if (!worldInRange(world) || gate < 0) {
        return false;
    }
    // A realm's first portal always stands; each after it wants the level before beaten.
    const auto previousBeaten = [&] {
        return gate == 0 ||
               (m_beaten[static_cast<usize>(world)] & (1U << static_cast<u32>(gate - 1))) != 0;
    };
    switch (world) {
    case kTempleWorld:
    case kUnderworldWorld: return worldOpen(world);
    case kBattlefieldWorld:
        if (!worldOpen(world)) {
            return false;
        }
        return gate == kGarmGate ? holds(m_runes, kGarmRunes) : previousBeaten();
    default: return previousBeaten();
    }
}

bool TowerAccess::liftsOpen() const {
    return (m_beaten[static_cast<usize>(kBattlefieldWorld)] & 1U) != 0;
}

u32 TowerAccess::startMarker(u32 marker) const {
    return marker < kMarkerWorlds.size() && worldOpen(kMarkerWorlds[marker]) ? marker : 0;
}

std::string TowerAccess::glowObjectName(s32 world, s32 gate) {
    return std::format("{}{}{}{}", kGlowPrefix, static_cast<char>(kWorldLetterBase + world),
                       gate + 1, kGlowSuffix);
}

bool TowerAccess::isGlowObject(std::string_view name) {
    if (name.size() != kGlowNameLength || !name.starts_with(kGlowPrefix) ||
        !name.ends_with(kGlowSuffix)) {
        return false;
    }
    const char letter = name[kGlowPrefix.size()];
    const char digit = name[kGlowPrefix.size() + 1];
    return worldOfLetter(letter) > 0 && digit >= '1' && digit <= '9';
}

s32 TowerAccess::worldOfLetter(char letter) {
    const char upper =
        letter >= 'a' && letter <= 'z' ? static_cast<char>(letter - 'a' + 'A') : letter;
    const s32 world = upper - kWorldLetterBase;
    return world >= 1 && world <= kMostLetteredWorld ? world : -1;
}

} // namespace gdl::game
