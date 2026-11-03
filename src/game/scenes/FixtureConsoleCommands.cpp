#include "game/scenes/FixtureConsoleCommands.h"

#include <format>
#include <memory>
#include <string>
#include <utility>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Assert.h"
#include "engine/core/Types.h"

#include "game/characters/ClassData.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleReach.h"
#include "game/console/DevConsole.h"
#include "game/level/Chests.h"
#include "game/level/InstanceNames.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/level/LockedGates.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

namespace {
using Arguments = ConsoleCommand::Arguments;

/** A door or chest of the level in play the console can name. */
struct ConsoleFixture {
    SceneSelection::Kind kind = SceneSelection::Kind::Gate;
    usize index = 0;
    NamedEntry entry;
};

std::vector<SwitchEntry> consoleSwitches(const FixtureConsoleAccess& access) {
    const LevelWorld* world = access.world();
    return world != nullptr ? switchesInPlay(*world) : std::vector<SwitchEntry>{};
}

std::vector<ConsoleFixture> consoleFixtures(const FixtureConsoleAccess& access) {
    std::vector<ConsoleFixture> fixtures;
    const PlayScene* scene = access.scene();
    const LevelWorld* world = access.world();
    if (scene == nullptr || world == nullptr) {
        return fixtures;
    }
    const WorldLayout& layout = world->layout();
    const LockedGates& gates = scene->gates();
    for (usize i = 0; i < gates.size(); ++i) {
        if (gates.gate(i).shown) {
            fixtures.push_back(
                {SceneSelection::Kind::Gate,
                 i,
                 {std::format("gate#{}", i), instanceName(layout, gates.gate(i).instance)}});
        }
    }
    const Chests& chests = scene->chests();
    for (usize i = 0; i < chests.size(); ++i) {
        if (chests.chest(i).shown && !chests.chest(i).gone) {
            fixtures.push_back(
                {SceneSelection::Kind::Chest,
                 i,
                 {std::format("chest#{}", i), instanceName(layout, chests.chest(i).instance)}});
        }
    }
    return fixtures;
}

std::vector<std::string> activateWords(const FixtureConsoleAccess& access, Arguments arguments) {
    std::vector<std::string> words;
    if (arguments.size() > 1) {
        return words;
    }
    for (const NamedEntry& entry : switchEntries(consoleSwitches(access))) {
        words.push_back(entry.token);
        if (!entry.name.empty()) {
            words.push_back(entry.name);
        }
    }
    return words;
}

std::vector<std::string> lockWords(const FixtureConsoleAccess& access, Arguments arguments) {
    std::vector<std::string> words;
    if (arguments.size() > 2) {
        return words;
    }
    if (arguments.size() == 2) {
        if (arguments[0] == "character" || arguments[0] == "CHARACTER") {
            for (s32 i = kStartingClassCount; i < kClassCount; ++i) {
                words.emplace_back(classCode(i));
            }
        }
        return words;
    }
    words.emplace_back("character");
    for (const ConsoleFixture& fixture : consoleFixtures(access)) {
        words.push_back(fixture.entry.token);
        if (!fixture.entry.name.empty()) {
            words.push_back(fixture.entry.name);
        }
    }
    return words;
}

void activate(const FixtureConsoleAccess& access, DevConsole& console, Arguments arguments) {
    if (arguments.size() != 1) {
        console.print(activateUsage());
        return;
    }
    PlayScene* scene = access.scene();
    if (scene == nullptr) {
        console.print("Nothing is in play.");
        return;
    }
    const auto switches = consoleSwitches(access);
    const auto entries = switchEntries(switches);
    const std::vector<usize> found = entriesNamed(entries, arguments[0]);
    if (found.empty()) {
        console.print(std::format("{} names no switch here; list switches names them, or click "
                                  "a pad and activate $SELECTED",
                                  arguments[0]));
        return;
    }
    if (found.size() > 1) {
        std::string tokens;
        for (const usize index : found) {
            tokens += tokens.empty() ? "" : ", ";
            tokens += entries[index].token;
        }
        console.print(std::format("{} names {} switches: {}; say which by its token", arguments[0],
                                  found.size(), tokens));
        return;
    }
    const SwitchEntry& entry = switches[found[0]];
    if (!scene->pressSwitch(static_cast<usize>(entry.index))) {
        console.print(std::format("switch#{} {} is not for this party, or stands open for good",
                                  entry.index, entry.name));
        return;
    }
    console.print(entry.target.empty()
                      ? std::format("Pressed switch#{} {}", entry.index, entry.name)
                      : std::format("Pressed switch#{} {}: {} answers, the camera following",
                                    entry.index, entry.name, entry.target));
}

/** lock's or unlock's body, as `verb` says, for a door or chest. */
void changeLock(const FixtureConsoleAccess& access, DevConsole& console, Arguments arguments,
                LockVerb verb) {
    const bool unlock = verb == LockVerb::Unlock;
    const auto request = parseLockRequest(arguments, verb);
    if (!request.has_value()) {
        console.print(request.error());
        return;
    }
    GDL_VERIFY(!request->character, "a character's code is the profile's form, claimed first");
    PlayScene* scene = access.scene();
    if (scene == nullptr) {
        console.print("Nothing is in play.");
        return;
    }
    const auto fixtures = consoleFixtures(access);
    std::vector<NamedEntry> entries;
    entries.reserve(fixtures.size());
    for (const ConsoleFixture& fixture : fixtures) {
        entries.push_back(fixture.entry);
    }
    const std::vector<usize> found = entriesNamed(entries, request->what);
    if (found.empty()) {
        console.print(std::format("{} names no door or chest here; gate#N, chest#N, a name or "
                                  "$SELECTED after a click does",
                                  request->what));
        return;
    }
    if (found.size() > 1) {
        std::string tokens;
        for (const usize index : found) {
            tokens += tokens.empty() ? "" : ", ";
            tokens += entries[index].token;
        }
        console.print(std::format("{} names {} things: {}; say which by its token", request->what,
                                  found.size(), tokens));
        return;
    }
    const ConsoleFixture& fixture = fixtures[found[0]];
    const std::string label = fixture.entry.name.empty()
                                  ? fixture.entry.token
                                  : std::format("{} {}", fixture.entry.token, fixture.entry.name);
    if (fixture.kind == SceneSelection::Kind::Gate) {
        const bool done =
            unlock ? scene->unlockGate(fixture.index) : scene->lockGate(fixture.index);
        if (!done) {
            console.print(std::format("{} is {} already", label, unlock ? "open" : "shut"));
        } else {
            console.print(unlock ? std::format("{} unlocked: it swings open", label)
                                 : std::format("{} locked: shut until a key is spent", label));
        }
        return;
    }
    if (!scene->setChestLocked(fixture.index, !unlock)) {
        console.print(std::format("{} is open already", label));
        return;
    }
    console.print(unlock ? std::format("{} unlocked: it opens without a key", label)
                         : std::format("{} locked: a key opens it", label));
}
} // namespace

std::vector<SwitchEntry> switchesInPlay(const LevelWorld& world) {
    std::vector<SwitchEntry> switches;
    const WorldLayout& layout = world.layout();
    const LevelTriggers& triggers = world.triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& trigger = triggers.trigger(i);
        SwitchEntry entry;
        entry.index = static_cast<s32>(i);
        entry.id = trigger.id;
        entry.name = instanceName(layout, trigger.instance);
        if (trigger.target >= 0 && static_cast<usize>(trigger.target) < layout.objects().size()) {
            entry.target = layout.objects()[static_cast<usize>(trigger.target)].name;
        }
        entry.shootable = trigger.shootable;
        entry.chained = trigger.chained;
        entry.fired = trigger.fired;
        entry.enabled = trigger.enabled;
        switches.push_back(std::move(entry));
    }
    return switches;
}

void registerFixtureConsoleCommands(ConsoleHost& host, const FixtureConsoleAccess& access) {
    host.add(std::make_unique<LambdaCommand>(
        "activate", "$SELECTED, switch#N, an id or a name: fires that switch as if stood on",
        ConsoleReach::OfflineOnly,
        [access](DevConsole& console, Arguments arguments) {
            activate(access, console, arguments);
        },
        [access](Arguments arguments) { return activateWords(access, arguments); }));
    host.add(std::make_unique<LambdaCommand>(
        "unlock", "a door or chest without a key, or character <code> for the select screen",
        ConsoleReach::OfflineOnly,
        [access](DevConsole& console, Arguments arguments) {
            changeLock(access, console, arguments, LockVerb::Unlock);
        },
        [access](Arguments arguments) { return lockWords(access, arguments); }));
    host.add(std::make_unique<LambdaCommand>(
        "lock", "a door shut or a chest wanting a key again, or character <code> hidden again",
        ConsoleReach::OfflineOnly,
        [access](DevConsole& console, Arguments arguments) {
            changeLock(access, console, arguments, LockVerb::Lock);
        },
        [access](Arguments arguments) { return lockWords(access, arguments); }));
}

} // namespace gdl::game
