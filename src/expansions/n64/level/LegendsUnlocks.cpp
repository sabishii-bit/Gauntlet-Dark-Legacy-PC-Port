#include "expansions/n64/level/LegendsUnlocks.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kProgramBase = 0xe0000400U;
constexpr usize kRealms = 0xe006c360U - kProgramBase;
constexpr usize kLevels = 0xe006c468U - kProgramBase;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends unlock policy: " + std::string{message});
    }
}

void range(std::span<const u8> bytes, usize offset, usize size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset, "truncated program");
}

std::string name(std::span<const u8> program, u32 address) {
    require(address >= kProgramBase, "invalid native name address");
    const usize start = address - kProgramBase;
    std::string result;
    for (usize i = 0; i <= LegendsProgress::kMaxMapName; ++i) {
        range(program, start + i, 1);
        if (program[start + i] == 0) {
            const auto identity = LegendsProgress::mapIdentity(result);
            require(identity.has_value(), "invalid native map name");
            return *identity;
        }
        result.push_back(static_cast<char>(program[start + i]));
    }
    throw FormatError("N64 Legends unlock policy: unterminated native name");
}
} // namespace

LegendsN64Unlocks LegendsN64Unlocks::decode(std::span<const u8> program) {
    struct Realm {
        std::string stem;
        u32 requiredObelisks = 0;
        std::vector<std::string> preceding;
    };
    std::map<u32, Realm> realms;
    bool terminated = false;
    // e006b8a4 reads realm+16 and removes the bits owned by each active player.
    // realm+20 belongs to a separate missing-obelisk hint, not admission.
    for (usize at = kRealms; at < kLevels; at += 24) {
        range(program, at, 24);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            terminated = true;
            break;
        }
        require(id < 16, "invalid realm identity");
        Realm realm{name(program, readU32BE(program, at + 8)), readU32BE(program, at + 16), {}};
        require(realms.emplace(id, std::move(realm)).second, "duplicate realm identity");
    }
    require(terminated, "unterminated realm table");
    LegendsN64Unlocks result;
    std::set<std::pair<u32, u32>> identities;
    std::set<std::string> names;
    for (usize i = 0; i < 128; ++i) {
        const usize at = kLevels + i * 40;
        range(program, at, 40);
        const u32 realmId = readU32BE(program, at);
        if (realmId == 16) {
            return result;
        }
        const u32 stage = readU32BE(program, at + 4);
        const auto found = realms.find(realmId);
        require(found != realms.end() && stage <= 10, "invalid stage identity");
        require(identities.emplace(realmId, stage).second, "duplicate stage identity");
        auto& realm = found->second;
        auto map = realm.stem + name(program, readU32BE(program, at + 12));
        require(LegendsProgress::mapIdentity(map).has_value(), "invalid combined map name");
        require(names.insert(map).second, "duplicate native map name");
        result.m_stages.push_back({map, realmId, stage, realm.requiredObelisks, realm.preceding});
        realm.preceding.push_back(std::move(map));
    }
    throw FormatError("N64 Legends unlock policy: unterminated stage table");
}

bool LegendsN64Unlocks::isTower(std::string_view map) const {
    const auto identity = LegendsProgress::mapIdentity(map);
    if (!identity) {
        return false;
    }
    const auto found = std::ranges::find(m_stages, *identity, &LegendsStageUnlock::map);
    return found != m_stages.end() && found->realm == 8;
}

bool LegendsN64Unlocks::allows(std::string_view map, const LegendsProgress& party) const {
    const auto identity = LegendsProgress::mapIdentity(map);
    if (!identity || !party.valid()) {
        return false;
    }
    const auto found = std::ranges::find(m_stages, *identity, &LegendsStageUnlock::map);
    if (found == m_stages.end() ||
        (party.obelisks & found->requiredObelisks) != found->requiredObelisks) {
        return false;
    }
    // e006bb1c passes each preceding table row's stage in a2 to e006bc28;
    // that helper accepts the row when ANY attending player owns completion.
    // The caller supplies the union, never GDL or another platform's history.
    return std::ranges::all_of(found->preceding,
                               [&](const auto& previous) { return party.hasCompleted(previous); });
}

bool LegendsN64Unlocks::unlockAll(LegendsProgress& progress) const {
    if (m_stages.empty() || !progress.valid()) {
        return false;
    }
    auto changed = progress;
    for (const auto& stage : m_stages) {
        if (!changed.hasCompleted(stage.map) && !changed.recordCompleted(stage.map)) {
            return false;
        }
    }
    changed.obelisks |= 0xffU;
    changed.runestones |= 0x1fffU;
    changed.shards |= 0xfU;
    progress = std::move(changed);
    return true;
}

} // namespace gdl::game
