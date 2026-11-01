#include "game/console/ConsoleCommands.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "game/characters/ClassData.h"
#include "game/characters/LevelRecord.h"
#include "game/characters/NameCheats.h"
#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/LevelTriggers.h"
#include "game/level/TowerAccess.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {
constexpr s32 kDefaultCharges = 10;
constexpr s32 kMostPartyCount = 4;
constexpr std::string_view kPreviewWord = "preview";
constexpr f32 kForGood = -1.0f; ///< a powerup's strength under none lasts
constexpr s32 kMostTier = 3;
constexpr s32 kShootableTriggerSubtype = 31; ///< a target on the wall (LevelTrigger)
constexpr std::string_view kGargoylePrefix = "GAR_";

struct Potion {
    std::string_view name;
    s32 kind;
};
/** The kinds the shop sells, by the colour each burst takes. */
constexpr std::array<Potion, 4> kPotions{
    {{"fire", 1}, {"electric", 2}, {"lightning", 3}, {"acid", 4}}};

struct Powerup {
    std::string_view name;
    std::string_view summary;
    s32 kind;
    u32 flags;
    bool charged; ///< spent a charge at a time rather than timed
    f32 charge;   ///< a stat boost, for one that has one
};
constexpr std::array<Powerup, 28> kPowerups{{
    {"levitate", "held above the ground", powerup::kSpecial, powerup::kLevitation, false, 0},
    {"invulnerable", "takes no harm", powerup::kArmor, armor::kInvulnerable, false, 0},
    {"goldinvulnerable", "takes no harm, gilded", powerup::kArmor, armor::kGoldInvulnerable, false,
     0},
    {"antideath", "a halo Death cannot take", powerup::kArmor, DeathRules::kProtection, false, 0},
    {"reflectshield", "turns missiles back", powerup::kArmor, armor::kReflect, false, 0},
    {"fireshield", "burns what it is against", powerup::kArmor, armor::kFireArmor, false, 0},
    {"lightningshield", "shocks what it is against", powerup::kArmor, armor::kElectricArmor, false,
     0},
    {"supershot", "one huge shot a charge", powerup::kWeapon, damage::kSuperShot, true, 0},
    {"threeway", "three shots at once", powerup::kWeapon, damage::kThreeWayShot, false, 0},
    {"fiveway", "five shots at once", powerup::kWeapon, damage::kFiveWayShot, false, 0},
    {"rapidfire", "shoots faster", powerup::kWeapon, damage::kRapidFire, false, 0},
    {"reflect", "shots that bounce off the walls", powerup::kWeapon, damage::kReflect, false, 0},
    {"thunderhammer", "a hammer blow of lightning a charge", powerup::kWeapon, damage::kHammer,
     true, 0},
    {"firebreath", "breathes fire a charge", powerup::kSpecial, powerup::kFireBreath, true, 0},
    {"acidbreath", "breathes acid a charge", powerup::kSpecial, powerup::kAcidBreath, true, 0},
    {"lightningbreath", "breathes lightning a charge", powerup::kSpecial, powerup::kLightningBreath,
     true, 0},
    {"phoenix", "a fire-spitting companion", powerup::kSpecial, powerup::kPhoenix, false, 0},
    {"growth", "grows the character", powerup::kSpecial, powerup::kGrowth, false, 0},
    {"shrink", "shrinks the swarm", powerup::kSpecial, powerup::kEnemyShrink, false, 0},
    {"invisible", "unseen by the enemies", powerup::kSpecial, powerup::kInvisible, false, 0},
    {"xray", "sees inside the nearest closed chest", powerup::kSpecial, powerup::kXRay, false, 0},
    {"stoptime", "halts ordinary opponents and hazards", powerup::kSpecial, powerup::kStopTime,
     false, 0},
    {"turbo", "fills the turbo meter at once", powerup::kSpecial, powerup::kTurbo, false, 0},
    {"speed", "runs faster", powerup::kSpecial, powerup::kSpeedBoost, false, 4},
    {"mikey", "a decoy the swarm chases", powerup::kSpecial, powerup::kMikey, false, 0},
    {"handofdeath", "returns melee harm to the striker", powerup::kSpecial, powerup::kHandOfDeath,
     false, 0},
    {"healthvamp", "melee harm heals the character", powerup::kSpecial, powerup::kHealthVamp, false,
     0},
    {"pojo", "Pojo speaks for the character", powerup::kSpecial, powerup::kPojo, false, 0},
}};

struct Critter {
    std::string_view name;
    CombatantKind kind;
};
constexpr std::array<Critter, 3> kCritters{{{"golem", CombatantKind::Golem},
                                            {"general", CombatantKind::General},
                                            {"gargoyle", CombatantKind::Gargoyle}}};
constexpr std::array<std::string_view, 3> kGargoyleForms{"EAGL", "LION", "SERP"};

std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string upper(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

std::optional<s32> integer(std::string_view text) {
    s32 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional{value}
                                                                    : std::nullopt;
}

std::optional<f32> number(std::string_view text) {
    f32 value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional{value}
                                                                    : std::nullopt;
}

std::optional<Vec3> point(ConsoleCommands::Arguments arguments) {
    if (arguments.size() != 3) {
        return std::nullopt;
    }
    const auto x = number(arguments[0]);
    const auto y = number(arguments[1]);
    const auto z = number(arguments[2]);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{*x, *y, *z};
}

const Powerup* powerupNamed(std::string_view name) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPowerups, name, &Powerup::name);
    return found != kPowerups.end() ? &*found : nullptr;
}

const Potion* potionNamed(std::string_view name) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPotions, name, &Potion::name);
    return found != kPotions.end() ? &*found : nullptr;
}

/** A positive count from the one optional argument, else `fallback`; nothing on nonsense. */
std::optional<s32> countOf(ConsoleCommands::Arguments rest, s32 fallback, std::string& error) {
    if (rest.empty()) {
        return fallback;
    }
    const auto value = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
    if (!value.has_value() || *value <= 0) {
        error = ConsoleCommands::getUsage();
        return std::nullopt;
    }
    return value;
}

/** The name as a count of it reads: one key, two keys. */
std::string counted(const ConsoleCommands::Gift& gift, s32 count) {
    if (gift.name == "keys" || gift.name == "potions") {
        return count == 1 ? gift.name.substr(0, gift.name.size() - 1) : gift.name;
    }
    return gift.name;
}

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

/** The layout's record of an instance, or null for one with none. */
const ItemInfo* recordOf(const WorldLayout& layout, const ItemInstance& instance) {
    const auto& infos = layout.itemInfos();
    return instance.info >= 0 && static_cast<usize>(instance.info) < infos.size()
               ? &infos[static_cast<usize>(instance.info)]
               : nullptr;
}

bool critterName(std::string_view name) {
    return name == "GOLEM" || name == "GENERAL" || name.starts_with(kGargoylePrefix) ||
           name == "GAR";
}

/** Forms in a column with what each does beside them, a line each, then any notes. */
std::string usage(std::initializer_list<std::pair<std::string_view, std::string_view>> forms,
                  std::initializer_list<std::string_view> notes = {}) {
    usize widest = 0;
    for (const auto& [form, what] : forms) {
        widest = std::max(widest, form.size());
    }
    std::string text;
    for (const auto& [form, what] : forms) {
        if (!text.empty()) {
            text += '\n';
        }
        text += std::format("{:<{}}  {}", form, widest, what);
    }
    for (const std::string_view note : notes) {
        text += '\n';
        text += note;
    }
    return text;
}

bool sameIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/** A tier from an optional argument, else one; nothing when it is no tier. */
std::optional<s32> tierOf(std::optional<std::string_view> argument, std::string& error) {
    if (!argument.has_value()) {
        return 1;
    }
    const auto tier = integer(*argument);
    if (!tier.has_value() || *tier < 1 || *tier > kMostTier) {
        error = std::format("A tier is 1 to {}", kMostTier);
        return std::nullopt;
    }
    return tier;
}
} // namespace

ConsoleCommands::Listing ConsoleCommands::listingOf(Arguments arguments) {
    if (arguments.empty()) {
        return Listing::Unknown;
    }
    const std::string what = lower(arguments[0]);
    if (what == "maps") {
        return Listing::Maps;
    }
    if (what == "items") {
        return Listing::Items;
    }
    if (what == "inventory") {
        return Listing::Inventory;
    }
    if (what == "enemies") {
        return Listing::Enemies;
    }
    if (what == "critters") {
        return Listing::Critters;
    }
    if (what == "generators") {
        return Listing::Generators;
    }
    if (what == "objects") {
        return Listing::Objects;
    }
    if (what == "switches") {
        return Listing::Switches;
    }
    if (what == "characters") {
        return Listing::Characters;
    }
    return Listing::Unknown;
}

std::string ConsoleCommands::listUsage() {
    return usage({{"list maps", "every level, by realm"},
                  {"list items [map]", "what get knows, then a level's pickups"},
                  {"list inventory", "what your character is and carries"},
                  {"list enemies [map]", "the swarm's kinds, then a level's roster and generators"},
                  {"list critters [map]", "the great ones, then those a level places"},
                  {"list generators [map]", "a level's generators"},
                  {"list objects [map]", "a level's objects"},
                  {"list switches [map]", "a level's switches, for activate"},
                  {"list characters", "every playable character, locked or hidden too"}});
}

std::vector<usize> ConsoleCommands::entriesNamed(std::span<const NamedEntry> entries,
                                                 std::string_view word) {
    std::vector<usize> found;
    for (usize i = 0; i < entries.size(); ++i) {
        if (sameIgnoringCase(entries[i].token, word)) {
            return {i}; // a token names one thing alone
        }
    }
    for (usize i = 0; i < entries.size(); ++i) {
        const NamedEntry& entry = entries[i];
        if ((!entry.name.empty() && sameIgnoringCase(entry.name, word)) ||
            (!entry.alternate.empty() && sameIgnoringCase(entry.alternate, word))) {
            found.push_back(i);
        }
    }
    return found;
}

std::string ConsoleCommands::instanceName(const WorldLayout& layout, s32 instance) {
    const auto& instances = layout.itemInstances();
    if (instance < 0 || static_cast<usize>(instance) >= instances.size()) {
        return {};
    }
    const ItemInstance& placed = instances[static_cast<usize>(instance)];
    if (!placed.name.empty()) {
        return placed.name;
    }
    const ItemInfo* record = recordOf(layout, placed);
    return record != nullptr ? record->name : std::string{};
}

std::vector<ConsoleCommands::SwitchEntry> ConsoleCommands::switchesOf(const WorldLayout& layout) {
    // As LevelTriggers::bind reads them: a trigger per trigger item, in the instances' order,
    // driving the object its first parameter names, with its id and the next id it fires.
    std::vector<SwitchEntry> switches;
    std::vector<s32> nextIds;
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        const ItemInfo* record = recordOf(layout, instance);
        if (record == nullptr || record->type != ItemInfo::kTrigger) {
            continue;
        }
        SwitchEntry entry;
        entry.index = static_cast<s32>(switches.size());
        entry.id = instance.params[6];
        entry.name = instanceName(layout, static_cast<s32>(i));
        s16 object = 0;
        std::memcpy(&object, instance.params.data(), sizeof(object));
        if (object >= 0 && static_cast<usize>(object) < layout.objects().size()) {
            entry.target = layout.objects()[static_cast<usize>(object)].name;
        }
        entry.shootable = record->subtype == kShootableTriggerSubtype;
        nextIds.push_back(instance.params[7]);
        switches.push_back(std::move(entry));
    }
    // A switch another names as its next is fired through it, never stepped on.
    for (usize i = 0; i < switches.size(); ++i) {
        for (usize j = 0; j < switches.size(); ++j) {
            if (i != j && nextIds[j] != 0 && nextIds[j] == switches[i].id) {
                switches[i].chained = true;
            }
        }
    }
    return switches;
}

std::vector<std::string> ConsoleCommands::switchLines(std::span<const SwitchEntry> switches) {
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

std::vector<ConsoleCommands::NamedEntry>
ConsoleCommands::switchEntries(std::span<const SwitchEntry> switches) {
    std::vector<NamedEntry> entries;
    for (const SwitchEntry& entry : switches) {
        entries.push_back({std::format("switch#{}", entry.index), entry.name, entry.target});
    }
    return entries;
}

std::string ConsoleCommands::activateUsage() {
    return usage(
        {{"activate $SELECTED", "fires the switch selected, as if you stood on it"},
         {"activate switch#N", "the same, by the token list switches gives"},
         {"activate <id or name>", "the same, by a switch's id, its name or its target's"}},
        {"The camera follows what it opens, as it does for a player."});
}

std::optional<ConsoleCommands::LockRequest>
ConsoleCommands::parseLockRequest(Arguments arguments, std::string& error, std::string_view verb) {
    if (arguments.size() == 2 && sameIgnoringCase(arguments[0], "character")) {
        return LockRequest{true, arguments[1]};
    }
    if (arguments.size() == 1 && !sameIgnoringCase(arguments[0], "character")) {
        return LockRequest{false, arguments[0]};
    }
    error = lockUsage(verb);
    return std::nullopt;
}

std::string ConsoleCommands::lockUsage(std::string_view verb) {
    const bool unlock = verb == "unlock";
    const std::string fixture = std::format("{} $SELECTED|gate#N|chest#N|<name>", verb);
    const std::string character = std::format("{} character <code>", verb);
    return usage({{fixture, unlock ? "a door or chest a key opens, opened without one"
                                   : "a door shut again, or a chest wanting a key again"},
                  {character, unlock ? "a hidden character, for the select screen"
                                     : "a character hidden again; the starting eight stay"}},
                 {"list characters names the codes; list switches and get info name the rest."});
}

std::vector<std::string> ConsoleCommands::characterLines(u16 unlocks) {
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

std::string ConsoleCommands::gotoUsage() {
    return usage(
        {{"goto <map> [x y z]", "travels to a level, standing at a point when given"},
         {"goto x y z", "moves the party to that point here"},
         {"goto <legends map> [preview]", "walks an installed Legends map, or flies through it"},
         {"goto n64|dreamcast <map>", "the same, naming the map's platform"},
         {"goto back", "returns from a Legends map to the level left for it"}},
        {"list maps names the levels and the Legends maps installed."});
}

std::string ConsoleCommands::partyCountUsage() {
    return usage({{"party_count <1-4>", "sets the level up for a party that size"},
                  {"party_count off", "follows the players present again"},
                  {"party_count", "says which it is"}});
}

std::optional<LegendsPlatform> ConsoleCommands::platformOf(std::string_view word) {
    const std::string name = lower(word);
    if (name == "n64") {
        return LegendsPlatform::N64;
    }
    if (name == "dreamcast" || name == "dc") {
        return LegendsPlatform::Dreamcast;
    }
    return std::nullopt;
}

std::string_view ConsoleCommands::platformName(LegendsPlatform platform) {
    return platform == LegendsPlatform::N64 ? "N64" : "Dreamcast";
}

std::optional<std::optional<s32>> ConsoleCommands::parsePartyCount(Arguments arguments,
                                                                   std::string& error) {
    if (arguments.size() != 1) {
        error = partyCountUsage();
        return std::nullopt;
    }
    const std::string word = lower(arguments[0]);
    if (word == "off" || word == "auto") {
        return std::optional<s32>{};
    }
    const auto count = integer(word);
    if (!count.has_value() || *count < 1 || *count > kMostPartyCount) {
        error = partyCountUsage();
        return std::nullopt;
    }
    return count;
}

std::string ConsoleCommands::getUsage() {
    return usage({{"get <item> [amount]", "keys, potions, gold, health or a powerup"},
                  {"get exp <points>", "experience to add"},
                  {"get level <level>", std::format("the level to be, 1 to {}", kMaxLevel)},
                  {"get info [$SELECTED]", "what the thing clicked in the view is"}},
                 {"list items names the items."});
}

std::string ConsoleCommands::spawnUsage() {
    return usage(
        {{"spawn <enemy> [tier] [count] [map]", "so many of the swarm's kind, at a tier"},
         {"spawn generator <enemy>|special [tier] [map]",
          "a generator breeding that kind, or the temple's or underworld's mixed brood"},
         {"spawn golem|general|gargoyle [form] [map]",
          "a great one; a gargoyle's form is eagl, lion or serp"},
         {"spawn item <name>", "a pickup of this level, by its record"},
         {"spawn object <name> [map]", "a copy of a level's object"}},
        {"A map (E2) brings the kind's voices, a great one's costume or the object from that "
         "level; list enemies, critters, items and objects name what there is."});
}

bool ConsoleCommands::looksLikeMap(std::string_view word) {
    const std::string name = upper(word);
    if (name == "TOWER") {
        return true;
    }
    if (name.size() < 2 || name.size() > 3 || name.front() < 'A' || name.front() > 'Z') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<std::string> ConsoleCommands::listedMap(Arguments arguments) {
    return arguments.size() >= 2 ? std::optional{upper(arguments[1])} : std::nullopt;
}

std::optional<ConsoleCommands::Destination> ConsoleCommands::parseGoto(Arguments arguments,
                                                                       std::string& error) {
    error = gotoUsage();
    // A Legends map: by its platform, or by name alone, flown through with "preview".
    Arguments rest = arguments;
    Destination legends;
    if (!rest.empty()) {
        legends.platform = platformOf(rest[0]);
        if (legends.platform.has_value()) {
            rest = rest.subspan(1);
        }
    }
    if (!rest.empty() && rest.size() <= 2 && !integer(rest[0]).has_value() &&
        (rest.size() == 1 || lower(rest[1]) == kPreviewWord)) {
        const std::string word = lower(rest[0]);
        if (word == "back" || word == "return") {
            if (rest.size() == 1 && !legends.platform.has_value()) {
                return Destination{.back = true};
            }
            return std::nullopt;
        }
        if (legends.platform.has_value() || !looksLikeMap(word)) {
            legends.legends = std::string(rest[0]);
            legends.preview = rest.size() == 2;
            return legends;
        }
    }
    if (legends.platform.has_value()) {
        return std::nullopt;
    }
    if (arguments.size() == 1) {
        return Destination{.level = upper(arguments[0])};
    }
    if (arguments.size() == 3) {
        if (const auto at = point(arguments)) {
            return Destination{.position = at};
        }
        return std::nullopt;
    }
    if (arguments.size() == 4) {
        if (const auto at = point(arguments.subspan(1))) {
            return Destination{.level = upper(arguments[0]), .position = at};
        }
    }
    return std::nullopt;
}

std::optional<ConsoleCommands::Gift> ConsoleCommands::parseGet(Arguments arguments,
                                                               std::string& error) {
    if (arguments.empty()) {
        error = getUsage();
        return std::nullopt;
    }
    const std::string name = lower(arguments[0]);
    const Arguments rest = arguments.subspan(1);
    Gift gift;
    if (name == "info") {
        if (rest.size() > 1) {
            error = "get info [$SELECTED]: one thing, or what is selected";
            return std::nullopt;
        }
        gift.kind = Gift::Kind::Info;
        gift.name = rest.empty() ? "" : rest[0];
        return gift;
    }
    if (name == "exp" || name == "experience") {
        const auto points = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
        if (!points.has_value() || *points <= 0) {
            error = "get exp <points>: how much to add";
            return std::nullopt;
        }
        gift.kind = Gift::Kind::Experience;
        gift.amount = *points;
        gift.name = "exp";
        return gift;
    }
    if (name == "level") {
        const auto level = rest.size() == 1 ? integer(rest[0]) : std::nullopt;
        if (!level.has_value() || *level < 1 || *level > kMaxLevel) {
            error = std::format("get level <level>: 1 to {}", kMaxLevel);
            return std::nullopt;
        }
        gift.kind = Gift::Kind::Level;
        gift.amount = *level;
        gift.name = "level";
        return gift;
    }
    if (name == "key" || name == "keys") {
        const auto count = countOf(rest, kDefaultKeys, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Keys), *count};
        gift.name = "keys";
        return gift;
    }
    if (name == "potion" || name == "potions") {
        s32 count = kDefaultPotions;
        s32 kind = kDefaultPotionKind;
        for (const std::string& argument : rest) {
            if (const auto value = integer(argument); value.has_value() && *value > 0) {
                count = *value;
            } else if (const Potion* potion = potionNamed(lower(argument))) {
                kind = potion->kind;
            } else {
                error = std::format("No potion kind named {}; fire, electric, lightning or acid",
                                    argument);
                return std::nullopt;
            }
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Potion), count, static_cast<u32>(kind)};
        gift.name = "potions";
        return gift;
    }
    if (name == "gold") {
        const auto count = countOf(rest, kDefaultGold, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Gold), *count};
        gift.name = "gold";
        return gift;
    }
    if (name == "health" || name == "food") {
        const auto count = countOf(rest, kDefaultHealth, error);
        if (!count) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Food), *count};
        gift.name = "health";
        return gift;
    }
    const Powerup* powerup = powerupNamed(name);
    if (powerup == nullptr) {
        error = std::format("No item named {}; list items names them", arguments[0]);
        return std::nullopt;
    }
    gift.name = std::string(powerup->name);
    if (powerup->charged) {
        const auto charges = countOf(rest, kDefaultCharges, error);
        if (!charges) {
            return std::nullopt;
        }
        gift.offer = ItemOffer{powerup->kind, *charges, powerup->flags, kForGood};
        return gift;
    }
    f32 seconds = kDefaultSeconds;
    if (!rest.empty()) {
        const auto given = rest.size() == 1 ? number(rest[0]) : std::nullopt;
        if (!given.has_value() || *given == 0.0f) {
            error =
                std::format("get {} [seconds]: how long for; under nought for good", powerup->name);
            return std::nullopt;
        }
        seconds = *given < 0.0f ? kForGood : *given;
    }
    gift.offer =
        ItemOffer{powerup->kind, static_cast<s32>(powerup->charge), powerup->flags, seconds};
    return gift;
}

std::string ConsoleCommands::report(const Gift& gift, const ItemTaking& taking) {
    const Powerup* powerup = powerupNamed(gift.name);
    switch (taking.outcome) {
    case ItemTaking::Outcome::Taken:
    case ItemTaking::Outcome::Shown:
        if (powerup == nullptr) {
            return std::format("Got {} {}", taking.count, counted(gift, taking.count));
        }
        if (powerup->charged) {
            return std::format("Got {}, {} charges", gift.name, gift.offer.amount);
        }
        if (gift.offer.strength < 0.0f) {
            return std::format("Got {} for good", gift.name);
        }
        return std::format("Got {} for {:.0f} seconds", gift.name, gift.offer.strength);
    case ItemTaking::Outcome::PartTaken:
        return std::format("Got {} {}; no room for {} more", taking.count,
                           counted(gift, taking.count), taking.left);
    case ItemTaking::Outcome::HealthFull: return "Health is full already";
    case ItemTaking::Outcome::GoldFull:
    case ItemTaking::Outcome::KeysFull:
    case ItemTaking::Outcome::PotionsFull: return std::format("No room for more {}", gift.name);
    case ItemTaking::Outcome::AlreadyHeld: return std::format("Already holding {}", gift.name);
    case ItemTaking::Outcome::NotCarried: break;
    }
    return std::format("Cannot carry {}", gift.name);
}

bool ConsoleCommands::spawnableEnemy(s32 kind) {
    return (kind >= 0 && kind < kSwarmKindCount) || kind == kDeathKind || kind == kItKind;
}

std::optional<ConsoleCommands::Spawn> ConsoleCommands::parseSpawn(Arguments arguments,
                                                                  std::string& error) {
    if (arguments.empty()) {
        error = spawnUsage();
        return std::nullopt;
    }
    // A category word first says which reading is meant; without one the name decides.
    enum class Category : u8 { Any, Enemy, Critter, Generator, Item, Object };
    Category category = Category::Any;
    std::string first = lower(arguments[0]);
    Arguments rest = arguments.subspan(1);
    if (first == "enemy") {
        category = Category::Enemy;
    } else if (first == "critter") {
        category = Category::Critter;
    } else if (first == "generator") {
        category = Category::Generator;
    } else if (first == "item" || first == "pickup") {
        category = Category::Item;
    } else if (first == "object") {
        category = Category::Object;
    }
    if (category != Category::Any) {
        if (rest.empty()) {
            error = spawnUsage();
            return std::nullopt;
        }
        first = lower(rest[0]);
        rest = rest.subspan(1);
    }
    std::string name = upper(first);
    Spawn spawn;
    // A map comes last; it is whatever is left that is no number or form.
    const auto takeMap = [&](usize numbers) -> bool {
        if (rest.size() > numbers + 1) {
            error = spawnUsage();
            return false;
        }
        if (rest.size() == numbers + 1) {
            spawn.map = upper(rest.back());
            rest = rest.first(numbers);
        }
        return true;
    };
    const auto numbersAhead = [&] {
        usize count = 0;
        while (count < rest.size() && integer(rest[count]).has_value()) {
            ++count;
        }
        return count;
    };
    const auto tierAhead = [&](std::optional<s32>& tier) {
        tier =
            tierOf(rest.empty() ? std::nullopt : std::optional<std::string_view>{rest[0]}, error);
        return tier.has_value();
    };
    if (category == Category::Any || category == Category::Enemy) {
        if (const auto kind = enemyKindOf(name); kind.has_value() && spawnableEnemy(*kind)) {
            spawn.kind = Spawn::Kind::Enemy;
            spawn.enemyKind = *kind;
            spawn.name = name;
            const usize numbers = numbersAhead();
            if (numbers > 2 || !takeMap(numbers)) {
                error = spawnUsage();
                return std::nullopt;
            }
            std::optional<s32> tier;
            if (!tierAhead(tier)) {
                return std::nullopt;
            }
            spawn.tier = *tier;
            if (rest.size() > 1) {
                const auto value = integer(rest[1]);
                if (!value.has_value() || *value < 1 || *value > kMostSpawned) {
                    error = std::format("A count is 1 to {}", kMostSpawned);
                    return std::nullopt;
                }
                spawn.count = *value;
            }
            return spawn;
        }
        if (category == Category::Enemy) {
            error = std::format("No enemy kind named {}; list enemies names them", name);
            return std::nullopt;
        }
    }
    if (category == Category::Any || category == Category::Critter) {
        // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
        // NOLINTNEXTLINE(readability-qualified-auto)
        if (const auto critter = std::ranges::find(kCritters, first, &Critter::name);
            critter != kCritters.end()) {
            spawn.kind = Spawn::Kind::Critter;
            spawn.critter = critter->kind;
            spawn.name = std::string(critter->name);
            if (critter->kind == CombatantKind::Gargoyle) {
                spawn.form = std::string(kGargoylePrefix) + std::string(kGargoyleForms.front());
                if (!rest.empty() &&
                    std::ranges::find(kGargoyleForms, upper(rest[0])) != kGargoyleForms.end()) {
                    spawn.form = std::string(kGargoylePrefix) + upper(rest[0]);
                    rest = rest.subspan(1);
                }
            }
            if (!takeMap(0)) {
                error = critter->kind == CombatantKind::Gargoyle
                            ? "A gargoyle is an eagl, a lion or a serp, then a map at most"
                            : spawnUsage();
                return std::nullopt;
            }
            return spawn;
        }
        if (category == Category::Critter) {
            error = "A critter is a golem, a general or a gargoyle";
            return std::nullopt;
        }
    }
    if (category == Category::Generator) {
        const auto kind = enemyKindOf(name);
        spawn.brood = name == "SPECIAL";
        if (!spawn.brood && (!kind.has_value() || !spawnableEnemy(*kind))) {
            error = std::format("No enemy kind named {} for a generator; list enemies names them",
                                name);
            return std::nullopt;
        }
        spawn.kind = Spawn::Kind::Generator;
        spawn.enemyKind = spawn.brood ? -1 : *kind;
        spawn.name = name;
        const usize numbers = numbersAhead();
        if (numbers > 1 || !takeMap(numbers)) {
            error = spawnUsage();
            return std::nullopt;
        }
        std::optional<s32> tier;
        if (!tierAhead(tier)) {
            return std::nullopt;
        }
        spawn.tier = *tier;
        return spawn;
    }
    if (!takeMap(0)) {
        return std::nullopt;
    }
    // An object may be named after its map: spawn object E1 E1XLNSNC_CNDL01.
    if (spawn.map.has_value() && looksLikeMap(name) && !looksLikeMap(*spawn.map)) {
        std::swap(name, *spawn.map);
    }
    spawn.kind = category == Category::Item     ? Spawn::Kind::Item
                 : category == Category::Object ? Spawn::Kind::Object
                                                : Spawn::Kind::Thing;
    spawn.name = name;
    return spawn;
}

std::vector<std::string> ConsoleCommands::itemLines() {
    constexpr s32 kNameColumn = 18;
    std::vector<std::string> lines;
    const auto add = [&](std::string_view name, std::string_view what) {
        lines.push_back(std::format("{:<{}} {}", name, kNameColumn, what));
    };
    add("key [n]", std::format("keys for the doors ({})", kDefaultKeys));
    add("potion [n] [kind]", std::format("potions: fire, electric, lightning or acid ({} {})",
                                         kDefaultPotions, potionName(kDefaultPotionKind)));
    add("gold [n]", std::format("gold for the shop ({})", kDefaultGold));
    add("health [n]", std::format("health, as food gives it ({})", kDefaultHealth));
    add("exp <points>", "experience to add");
    add("level <level>", std::format("the level to be, 1 to {}", kMaxLevel));
    for (const Powerup& powerup : kPowerups) {
        if (powerup.charged) {
            add(std::format("{} [n]", powerup.name),
                std::format("{} ({} charges)", powerup.summary, kDefaultCharges));
        } else {
            add(std::format("{} [s]", powerup.name),
                std::format("{} ({:.0f} s; under nought for good)", powerup.summary,
                            kDefaultSeconds));
        }
    }
    return lines;
}

std::vector<std::string> ConsoleCommands::mapLines(const LevelCatalog& catalog) {
    std::vector<std::string> lines;
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        const char letter =
            realm.prefix.empty()
                ? '?'
                : static_cast<char>(std::toupper(static_cast<unsigned char>(realm.prefix.back())));
        std::string line = std::format("{} {} ({}):", letter, lower(realm.file), realm.id);
        for (usize i = 0; i < realm.levels.size(); ++i) {
            const std::string_view title =
                i < realm.titles.size() ? std::string_view(realm.titles[i]) : std::string_view{};
            line += std::format("{} {} {}", i == 0 ? "" : ",", realm.levels[i], title);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> ConsoleCommands::legendsMapLines(LegendsPlatform platform,
                                                          std::span<const std::string> names) {
    if (names.empty()) {
        return {};
    }
    std::string joined;
    for (const std::string& name : names) {
        joined += joined.empty() ? "" : ", ";
        joined += name;
    }
    return {std::format("{} Legends maps (goto <name>): {}", platformName(platform), joined)};
}

std::vector<std::string> ConsoleCommands::enemyKindLines() {
    std::string names;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        if (spawnableEnemy(kind)) {
            names += names.empty() ? "" : ", ";
            names += enemyKind(kind).name;
        }
    }
    return {std::format("Enemies (spawn <enemy> [tier 1-{}] [count]): {}", kMostTier, names)};
}

std::vector<std::string> ConsoleCommands::critterLines() {
    std::string forms;
    for (const std::string_view form : kGargoyleForms) {
        forms += forms.empty() ? "" : "|";
        forms += lower(form);
    }
    return {std::format("Critters (spawn <critter>): golem, general, gargoyle [{}]", forms)};
}

std::vector<std::string> ConsoleCommands::rosterLines(std::span<const LevelEnemy> roster) {
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

std::vector<std::string> ConsoleCommands::generatorLines(const WorldLayout& layout) {
    std::map<std::string, std::pair<s32, std::set<s32>>> kinds; // count and tiers by name
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
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

std::vector<std::string> ConsoleCommands::placedEnemyLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && !critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Placed enemies: none")
                           : std::format("Placed enemies: {}", joined(counts))};
}

std::vector<std::string> ConsoleCommands::placedCritterLines(const WorldLayout& layout) {
    std::map<std::string, s32> counts;
    for (const ItemInstance& instance : layout.itemInstances()) {
        const ItemInfo* info = recordOf(layout, instance);
        if (info != nullptr && info->type == ItemInfo::kPlacedEnemy && critterName(info->name)) {
            ++counts[info->name];
        }
    }
    return {counts.empty() ? std::string("Critters placed: none")
                           : std::format("Critters placed: {}", joined(counts))};
}

std::string_view ConsoleCommands::pickupKindName(s32 subtype) {
    switch (static_cast<ItemKind>(subtype)) {
    case ItemKind::Gold: return "gold";
    case ItemKind::Keys: return "keys";
    case ItemKind::Food: return "food";
    case ItemKind::Potion: return "potion";
    case ItemKind::WeaponPowerup: return "weapon powerup";
    case ItemKind::ArmorPowerup: return "armor powerup";
    case ItemKind::SpeedPowerup: return "speed powerup";
    case ItemKind::MagicPowerup: return "magic powerup";
    case ItemKind::SpecialPowerup: return "special powerup";
    case ItemKind::Runestone: return "runestone";
    case ItemKind::Legend: return "legend item";
    case ItemKind::Scroll: return "scroll";
    case ItemKind::Crystal: return "crystal";
    case ItemKind::GargoyleKey: return "gargoyle piece";
    }
    return "pickup";
}

std::vector<std::string> ConsoleCommands::pickupLines(const WorldLayout& layout) {
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

std::vector<std::string> ConsoleCommands::objectLines(const WorldLayout& layout) {
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

const ItemInfo* ConsoleCommands::generatorRecord(const WorldLayout& layout, std::string_view name) {
    const auto found = std::ranges::find_if(layout.itemInfos(), [&](const ItemInfo& info) {
        return info.type == ItemInfo::kGenerator && sameIgnoringCase(info.name, name);
    });
    return found != layout.itemInfos().end() ? &*found : nullptr;
}

bool ConsoleCommands::hasObject(const WorldLayout& layout, std::string_view name) {
    return std::ranges::any_of(layout.objects(), [&](const WorldObject& object) {
        return sameIgnoringCase(object.name, name);
    });
}

bool ConsoleCommands::hasPickup(const WorldLayout& layout, std::string_view name) {
    return std::ranges::any_of(layout.itemInfos(), [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup && sameIgnoringCase(info.name, name);
    });
}

std::string_view ConsoleCommands::potionName(s32 kind) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPotions, kind, &Potion::kind);
    return found != kPotions.end() ? found->name : kPotions.front().name;
}

std::string ConsoleCommands::powerupName(const PowerupSlot& slot) {
    std::string names;
    for (const Powerup& powerup : kPowerups) {
        if (powerup.kind == slot.kind && (slot.flags & powerup.flags) == powerup.flags) {
            names += names.empty() ? "" : "+";
            names += powerup.name;
        }
    }
    return names.empty() ? std::format("kind {} flags {:#x}", slot.kind, slot.flags) : names;
}

std::vector<std::string> ConsoleCommands::inventoryLines(const CharacterSave& save) {
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

void ConsoleCommands::unlockTower(CharacterSave& save) {
    ClassProgress& progress = save.progress();
    for (s32 realm = 0; realm < static_cast<s32>(LevelRecord::kRealmCount); ++realm) {
        for (s32 level = 0; level < LevelRecord::kLevelsPerRealm; ++level) {
            progress.levels.recordBeaten(realm, level);
        }
    }
    for (s32 rune = 0; rune < Relics::kRuneCount; ++rune) {
        progress.relics.addRune(rune);
    }
    // The bosses' shards by their realms' places in the tower's order: the eight realms',
    // the temple's and the underworld's (TowerAccess::kBattlefieldShards).
    for (s32 order = 1; (TowerAccess::kBattlefieldShards >> order) != 0; ++order) {
        if ((TowerAccess::kBattlefieldShards & (1U << order)) != 0) {
            progress.relics.addShard(order);
        }
    }
    progress.relics.pendingRunes = 0;
    progress.relics.pendingShards = 0;
    progress.relics.pendingCeremonies = 0;
    // The gargoyle statues' wings: each kind's pieces kept as the tower keeps them once it
    // has congratulated the collector, so no ceremony waits.
    progress.relics.gargoylePieces.fill(-1);
    for (const s32 gate : TowerAccess::kWorldGates) {
        if (gate != 0) {
            progress.crystals[static_cast<usize>(gate)] = std::max(
                progress.crystals[static_cast<usize>(gate)], LevelTriggers::crystalsNeeded(gate));
        }
    }
    progress.unlocked = (1U << static_cast<u32>(kRealmCount)) - 1U;
}

} // namespace gdl::game
