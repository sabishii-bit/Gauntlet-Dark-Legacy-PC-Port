#include "game/console/SpawnRequest.h"

#include <algorithm>
#include <format>
#include <utility>

#include "game/console/ConsoleArguments.h"
#include "game/console/ConsoleCommands.h"
#include "game/console/ConsoleUsage.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {

namespace {
struct Critter {
    std::string_view name;
    CombatantKind kind;
};
constexpr std::array<Critter, 3> kCritters{{{"golem", CombatantKind::Golem},
                                            {"general", CombatantKind::General},
                                            {"gargoyle", CombatantKind::Gargoyle}}};

/** A tier from an optional argument, else one; the reason when it is no tier. */
gdl::Expected<s32, std::string> tierOf(std::optional<std::string_view> argument) {
    if (!argument.has_value()) {
        return 1;
    }
    const auto tier = integerArgument(*argument);
    if (!tier.has_value() || *tier < 1 || *tier > SpawnRequest::kMostTier) {
        return gdl::Unexpected(std::format("A tier is 1 to {}", SpawnRequest::kMostTier));
    }
    return *tier;
}
} // namespace

std::string spawnUsage() {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kRows{
        {{"spawn <enemy> [tier] [count] [map]", "so many of the swarm's kind, at a tier"},
         {"spawn generator <enemy>|special [tier] [map]",
          "a generator breeding that kind, or the temple's or underworld's mixed brood"},
         {"spawn golem|general|gargoyle [form] [map]",
          "a great one; a gargoyle's form is eagl, lion or serp"},
         {"spawn item <name>", "a pickup of this level, by its record"},
         {"spawn object <name> [map]", "a copy of a level's object"}}};
    constexpr std::array<std::string_view, 1> kNotes{
        "A map (E2) brings the kind's voices, a great one's costume or the object from that "
        "level; list enemies, critters, items and objects name what there is."};
    return consoleUsage(kRows, kNotes);
}

bool spawnableEnemy(s32 kind) {
    return (kind >= 0 && kind < kSwarmKindCount) || kind == kDeathKind || kind == kItKind;
}

gdl::Expected<SpawnRequest, std::string> parseSpawn(ConsoleCommand::Arguments arguments) {
    if (arguments.empty()) {
        return gdl::Unexpected(spawnUsage());
    }
    // A category word first says which reading is meant; without one the name decides.
    enum class Category : u8 { Any, Enemy, Critter, Generator, Item, Object };
    Category category = Category::Any;
    std::string first = lowerArgument(arguments[0]);
    ConsoleCommand::Arguments rest = arguments.subspan(1);
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
            return gdl::Unexpected(spawnUsage());
        }
        first = lowerArgument(rest[0]);
        rest = rest.subspan(1);
    }
    std::string name = upperArgument(first);
    SpawnRequest spawn;
    // A map comes last; it is whatever is left that is no number or form. False when more
    // than that is left.
    const auto takeMap = [&](usize numbers) {
        if (rest.size() > numbers + 1) {
            return false;
        }
        if (rest.size() == numbers + 1) {
            spawn.map = upperArgument(rest.back());
            rest = rest.first(numbers);
        }
        return true;
    };
    const auto numbersAhead = [&] {
        usize count = 0;
        while (count < rest.size() && integerArgument(rest[count]).has_value()) {
            ++count;
        }
        return count;
    };
    const auto tierAhead = [&] {
        return tierOf(rest.empty() ? std::nullopt : std::optional<std::string_view>{rest[0]});
    };
    if (category == Category::Any || category == Category::Enemy) {
        if (const auto kind = enemyKindOf(name); kind.has_value() && spawnableEnemy(*kind)) {
            spawn.kind = SpawnRequest::Kind::Enemy;
            spawn.enemyKind = *kind;
            spawn.name = name;
            const usize numbers = numbersAhead();
            if (numbers > 2 || !takeMap(numbers)) {
                return gdl::Unexpected(spawnUsage());
            }
            const auto tier = tierAhead();
            if (!tier) {
                return gdl::Unexpected(tier.error());
            }
            spawn.tier = *tier;
            if (rest.size() > 1) {
                const auto value = integerArgument(rest[1]);
                if (!value.has_value() || *value < 1 || *value > SpawnRequest::kMostSpawned) {
                    return gdl::Unexpected(
                        std::format("A count is 1 to {}", SpawnRequest::kMostSpawned));
                }
                spawn.count = *value;
            }
            return spawn;
        }
        if (category == Category::Enemy) {
            return gdl::Unexpected(
                std::format("No enemy kind named {}; list enemies names them", name));
        }
    }
    if (category == Category::Any || category == Category::Critter) {
        // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
        // NOLINTNEXTLINE(readability-qualified-auto)
        if (const auto critter = std::ranges::find(kCritters, first, &Critter::name);
            critter != kCritters.end()) {
            spawn.kind = SpawnRequest::Kind::Critter;
            spawn.critter = critter->kind;
            spawn.name = std::string(critter->name);
            if (critter->kind == CombatantKind::Gargoyle) {
                spawn.form = std::string(SpawnRequest::kGargoylePrefix) +
                             std::string(SpawnRequest::kGargoyleForms.front());
                if (!rest.empty() &&
                    std::ranges::find(SpawnRequest::kGargoyleForms, upperArgument(rest[0])) !=
                        SpawnRequest::kGargoyleForms.end()) {
                    spawn.form =
                        std::string(SpawnRequest::kGargoylePrefix) + upperArgument(rest[0]);
                    rest = rest.subspan(1);
                }
            }
            if (!takeMap(0)) {
                return gdl::Unexpected(
                    critter->kind == CombatantKind::Gargoyle
                        ? std::string("A gargoyle is an eagl, a lion or a serp, then a map at most")
                        : spawnUsage());
            }
            return spawn;
        }
        if (category == Category::Critter) {
            return gdl::Unexpected(std::string("A critter is a golem, a general or a gargoyle"));
        }
    }
    if (category == Category::Generator) {
        const auto kind = enemyKindOf(name);
        spawn.brood = name == "SPECIAL";
        if (!spawn.brood && (!kind.has_value() || !spawnableEnemy(*kind))) {
            return gdl::Unexpected(std::format(
                "No enemy kind named {} for a generator; list enemies names them", name));
        }
        spawn.kind = SpawnRequest::Kind::Generator;
        spawn.enemyKind = spawn.brood ? -1 : *kind;
        spawn.name = name;
        const usize numbers = numbersAhead();
        if (numbers > 1 || !takeMap(numbers)) {
            return gdl::Unexpected(spawnUsage());
        }
        const auto tier = tierAhead();
        if (!tier) {
            return gdl::Unexpected(tier.error());
        }
        spawn.tier = *tier;
        return spawn;
    }
    if (!takeMap(0)) {
        return gdl::Unexpected(spawnUsage());
    }
    // An object may be named after its map: spawn object E1 E1XLNSNC_CNDL01.
    if (spawn.map.has_value() && ConsoleCommands::looksLikeMap(name) &&
        !ConsoleCommands::looksLikeMap(*spawn.map)) {
        std::swap(name, *spawn.map);
    }
    spawn.kind = category == Category::Item     ? SpawnRequest::Kind::Item
                 : category == Category::Object ? SpawnRequest::Kind::Object
                                                : SpawnRequest::Kind::Thing;
    spawn.name = name;
    return spawn;
}

const ItemInfo* generatorRecord(const WorldLayout& layout, std::string_view name) {
    const auto found = std::ranges::find_if(layout.itemInfos(), [&](const ItemInfo& info) {
        return info.type == ItemInfo::kGenerator && sameNameIgnoringCase(info.name, name);
    });
    return found != layout.itemInfos().end() ? &*found : nullptr;
}

} // namespace gdl::game
