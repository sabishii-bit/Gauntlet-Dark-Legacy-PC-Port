#include "game/netplay/CharacterProfile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"

#include "game/players/NameCheats.h"

namespace gdl::game {
namespace {
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool counter(s32 value) {
    return value >= 0 && value <= 1'000'000;
}
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
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
        !std::ranges::all_of(relics.gargoylePieces, counter) ||
        !std::ranges::all_of(p.levels.runeLevels,
                             [](u16 mask) { return (mask >> Relics::kRuneCount) == 0; }) ||
        p.lifetime.enemiesKilled < 0 || p.lifetime.generatorsDestroyed < 0 ||
        p.lifetime.goldFound < 0 || !std::isfinite(p.lifetime.playSeconds) ||
        p.lifetime.playSeconds < 0 || p.lifetime.playSeconds > 1e12 || helpSeen.size() > 151 ||
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
    bytes.reserve(kBytes);
    word(bytes, fourcc("GDCP"));
    word(bytes, kVersion);
    for (usize i = 0; i < 8; ++i) {
        bytes.push_back(i < profile.name.size() ? static_cast<u8>(profile.name[i]) : 0);
    }
    const auto& p = profile.progress;
    for (const s32 value : {profile.character, profile.color, profile.gold, profile.levelTotal,
                            p.experience, p.gold, p.promotedLevel, p.health}) {
        word(bytes, static_cast<u32>(value));
    }
    word(bytes, profile.autoAttack ? 1 : 0);
    for (const f32 value : {p.fightAdd, p.armorAdd, p.magicAdd, p.speedAdd}) {
        real(bytes, value);
    }
    for (const s32 value : p.crystals) {
        word(bytes, static_cast<u32>(value));
    }
    word(bytes, p.unlocked);
    word(bytes, static_cast<u32>(p.inventory.keys));
    word(bytes, static_cast<u32>(p.inventory.potions.size()));
    for (usize i = 0; i < Inventory::kMostPotions; ++i) {
        word(bytes, i < p.inventory.potions.size() ? static_cast<u32>(p.inventory.potions[i]) : 0);
    }
    for (const auto& slot : p.inventory.powerups) {
        real(bytes, slot.strength);
        word(bytes, static_cast<u32>(slot.kind));
        real(bytes, slot.charge);
        word(bytes, slot.flags);
        word(bytes, slot.on ? 1 : 0);
    }
    for (const u16 value :
         {p.relics.runes, p.relics.legends, p.relics.shards, p.relics.pendingRunes,
          p.relics.pendingShards, p.relics.pendingCeremonies}) {
        word(bytes, value);
    }
    for (const s32 value : p.relics.gargoylePieces) {
        word(bytes, static_cast<u32>(value));
    }
    bytes.insert(bytes.end(), p.levels.beaten.begin(), p.levels.beaten.end());
    bytes.insert(bytes.end(), 2, 0);
    for (const auto& masks : {p.levels.runeLevels, p.levels.legendLevels, p.levels.bossDeaths}) {
        for (const u16 value : masks) {
            word(bytes, value);
        }
    }
    for (const s32 value :
         {p.lifetime.enemiesKilled, p.lifetime.generatorsDestroyed, p.lifetime.goldFound}) {
        word(bytes, static_cast<u32>(value));
    }
    const auto seconds = std::bit_cast<u64>(p.lifetime.playSeconds);
    word(bytes, static_cast<u32>(seconds));
    word(bytes, static_cast<u32>(seconds >> 32U));
    std::array<u32, 5> help{};
    for (const s32 id : profile.helpSeen) {
        help[static_cast<usize>(id / 32)] |= 1U << static_cast<u32>(id % 32);
    }
    for (const u32 mask : help) {
        word(bytes, mask);
    }
    return bytes;
}

std::optional<CharacterProfile> CharacterProfilePacket::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
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
    p.fightAdd = real(reader);
    p.armorAdd = real(reader);
    p.magicAdd = real(reader);
    p.speedAdd = real(reader);
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
        slot.strength = real(reader);
        slot.kind = reader.readS32();
        slot.charge = real(reader);
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
    const u64 low = reader.readU32();
    p.lifetime.playSeconds = std::bit_cast<f64>(low | (u64{reader.readU32()} << 32U));
    for (s32 block = 0; block < 5; ++block) {
        const auto mask = reader.readU32();
        for (s32 bit = 0; bit < 32; ++bit) {
            if ((mask & (1U << static_cast<u32>(bit))) != 0) {
                profile.helpSeen.push_back(block * 32 + bit);
            }
        }
    }
    if (!profile.valid()) {
        return std::nullopt;
    }
    restoreNameForm(profile.progress.inventory, profile.name);
    return profile;
}
} // namespace gdl::game
