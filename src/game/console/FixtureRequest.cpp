#include "game/console/FixtureRequest.h"

#include <array>
#include <cstring>
#include <format>
#include <utility>

#include "game/console/ConsoleUsage.h"
#include "game/level/InstanceNames.h"

namespace gdl::game {

namespace {
constexpr s32 kShootableTriggerSubtype = 31; ///< a target on the wall (LevelTrigger)
} // namespace

std::vector<usize> entriesNamed(std::span<const NamedEntry> entries, std::string_view word) {
    std::vector<usize> found;
    for (usize i = 0; i < entries.size(); ++i) {
        if (sameNameIgnoringCase(entries[i].token, word)) {
            return {i}; // a token names one thing alone
        }
    }
    for (usize i = 0; i < entries.size(); ++i) {
        const NamedEntry& entry = entries[i];
        if ((!entry.name.empty() && sameNameIgnoringCase(entry.name, word)) ||
            (!entry.alternate.empty() && sameNameIgnoringCase(entry.alternate, word))) {
            found.push_back(i);
        }
    }
    return found;
}

std::vector<SwitchEntry> switchesOf(const WorldLayout& layout) {
    // As LevelTriggers::bind reads them: a trigger per trigger item, in the instances' order,
    // driving the object its first parameter names, with its id and the next id it fires.
    std::vector<SwitchEntry> switches;
    std::vector<s32> nextIds;
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        const ItemInfo* record = instanceRecord(layout, instance);
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

std::vector<NamedEntry> switchEntries(std::span<const SwitchEntry> switches) {
    std::vector<NamedEntry> entries;
    for (const SwitchEntry& entry : switches) {
        entries.push_back({std::format("switch#{}", entry.index), entry.name, entry.target});
    }
    return entries;
}

std::string activateUsage() {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kRows{
        {{"activate $SELECTED", "fires the switch selected, as if you stood on it"},
         {"activate switch#N", "the same, by the token list switches gives"},
         {"activate <id or name>", "the same, by a switch's id, its name or its target's"}}};
    constexpr std::array<std::string_view, 1> kNotes{
        "The camera follows what it opens, as it does for a player."};
    return consoleUsage(kRows, kNotes);
}

gdl::Expected<LockRequest, std::string> parseLockRequest(ConsoleCommand::Arguments arguments,
                                                         LockVerb verb) {
    if (arguments.size() == 2 && sameNameIgnoringCase(arguments[0], "character")) {
        return LockRequest{true, arguments[1]};
    }
    if (arguments.size() == 1 && !sameNameIgnoringCase(arguments[0], "character")) {
        return LockRequest{false, arguments[0]};
    }
    return gdl::Unexpected(lockUsage(verb));
}

std::string lockUsage(LockVerb verb) {
    const bool unlock = verb == LockVerb::Unlock;
    const std::string_view word = unlock ? "unlock" : "lock";
    const std::string fixture = std::format("{} $SELECTED|gate#N|chest#N|<name>", word);
    const std::string character = std::format("{} character <code>", word);
    const std::array<std::pair<std::string_view, std::string_view>, 2> rows{
        {{fixture, unlock ? "a door or chest a key opens, opened without one"
                          : "a door shut again, or a chest wanting a key again"},
         {character, unlock ? "a hidden character, for the select screen"
                            : "a character hidden again; the starting eight stay"}}};
    constexpr std::array<std::string_view, 1> kNotes{
        "list characters names the codes; list switches and get info name the rest."};
    return consoleUsage(rows, kNotes);
}

} // namespace gdl::game
