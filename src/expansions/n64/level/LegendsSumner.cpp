#include "expansions/n64/level/LegendsSumner.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/io/Sha256.h"

#include "expansions/n64/level/LegendsTowerMotion.h"
#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/shared/level/LegendsItems.h"

namespace gdl::game {
namespace {
constexpr u32 kProgramBase = 0xe0000400U;
constexpr u32 kTowerBase = 0xe0860000U;

bool validParty(std::span<const LegendsProgress> party) {
    return !party.empty() && party.size() <= 4 &&
           std::ranges::all_of(party, [](const auto& progress) { return progress.valid(); });
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

std::optional<u32> legendsN64MissedRelicCue(std::span<const LegendsPickup> pickups,
                                            std::span<const LegendsProgress> party) {
    if (!validParty(party))
        return {};
    std::optional<u32> rune;
    for (const auto& pickup : pickups) {
        if (pickup.collected || !pickup.award || pickup.award->index < 0)
            continue;
        const auto award = *pickup.award;
        const bool owned = std::ranges::any_of(
            party, [&](const auto& progress) { return progress.has(award.kind, award.index); });
        if (owned)
            continue;
        // e00694e4/e0064e60 set pending bits only for unowned native
        // obelisks/runes; e0068000/e0065618 clear them on collection.
        // e00649dc gives the obelisk priority and uses 0x4058 + rune ID.
        if (award.kind == LegendsRelic::Obelisk && award.index < 7)
            return 0x4057;
        if (award.kind == LegendsRelic::Runestone && award.index < 13)
            rune = 0x4059U + static_cast<u32>(award.index);
    }
    return rune;
}

std::optional<u32> legendsN64DepartureCue(std::string_view map,
                                          std::span<const LegendsPickup> pickups,
                                          std::span<const LegendsProgress> party) {
    if (!validParty(party))
        return {};
    // e0062ca0 installs bit8 for these exact realm/stage pairs; e00649dc
    // prioritizes it over pending obelisk/rune reminders. Unlike those reminders,
    // the native overrides are not suppressed by previously owned collectibles.
    // These are bank tokens, not invented transcripts or Sumner ceremony voices.
    constexpr std::array<std::pair<std::string_view, u32>, 7> overrides{{{"mount6", 0x4067},
                                                                         {"castle5", 0x4068},
                                                                         {"town5", 0x4069},
                                                                         {"ice5", 0x406a},
                                                                         {"boss2", 0x406b},
                                                                         {"boss1", 0x406c},
                                                                         {"hell1", 0x406d}}};
    const auto identity = toLowerAscii(map);
    for (const auto& [stage, token] : overrides) {
        if (identity == stage)
            return token;
    }
    return legendsN64MissedRelicCue(pickups, party);
}

LegendsSumner LegendsSumner::decode(std::span<const u8> program, std::span<const u8> tower) {
    Sha256 hash;
    hash.update(program);
    if (program.size() != 707312 ||
        hash.hex() != "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585") {
        throw FormatError("N64 Sumner: unsupported native program");
    }
    const auto layout = formats::decodeLegendsN64Tower(tower);
    const auto stages = LegendsN64Unlocks::decode(program);
    LegendsSumner result;
    result.m_podium = layout.residentVolumes[1];
    // e0042614 state0x30 opens e0046000. e0041894 reads its title and
    // +0x50 option pointer; options are 32-byte records ending at text=-1.
    constexpr usize kMenu = 0xe0046000U - kProgramBase;
    result.m_title = readU32BE(program, kMenu);
    const u32 optionAddress = readU32BE(program, kMenu + 0x50);
    if (optionAddress < kProgramBase || optionAddress - kProgramBase > program.size()) {
        throw FormatError("N64 Sumner: invalid menu pointer");
    }
    usize at = optionAddress - kProgramBase;
    bool ended = false;
    for (usize i = 0; i <= 6; ++i, at += 32) {
        if (at > program.size() || program.size() - at < 32) {
            throw FormatError("N64 Sumner: truncated menu");
        }
        const u32 text = readU32BE(program, at);
        if (text == 0xffffffffU) {
            ended = true;
            break;
        }
        result.m_choices.push_back({text, readU32BE(program, at + 4)});
    }
    if (!ended || result.m_choices.size() != 6) {
        throw FormatError("N64 Sumner: unsupported menu");
    }
    // Tower e086a294 checks these records in order. Types 1/2 skip a relic
    // owned by ANY attending player; type 3 does the same for realm boss stage 8.
    // Type 0 is the final answer, including the all-collected reply.
    constexpr std::array<u32, 3> kTables{0xe086a538U, 0xe086a490U, 0xe086a598U};
    constexpr std::array<usize, 3> kCounts{8, 14, 7};
    for (usize topic = 0; topic < kTables.size(); ++topic) {
        for (usize i = 0; i < kCounts[topic]; ++i) {
            const usize record = kTables[topic] - kTowerBase + i * 12;
            const u32 kind = readU32BE(tower, record);
            const u32 identity = readU32BE(tower, record + 4);
            const u32 advice = readU32BE(tower, record + 8);
            if (kind > 3 || advice < 53 || advice > 81 ||
                ((kind == 1 || kind == 2) && (identity == 0 || identity > 64)) ||
                (kind == 0) != (i + 1 == kCounts[topic])) {
                throw FormatError("N64 Sumner: unsupported advice selector");
            }
            const usize row = 0xe00722d0U - kProgramBase + advice * 24;
            // e0071530 uses +8 as text identity and +12 as speech cue. Every
            // Tower hint has cue 0: never substitute guessed spoken dialogue.
            if (readU32BE(program, row + 12) != 0) {
                throw FormatError("N64 Sumner: unexpected voiced hint");
            }
            Rule rule{kind, identity, readU32BE(program, row + 8), {}};
            if (kind == 3) {
                const auto stage = std::ranges::find_if(stages.stages(), [&](const auto& value) {
                    return value.realm == identity && value.stage == 8;
                });
                if (stage == stages.stages().end()) {
                    throw FormatError("N64 Sumner: missing native boss stage");
                }
                rule.map = stage->map;
            }
            result.m_rules[topic].push_back(std::move(rule));
        }
    }
    const auto first = std::ranges::find_if(
        stages.stages(), [](const auto& stage) { return stage.realm == 3 && stage.stage == 0; });
    if (first == stages.stages().end()) {
        throw FormatError("N64 Sumner: missing first Mountain stage");
    }
    result.m_firstMountain = first->map;
    // Tower e086239c reads one rune caption by installed count. e0863440
    // reads shard pairs in identity order; e0865c98 reads realm-opening pairs.
    for (usize i = 0; i < result.m_runeCaptions.size(); ++i) {
        result.m_runeCaptions[i] = readU16BE(tower, 0x9700 + i * 2);
    }
    constexpr std::array<usize, 4> kDoorRows{3, 1, 2, 4};
    for (usize i = 0; i < 4; ++i) {
        for (usize line = 0; line < 2; ++line) {
            result.m_shardCaptions[i][line] = readU16BE(tower, 0x96ec + i * 4 + line * 2);
            result.m_gateCaptions[i][line] = readU16BE(tower, 0x96d8 + kDoorRows[i] * 4 + line * 2);
        }
    }
    result.m_windowCaptions = {readU16BE(tower, 0x96fc), readU16BE(tower, 0x96fe)};
    return result;
}

u32 LegendsSumner::milestone(std::span<const LegendsProgress> party) const {
    if (!ready() || !validParty(party)) {
        return 14;
    }
    u64 runes = 0;
    u64 shards = 0;
    u64 obelisks = 0;
    for (const auto& progress : party) {
        if (progress.hasCompleted("hell2")) {
            return 10;
        }
        runes |= progress.runestones;
        shards |= progress.shards;
        obelisks |= progress.obelisks;
    }
    // The state/page identities are game e0042614's Tower advice switch.
    // Ownership survives a save reload; the original transient history global
    // does not, so restore the most advanced applicable chapter from that state.
    if ((runes & 0x1fffU) == 0x1fffU)
        return 5;
    if ((obelisks & 0x80U) != 0)
        return 12;
    if ((shards & 0xfU) == 0xfU)
        return 13;
    if ((obelisks & 0x18U) == 0x18U)
        return 6;
    if ((obelisks & 0x60U) == 0x60U)
        return 7;
    if ((obelisks & 7U) == 7U)
        return 0;
    return std::ranges::any_of(
               party, [&](const auto& progress) { return progress.hasCompleted(m_firstMountain); })
               ? 2U
               : 14U;
}

std::vector<u32> LegendsSumner::milestoneTextIds(std::span<const LegendsProgress> before,
                                                 std::span<const LegendsProgress> after) const {
    if (!ready() || !validParty(before) || !validParty(after)) {
        return {};
    }
    const auto combine = [](std::span<const LegendsProgress> party) {
        LegendsProgress result;
        for (const auto& progress : party) {
            result.runestones |= progress.runestones;
            result.shards |= progress.shards;
            result.obelisks |= progress.obelisks;
        }
        return result;
    };
    const auto old = combine(before);
    const auto now = combine(after);
    std::vector<u32> result;
    const u64 addedRunes = (now.runestones & ~old.runestones) & 0x1fffU;
    usize count = static_cast<usize>(std::popcount(old.runestones & 0x1fffU));
    for (s32 i = 0; i < std::popcount(addedRunes); ++i) {
        result.push_back(m_runeCaptions[count++]);
    }
    constexpr std::array<u8, 4> kRequired{0x60, 7, 0x18, 0x80};
    for (usize i = 0; i < kRequired.size(); ++i) {
        const auto required = kRequired[i];
        if ((old.obelisks & required) != required && (now.obelisks & required) == required) {
            result.insert(result.end(), m_gateCaptions[i].begin(), m_gateCaptions[i].end());
        }
    }
    for (usize i = 0; i < m_shardCaptions.size(); ++i) {
        const u64 bit = u64{1} << i;
        if ((now.shards & ~old.shards & bit) != 0) {
            result.insert(result.end(), m_shardCaptions[i].begin(), m_shardCaptions[i].end());
        }
    }
    if ((old.shards & 0xfU) != 0xfU && (now.shards & 0xfU) == 0xfU) {
        result.insert(result.end(), m_windowCaptions.begin(), m_windowCaptions.end());
    }
    // Native text IDs521/522/526 are empty continuation slots, not blank pages.
    std::erase_if(result, [](u32 id) { return id == 0 || id == 521 || id == 522 || id == 526; });
    return result;
}

std::optional<u32> LegendsSumner::adviceTextId(u32 action,
                                               std::span<const LegendsProgress> party) const {
    if (!ready() || !validParty(party)) {
        return std::nullopt;
    }
    const usize topic = action == 0xd ? 0 : action == 0x2e ? 1 : action == 0x2f ? 2 : 3;
    if (topic >= m_rules.size()) {
        return std::nullopt;
    }
    for (const Rule& rule : m_rules[topic]) {
        const bool owned = std::ranges::any_of(party, [&](const auto& progress) {
            switch (rule.kind) {
            case 1: return progress.has(LegendsRelic::Obelisk, static_cast<s32>(rule.identity - 1));
            case 2:
                return progress.has(LegendsRelic::Runestone, static_cast<s32>(rule.identity - 1));
            case 3: return progress.hasCompleted(rule.map);
            default: return false;
            }
        });
        if (!owned) {
            return rule.textId;
        }
    }
    return std::nullopt;
}

std::optional<u8> LegendsSumner::historyPage(u32 state,
                                             std::span<const LegendsProgress> party) const {
    if (!ready() || !validParty(party)) {
        return std::nullopt;
    }
    // e086a280 restores the saved milestone; e0042614 then selects its
    // authored adv_t1 document through e0071bc8. This does not replay a ceremony.
    switch (state) {
    case 0: return 12;
    case 2: return 10;
    case 3:
        return std::ranges::any_of(
                   party,
                   [&](const auto& progress) { return progress.hasCompleted(m_firstMountain); })
                   ? std::nullopt
                   : std::optional<u8>{10};
    case 5: return 16;
    case 6: return 13;
    case 7: return 11;
    case 10: return 17;
    case 12: return 15;
    case 13: return 14;
    case 14: return 1;
    case 15: return 2;
    default: return std::nullopt;
    }
}

bool LegendsSumner::podiumContact(const Vec3& from, const Vec3& requested, f32 radius,
                                  f32 height) const {
    if (!ready() || !finite(from) || !finite(requested) || !std::isfinite(radius) || radius <= 0 ||
        !std::isfinite(height) || height <= 0 || from == requested) {
        return false;
    }
    return resolveLegendsTowerVolume(m_podium, from, requested, radius, height) != requested;
}

std::optional<s32>
LegendsSumner::update(f32 seconds, std::span<const LegendsSumnerVisitor> visitors, bool blocked) {
    if (!ready() || blocked || !std::isfinite(seconds) || seconds < 0) {
        return std::nullopt;
    }
    // e0042614 advances its game clock by dtSeconds*150, then requires
    // lastPrompt+150 < now. Modal states do not advance that gameplay clock.
    m_sincePrompt = std::min(2.0, m_sincePrompt + static_cast<f64>(seconds));
    if (m_sincePrompt <= 1) {
        return std::nullopt;
    }
    s32 owner = 4;
    for (const auto& visitor : visitors) {
        if (visitor.alive && visitor.touching && visitor.seat >= 0 && visitor.seat < owner) {
            owner = visitor.seat;
        }
    }
    if (owner == 4) {
        return std::nullopt;
    }
    m_sincePrompt = 0;
    return owner;
}

} // namespace gdl::game
