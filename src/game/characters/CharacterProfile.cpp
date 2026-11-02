#include "game/characters/CharacterProfile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

#include "game/characters/NameCheats.h"

namespace gdl::game {
namespace {
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool counter(s32 value) {
    return value >= 0 && value <= 1'000'000;
}

usize sharedPrefix(std::string_view first, std::string_view second) {
    usize result = 0;
    while (result < first.size() && result < second.size() && first[result] == second[result]) {
        ++result;
    }
    return result;
}

void legends(ByteWriter& out, const LegendsProgress& progress) {
    for (const u64 mask :
         {progress.runestones, progress.shards, progress.obelisks, progress.legendItems}) {
        out.writeU64(mask);
    }
    out.writeU32(static_cast<u32>(progress.completedMaps.size()));
    std::string_view previous;
    for (const auto& map : progress.completedMaps) {
        const usize prefix = sharedPrefix(previous, map);
        out.writeU8(static_cast<u8>(prefix));
        out.writeU8(static_cast<u8>(map.size() - prefix));
        out.writeString(std::string_view{map}.substr(prefix));
        previous = map;
    }
}

bool legends(ByteReader& reader, LegendsProgress& progress) {
    if (reader.remaining() < 36) {
        return false;
    }
    for (auto* mask :
         {&progress.runestones, &progress.shards, &progress.obelisks, &progress.legendItems}) {
        *mask = reader.readU64();
    }
    const usize count = reader.readU32();
    if (count > LegendsProgress::kMaxCompletedMaps || count > reader.remaining() / 2) {
        return false;
    }
    std::string previous;
    for (usize i = 0; i < count; ++i) {
        if (reader.remaining() < 2) {
            return false;
        }
        const usize prefix = reader.readU8();
        const usize suffix = reader.readU8();
        if (prefix > previous.size() || suffix == 0 ||
            prefix + suffix > LegendsProgress::kMaxMapName || suffix > reader.remaining()) {
            return false;
        }
        std::string map = previous.substr(0, prefix) + reader.readString(suffix);
        // One representation per sorted map list, not arbitrary equivalent prefix splits.
        if (sharedPrefix(previous, map) != prefix) {
            return false;
        }
        progress.completedMaps.push_back(map);
        previous = std::move(map);
    }
    return progress.valid();
}
} // namespace

bool CharacterProfile::valid() const {
    const auto& p = progress;
    const auto& inventory = p.inventory;
    const auto& relics = p.relics;
    if (name.empty() || name.size() > kCharacterNameLength ||
        !std::ranges::all_of(name, [](unsigned char c) { return c >= 32 && c <= 126; }) ||
        character < 0 || character >= kClassCount || color < 0 || color >= kColorCount ||
        !counter(gold) || !counter(levelTotal) || p.experience < 0 ||
        p.experience > 1'000'000'000 || !counter(p.gold) || !counter(p.health) ||
        p.promotedLevel < -1 || p.promotedLevel > kMaxLevel ||
        !bounded(p.fightAdd, -1'000'000, 1'000'000) ||
        !bounded(p.armorAdd, -1'000'000, 1'000'000) ||
        !bounded(p.magicAdd, -1'000'000, 1'000'000) ||
        !bounded(p.speedAdd, -1'000'000, 1'000'000) || (p.unlocked >> kRealmCount) != 0 ||
        !std::ranges::all_of(p.crystals,
                             [](s32 value) { return value >= -1'000'000 && value <= 1'000'000; }) ||
        inventory.keys < 0 || inventory.keys > Inventory::kMostKeys ||
        inventory.potions.size() > Inventory::kMostPotions ||
        !std::ranges::all_of(inventory.potions,
                             [](s32 value) { return value >= 1 && value <= 4; }) ||
        !std::ranges::all_of(inventory.powerups,
                             [](const PowerupSlot& slot) {
                                 return (slot.kind == 0
                                             ? !slot.held() && slot.flags == 0 && slot.charge == 0
                                             : slot.kind >= 5 && slot.kind <= 9) &&
                                        bounded(slot.strength, -1'000'000, 1'000'000) &&
                                        bounded(slot.charge, -1'000'000, 1'000'000);
                             }) ||
        (relics.runes >> Relics::kRuneCount) != 0 || (relics.pendingRunes & ~relics.runes) != 0 ||
        (relics.pendingShards & ~relics.shards) != 0 ||
        (relics.pendingCeremonies & ~Relics::kTowerCeremonyMask) != 0 ||
        !std::ranges::all_of(relics.gargoylePieces,
                             [](s32 value) { return value == -1 || counter(value); }) ||
        !std::ranges::all_of(p.levels.runeLevels,
                             [](u16 mask) { return (mask >> Relics::kRuneCount) == 0; }) ||
        p.lifetime.enemiesKilled < 0 || p.lifetime.generatorsDestroyed < 0 ||
        p.lifetime.goldFound < 0 || p.lifetime.bestStreak < 0 ||
        !std::isfinite(p.lifetime.playSeconds) || p.lifetime.playSeconds < 0 ||
        p.lifetime.playSeconds > 1e12 || helpSeen.size() > 151 ||
        !std::ranges::all_of(p.legends, [](const auto& campaign) { return campaign.valid(); }) ||
        !std::ranges::all_of(helpSeen, [](s32 value) { return value >= 0 && value <= 150; }) ||
        !std::ranges::is_sorted(helpSeen)) {
        return false;
    }
    return std::adjacent_find(helpSeen.begin(), helpSeen.end()) == helpSeen.end();
}

std::optional<CharacterProfile> CharacterProfile::capture(const CharacterSave& save) {
    if (save.character < 0 || save.character >= kClassCount) {
        return std::nullopt; // Check before CharacterSave::progress indexes the selected class.
    }
    CharacterProfile profile{save.name,       save.character,  save.color,      save.gold,
                             save.levelTotal, save.autoAttack, save.progress(), save.helpSeen};
    std::ranges::sort(profile.helpSeen);
    const auto duplicate = std::ranges::unique(profile.helpSeen);
    profile.helpSeen.erase(duplicate.begin(), duplicate.end());
    profile.progress.gold = save.gold;
    if (!profile.valid()) {
        return std::nullopt;
    }
    restoreNameForm(profile.progress.inventory, profile.name);
    return profile;
}
CharacterSave CharacterProfile::gameplayCopy() const {
    CharacterSave result;
    if (valid()) {
        result.name = name;
        result.character = character;
        result.color = color;
        result.gold = gold;
        result.levelTotal = levelTotal;
        result.autoAttack = autoAttack;
        result.progress() = progress;
        result.helpSeen = helpSeen;
        restoreNameForm(result);
    }
    return result;
}

std::optional<std::vector<u8>> CharacterProfilePacket::encode(const CharacterProfile& profile) {
    if (!profile.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kMinBytes);
    ByteWriter out(bytes);
    out.writeU32(fourcc("GDCP"));
    out.writeU32(kVersion);
    for (usize i = 0; i < 8; ++i) {
        out.writeU8(i < profile.name.size() ? static_cast<u8>(profile.name[i]) : u8{0});
    }
    const auto& p = profile.progress;
    for (const s32 value : {profile.character, profile.color, profile.gold, profile.levelTotal,
                            p.experience, p.gold, p.promotedLevel, p.health}) {
        out.writeS32(value);
    }
    out.writeU32(profile.autoAttack ? 1 : 0);
    for (const f32 value : {p.fightAdd, p.armorAdd, p.magicAdd, p.speedAdd}) {
        out.writeF32(value);
    }
    for (const s32 value : p.crystals) {
        out.writeS32(value);
    }
    out.writeU32(p.unlocked);
    out.writeS32(p.inventory.keys);
    out.writeU32(static_cast<u32>(p.inventory.potions.size()));
    for (usize i = 0; i < Inventory::kMostPotions; ++i) {
        out.writeS32(i < p.inventory.potions.size() ? p.inventory.potions[i] : 0);
    }
    for (const auto& slot : p.inventory.powerups) {
        out.writeF32(slot.strength);
        out.writeS32(slot.kind);
        out.writeF32(slot.charge);
        out.writeU32(slot.flags);
        out.writeU32(slot.on ? 1 : 0);
    }
    for (const u16 value :
         {p.relics.runes, p.relics.legends, p.relics.shards, p.relics.pendingRunes,
          p.relics.pendingShards, p.relics.pendingCeremonies}) {
        out.writeU32(value);
    }
    for (const s32 value : p.relics.gargoylePieces) {
        out.writeS32(value);
    }
    out.writeBytes(p.levels.beaten);
    out.writeU16(0);
    for (const auto& masks : {p.levels.runeLevels, p.levels.legendLevels, p.levels.bossDeaths}) {
        for (const u16 value : masks) {
            out.writeU32(value);
        }
    }
    for (const s32 value : {p.lifetime.enemiesKilled, p.lifetime.generatorsDestroyed,
                            p.lifetime.goldFound, p.lifetime.bestStreak}) {
        out.writeS32(value);
    }
    out.writeU64(std::bit_cast<u64>(p.lifetime.playSeconds));
    std::array<u32, 5> help{};
    for (const s32 id : profile.helpSeen) {
        help[static_cast<usize>(id / 32)] |= 1U << static_cast<u32>(id % 32);
    }
    for (const u32 mask : help) {
        out.writeU32(mask);
    }
    for (const auto& campaign : p.legends) {
        legends(out, campaign);
    }
    if (bytes.size() > kMaxBytes) {
        return std::nullopt; // Preserve the whole profile or refuse; never discard progress.
    }
    return bytes;
}

std::optional<CharacterProfile> CharacterProfilePacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kMinBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != fourcc("GDCP") || reader.readU32() != kVersion) {
        return std::nullopt;
    }
    CharacterProfile profile;
    const auto name = reader.readBytes(8);
    bool terminated = false;
    for (const u8 c : name) {
        if (c == 0) {
            terminated = true;
        } else if (terminated) {
            return std::nullopt;
        } else {
            profile.name += static_cast<char>(c);
        }
    }
    profile.character = reader.readS32();
    profile.color = reader.readS32();
    profile.gold = reader.readS32();
    profile.levelTotal = reader.readS32();
    auto& p = profile.progress;
    p.experience = reader.readS32();
    p.gold = reader.readS32();
    p.promotedLevel = reader.readS32();
    p.health = reader.readS32();
    const auto autoAttack = reader.readU32();
    if (autoAttack > 1) {
        return std::nullopt;
    }
    profile.autoAttack = autoAttack != 0;
    p.fightAdd = reader.readF32();
    p.armorAdd = reader.readF32();
    p.magicAdd = reader.readF32();
    p.speedAdd = reader.readF32();
    for (auto& crystal : p.crystals) {
        crystal = reader.readS32();
    }
    p.unlocked = reader.readU32();
    p.inventory.keys = reader.readS32();
    const usize potions = reader.readU32();
    if (potions > Inventory::kMostPotions) {
        return std::nullopt;
    }
    for (usize i = 0; i < Inventory::kMostPotions; ++i) {
        const auto kind = reader.readS32();
        if (i < potions) {
            p.inventory.potions.push_back(kind);
        } else if (kind != 0) {
            return std::nullopt;
        }
    }
    for (auto& slot : p.inventory.powerups) {
        slot.strength = reader.readF32();
        slot.kind = reader.readS32();
        slot.charge = reader.readF32();
        slot.flags = reader.readU32();
        const auto on = reader.readU32();
        if (on > 1) {
            return std::nullopt;
        }
        slot.on = on != 0;
    }
    for (auto* value :
         {&p.relics.runes, &p.relics.legends, &p.relics.shards, &p.relics.pendingRunes,
          &p.relics.pendingShards, &p.relics.pendingCeremonies}) {
        const auto mask = reader.readU32();
        if (mask > std::numeric_limits<u16>::max()) {
            return std::nullopt;
        }
        *value = static_cast<u16>(mask);
    }
    for (auto& value : p.relics.gargoylePieces) {
        value = reader.readS32();
    }
    for (auto& value : p.levels.beaten) {
        value = reader.readU8();
    }
    if (reader.readU16() != 0) {
        return std::nullopt;
    }
    for (auto* masks : {&p.levels.runeLevels, &p.levels.legendLevels, &p.levels.bossDeaths}) {
        for (auto& value : *masks) {
            const auto mask = reader.readU32();
            if (mask > std::numeric_limits<u16>::max()) {
                return std::nullopt;
            }
            value = static_cast<u16>(mask);
        }
    }
    p.lifetime.enemiesKilled = reader.readS32();
    p.lifetime.generatorsDestroyed = reader.readS32();
    p.lifetime.goldFound = reader.readS32();
    p.lifetime.bestStreak = reader.readS32();
    p.lifetime.playSeconds = std::bit_cast<f64>(reader.readU64());
    for (s32 block = 0; block < 5; ++block) {
        const auto mask = reader.readU32();
        for (s32 bit = 0; bit < 32; ++bit) {
            if ((mask & (1U << static_cast<u32>(bit))) != 0) {
                profile.helpSeen.push_back(block * 32 + bit);
            }
        }
    }
    for (auto& campaign : p.legends) {
        if (!legends(reader, campaign)) {
            return std::nullopt;
        }
    }
    if (!reader.atEnd() || !profile.valid()) {
        return std::nullopt;
    }
    restoreNameForm(profile.progress.inventory, profile.name);
    return profile;
}
} // namespace gdl::game
