#include "game/console/LevelListingLines.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "engine/core/Strings.h"

#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/NameCheats.h"
#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/console/GiftRequest.h"
#include "game/console/SpawnRequest.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/InstanceNames.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {

namespace {
/** `name x3` for a count over one, else the name alone. */
std::string tallied(std::string_view name, s32 count) {
    return count > 1 ? std::format("{} x{}", name, count) : std::string(name);
}

/** The names of a map joined ", ", each with its count. */
std::string joined(const std::map<std::string, s32>& counts) {
    std::string line;
    for (const auto& [name, count] : counts) {
        line += line.empty() ? "" : ", ";
        line += tallied(name, count);
    }
    return line;
}

bool critterName(std::string_view name) {
    return name == "GOLEM" || name == "GENERAL" ||
           name.starts_with(SpawnRequest::kGargoylePrefix) || name == "GAR";
}
} // namespace

std::vector<std::string> itemLines() {
    constexpr s32 kNameColumn = 18;
    std::vector<std::string> lines;
    const auto add = [&](std::string_view name, std::string_view what) {
        lines.push_back(std::format("{:<{}} {}", name, kNameColumn, what));
    };
    add("key [n]", std::format("keys for the doors ({})", GiftRequest::kDefaultKeys));
    add("potion [n] [kind]",
        std::format("potions: fire, electric, lightning or acid ({} {})",
                    GiftRequest::kDefaultPotions, potionName(GiftRequest::kDefaultPotionKind)));
    add("gold [n]", std::format("gold for the shop ({})", GiftRequest::kDefaultGold));
    add("health [n]", std::format("health, as food gives it ({})", GiftRequest::kDefaultHealth));
    add("exp <points>", "experience to add");
    add("level <level>", std::format("the level to be, 1 to {}", kMaxLevel));
    for (const GiftPowerup& powerup : giftPowerups()) {
        if (powerup.charged) {
            add(std::format("{} [n]", powerup.name),
                std::format("{} ({} charges)", powerup.summary, GiftRequest::kDefaultCharges));
        } else {
            add(std::format("{} [s]", powerup.name),
                std::format("{} ({:.0f} s; under nought for good)", powerup.summary,
                            GiftRequest::kDefaultSeconds));
        }
    }
    return lines;
}

std::vector<std::string> mapLines(const LevelCatalog& catalog) {
    std::vector<std::string> lines;
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        const char letter =
            realm.prefix.empty()
                ? '?'
                : static_cast<char>(std::toupper(static_cast<unsigned char>(realm.prefix.back())));
        std::string line = std::format("{} {} ({}):", letter, toLowerAscii(realm.file), realm.id);
        for (usize i = 0; i < realm.levels.size(); ++i) {
            const std::string_view title =
                i < realm.titles.size() ? std::string_view(realm.titles[i]) : std::string_view{};
            line += std::format("{} {} {}", i == 0 ? "" : ",", realm.levels[i], title);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> enemyKindLines() {
    std::string names;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        if (spawnableEnemy(kind)) {
            names += names.empty() ? "" : ", ";
            names += enemyKind(kind).name;
        }
    }
    return {std::format("Enemies (spawn <enemy> [tier 1-{}] [count]): {}", SpawnRequest::kMostTier,
                        names)};
}

std::vector<std::string> critterLines() {
    std::string forms;
    for (const std::string_view form : SpawnRequest::kGargoyleForms) {
        forms += forms.empty() ? "" : "|";
        forms += toLowerAscii(form);
    }
    return {std::format("Critters (spawn <critter>): golem, general, gargoyle [{}]", forms)};
}

std::vector<std::string> rosterLines(std::span<const LevelEnemy> roster) {
    constexpr std::array<std::string_view, 5> kClasses{"", "small", "medium", "large",
                                                       "medium other"};
    std::string line;
    for (const LevelEnemy& enemy : roster) {
        if (enemy.kind < 0 || enemy.kind >= kEnemyKindCount) {
            continue;
        }
        const std::string_view label =
            enemy.subtype >= 0 && static_cast<usize>(enemy.subtype) < kClasses.size()
                ? kClasses[static_cast<usize>(enemy.subtype)]
                : std::string_view{};
        line += line.empty() ? "" : ", ";
        line += label.empty() ? std::string(enemyKind(enemy.kind).name)
                              : std::format("{} {}", label, enemyKind(enemy.kind).name);
    }
    return {line.empty() ? std::string("Roster: none") : std::format("Roster: {}", line)};
}

std::vector<std::string> generatorLines(const WorldLayout& layout) {
    std::map<std::string, std::pair<s32, std::set<s32>>> kinds; // count and tiers by name
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = instanceRecord(layout, instance);
        if (info == nullptr || info->type != ItemInfo::kGenerator) {
            continue;
        }
        auto& [count, tiers] = kinds[info->name];
        ++count;
        tiers.insert(std::max(static_cast<s32>(instance.params[0]), 1));
    }
    std::string line;
    for (const auto& [name, record] : kinds) {
        std::string tiers;
        for (const s32 tier : record.second) {
            tiers += tiers.empty() ? "" : ", ";
            tiers += std::to_string(tier);
        }
        line += line.empty() ? "" : "; ";
        line += std::format("{} (tier {})", tallied(name, record.first), tiers);
    }
    return {line.empty() ? std::string("Generators: none") : std::format("Generators: {}", line)};
}

std::vector<std::string> placedEnemyLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = instanceRecord(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && !critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Placed enemies: none")
                           : std::format("Placed enemies: {}", joined(counts))};
}

std::vector<std::string> placedCritterLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = instanceRecord(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Critters placed: none")
                           : std::format("Critters placed: {}", joined(counts))};
}

std::vector<std::string> pickupLines(const WorldLayout& layout) {
    std::string line;
    std::set<std::string> seen;
    for (const ItemInfo& info : layout.itemInfos()) {
        if (info.type != ItemInfo::kPowerup || !seen.insert(info.name).second) {
            continue;
        }
        line += line.empty() ? "" : ", ";
        line += std::format("{} ({})", info.name, pickupKindName(info.subtype));
    }
    return {line.empty() ? std::string("Pickups: none")
                         : std::format("Pickups (spawn <name>): {}", line)};
}

std::vector<std::string> objectLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const WorldObject& object : layout.objects()) {
        ++counts[object.name];
    }
    if (counts.empty()) {
        return {"Objects: none"};
    }
    return {std::format("Objects ({}, {} distinct): {}", layout.objects().size(), counts.size(),
                        joined(counts))};
}

std::vector<std::string> switchLines(std::span<const SwitchEntry> switches) {
    if (switches.empty()) {
        return {"Switches: none"};
    }
    std::vector<std::string> lines{
        std::format("Switches ({}; activate <switch#N|id|name>):", switches.size())};
    for (const SwitchEntry& entry : switches) {
        std::string line = std::format("switch#{} {} id {}", entry.index, entry.name, entry.id);
        if (!entry.target.empty()) {
            line += std::format(" -> {}", entry.target);
        }
        if (entry.chained) {
            line += ", fired through another";
        }
        if (entry.shootable) {
            line += ", shootable";
        }
        if (!entry.enabled) {
            line += ", not for this party";
        }
        line += entry.fired ? ", fired" : ", waiting";
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> characterLines(u16 unlocks) {
    std::string starting;
    for (s32 i = 0; i < kStartingClassCount; ++i) {
        starting += starting.empty() ? "" : ", ";
        starting += classCode(i);
    }
    std::string unlockable;
    for (s32 i = kStartingClassCount; i < kClassCount; ++i) {
        unlockable += unlockable.empty() ? "" : ", ";
        const bool known = classUnlocked(i, unlocks);
        unlockable += std::format("{} {}", classCode(i),
                                  known ? "unlocked" : (i == kSumnerClass ? "hidden" : "locked"));
    }
    std::string costumes;
    for (const HiddenCostume& costume : hiddenCostumes()) {
        costumes += costumes.empty() ? "" : ", ";
        costumes += std::format("{} ({} {})", costume.name, classCode(costume.character),
                                costume.directory);
    }
    return {std::format("Starting: {}", starting),
            std::format("Unlockable (unlock character <code>): {}", unlockable),
            std::format("Costumes by name, entered at the select screen: {}", costumes)};
}

std::vector<std::string> inventoryLines(const CharacterSave& save) {
    const ClassProgress& progress = save.progress();
    const s32 level = experienceLevel(progress.experience);
    std::vector<std::string> lines;
    lines.push_back(std::format("{} the {} at level {} ({} experience), health {}/{}, {} gold",
                                save.name, classCode(save.character), level, progress.experience,
                                save.health(), mostHealth(level), save.gold));
    std::string carried =
        std::format("Keys {}/{}, potions {}/{}", progress.inventory.keys, Inventory::kMostKeys,
                    progress.inventory.potions.size(), Inventory::kMostPotions);
    for (usize i = 0; i < progress.inventory.potions.size(); ++i) {
        carried +=
            std::format("{} {}", i == 0 ? ":" : ",", potionName(progress.inventory.potions[i]));
    }
    lines.push_back(std::move(carried));
    std::string powerups;
    for (const PowerupSlot& slot : progress.inventory.powerups) {
        if (!slot.held()) {
            continue;
        }
        powerups += powerups.empty() ? "Powerups: " : ", ";
        powerups += powerupName(slot);
        powerups += slot.strength < 0.0f ? std::string(" for good")
                                         : std::format(" {:.0f} s", slot.strength);
        if (slot.charge != 0.0f) {
            powerups += std::format(" x{:.0f}", slot.charge);
        }
        if (!slot.on) {
            powerups += " (off)";
        }
    }
    lines.push_back(powerups.empty() ? "No powerups" : std::move(powerups));
    const Relics& relics = progress.relics;
    lines.push_back(std::format(
        "Runestones {}/{}, boss shards {}, legend items {}, gargoyle pieces {}/{} {}/{} {}/{}",
        relics.runeCount(), Relics::kRuneCount, std::popcount(relics.shards),
        std::popcount(relics.legends), relics.gargoylePieces[0], Relics::kGargoyleNeeded[0],
        relics.gargoylePieces[1], Relics::kGargoyleNeeded[1], relics.gargoylePieces[2],
        Relics::kGargoyleNeeded[2]));
    std::string crystals;
    for (usize gate = 0; gate < progress.crystals.size(); ++gate) {
        if (progress.crystals[gate] != 0) {
            crystals += std::format("{}gate {}: {}", crystals.empty() ? "Crystals: " : ", ", gate,
                                    progress.crystals[gate]);
        }
    }
    lines.push_back(crystals.empty() ? "Crystals: none" : std::move(crystals));
    return lines;
}

} // namespace gdl::game
