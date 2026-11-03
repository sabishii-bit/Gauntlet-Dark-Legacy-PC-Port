#include "game/app/OfflineConsoleCommands.h"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/console/LevelListingLines.h"
#include "game/console/PartyCountRequest.h"
#include "game/level/Chests.h"
#include "game/level/InstanceNames.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LockedGates.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {

OfflineConsoleCommands::OfflineConsoleCommands(Ports ports) : m_ports(std::move(ports)) {}

bool OfflineConsoleCommands::consoleLevel(DevConsole& console,
                                          const std::optional<std::string>& map,
                                          ConsoleLevel& level) {
    if (!map.has_value()) {
        const auto* world = consoleWorld();
        if (world == nullptr) {
            console.print("Nothing is in play.");
            return false;
        }
        level.name = world->ref().name;
        level.layout = &world->layout();
        if (const LevelInfo* info = world->level()) {
            level.roster = info->enemies;
        }
        return true;
    }
    const std::optional<LevelRef> ref =
        *map == "TOWER" ? std::optional{LevelRef::tower()} : m_ports.levels.byName(*map);
    if (!ref.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *map));
        return false;
    }
    if (!level.loaded.load(m_ports.unpackedRoot / ref->directory)) {
        console.print(std::format("{} is not unpacked", ref->name));
        return false;
    }
    level.name = ref->name;
    level.layout = &level.loaded;
    if (level.data.load(m_ports.unpackedRoot / ref->worldDataFile())) {
        if (const LevelInfo* info = level.data.level(ref->name)) {
            level.roster = info->enemies;
        }
    }
    return true;
}

void OfflineConsoleCommands::list(DevConsole& console, Arguments arguments) {
    const auto say = [&](const std::vector<std::string>& lines) {
        for (const std::string& line : lines) {
            console.print(line);
        }
    };
    const Listing listing = listingOf(arguments);
    const std::optional<std::string> map = listedMap(arguments);
    switch (listing) {
    case Listing::Maps:
        say(mapLines(m_ports.levels));
        // The Legends maps installed stand with them, each platform's on a line; nothing is
        // said of a platform without any.
        say(m_ports.legendsMapLines());
        return;
    case Listing::Inventory:
        if (const PlayerActor* actor =
                consoleScene() ? consoleScene()->actor(consolePlayer()) : nullptr) {
            say(inventoryLines(actor->save()));
        } else {
            console.print("Nobody is in.");
        }
        return;
    case Listing::Unknown: console.print(listUsage()); return;
    case Listing::Characters:
        if (m_ports.saves.opened() ||
            m_ports.saves.open(m_ports.config.saveDirectory(), m_ports.config.save.slots)) {
            say(characterLines(m_ports.saves.classUnlocks()));
        } else {
            console.print("The saves cannot be opened, so what is unlocked is unknown.");
        }
        return;
    case Listing::Switches:
        if (!map.has_value() && consoleWorld() != nullptr) {
            console.print(std::format("In {}:", consoleWorld()->ref().name));
            say(switchLines(consoleSwitches()));
            return;
        }
        break;
    case Listing::Items:
        if (!map.has_value()) {
            say(itemLines());
        }
        break;
    case Listing::Enemies:
        if (!map.has_value()) {
            say(enemyKindLines());
        }
        break;
    case Listing::Critters: say(critterLines()); break;
    case Listing::Generators:
    case Listing::Objects: break;
    }
    ConsoleLevel level;
    if (!consoleLevel(console, map, level)) {
        return;
    }
    console.print(std::format("In {}:", level.name));
    switch (listing) {
    case Listing::Items: say(pickupLines(*level.layout)); break;
    case Listing::Enemies:
        say(rosterLines(level.roster));
        say(generatorLines(*level.layout));
        say(placedEnemyLines(*level.layout));
        break;
    case Listing::Critters: say(placedCritterLines(*level.layout)); break;
    case Listing::Generators:
        say(generatorLines(*level.layout));
        console.print("spawn generator <enemy> [tier] stands another");
        break;
    case Listing::Objects: say(objectLines(*level.layout)); break;
    case Listing::Switches: say(switchLines(switchesOf(*level.layout))); break;
    case Listing::Maps:
    case Listing::Inventory:
    case Listing::Characters:
    case Listing::Unknown: break;
    }
}

std::vector<SwitchEntry> OfflineConsoleCommands::consoleSwitches() const {
    std::vector<SwitchEntry> switches;
    const LevelWorld* world = consoleWorld();
    if (world == nullptr) {
        return switches;
    }
    const WorldLayout& layout = world->layout();
    const LevelTriggers& triggers = world->triggers();
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

std::vector<OfflineConsoleCommands::ConsoleFixture>
OfflineConsoleCommands::consoleFixtures() const {
    std::vector<ConsoleFixture> fixtures;
    const PlayScene* scene = consoleScene();
    const LevelWorld* world = consoleWorld();
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

std::vector<std::string> OfflineConsoleCommands::activateWords(Arguments arguments) const {
    std::vector<std::string> words;
    if (arguments.size() > 1) {
        return words;
    }
    for (const NamedEntry& entry : switchEntries(consoleSwitches())) {
        words.push_back(entry.token);
        if (!entry.name.empty()) {
            words.push_back(entry.name);
        }
    }
    return words;
}

std::vector<std::string> OfflineConsoleCommands::lockWords(Arguments arguments) const {
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
    for (const ConsoleFixture& fixture : consoleFixtures()) {
        words.push_back(fixture.entry.token);
        if (!fixture.entry.name.empty()) {
            words.push_back(fixture.entry.name);
        }
    }
    return words;
}

void OfflineConsoleCommands::activate(DevConsole& console, Arguments arguments) {
    if (arguments.size() != 1) {
        console.print(activateUsage());
        return;
    }
    if (consoleScene() == nullptr || playScene() == nullptr || !playScene()->isOpen()) {
        console.print("Nothing is in play.");
        return;
    }
    const auto switches = consoleSwitches();
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
    if (!playScene()->pressSwitch(static_cast<usize>(entry.index))) {
        console.print(std::format("switch#{} {} is not for this party, or stands open for good",
                                  entry.index, entry.name));
        return;
    }
    console.print(entry.target.empty()
                      ? std::format("Pressed switch#{} {}", entry.index, entry.name)
                      : std::format("Pressed switch#{} {}: {} answers, the camera following",
                                    entry.index, entry.name, entry.target));
}

void OfflineConsoleCommands::lock(DevConsole& console, Arguments arguments) {
    changeLock(console, arguments, LockVerb::Lock);
}

void OfflineConsoleCommands::unlock(DevConsole& console, Arguments arguments) {
    changeLock(console, arguments, LockVerb::Unlock);
}

void OfflineConsoleCommands::changeLock(DevConsole& console, Arguments arguments, LockVerb verb) {
    const bool unlock = verb == LockVerb::Unlock;
    const auto request = parseLockRequest(arguments, verb);
    if (!request.has_value()) {
        console.print(request.error());
        return;
    }
    if (request->character) {
        const auto index = classIndexOf(request->what);
        if (!index.has_value()) {
            console.print(
                std::format("No character is coded {}; list characters names them", request->what));
            return;
        }
        if (*index < kStartingClassCount) {
            console.print(std::format("{} is always there to choose", classCode(*index)));
            return;
        }
        if (!m_ports.saves.opened() &&
            !m_ports.saves.open(m_ports.config.saveDirectory(), m_ports.config.save.slots)) {
            console.print("The saves cannot be opened.");
            return;
        }
        const auto mask = static_cast<u16>(1U << (*index - kStartingClassCount));
        const bool written =
            unlock ? m_ports.saves.unlockClasses(mask) : m_ports.saves.lockClasses(mask);
        if (!unlock && playScene() != nullptr && playScene()->isOpen()) {
            // The party in play carries the old reward bits; they would bring it back.
            for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
                if (CharacterSave* save = playScene()->liveSave(player)) {
                    save->classUnlock &= static_cast<u16>(~mask);
                }
            }
        }
        console.print(
            std::format("{} {}{}", classCode(*index),
                        unlock ? "unlocked: the select screen offers it"
                               : "locked: the select screen hides it again",
                        written ? "" : " (for this session; the profile was not written)"));
        return;
    }
    if (consoleScene() == nullptr || playScene() == nullptr || !playScene()->isOpen()) {
        console.print("Nothing is in play.");
        return;
    }
    const auto fixtures = consoleFixtures();
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
            unlock ? playScene()->unlockGate(fixture.index) : playScene()->lockGate(fixture.index);
        if (!done) {
            console.print(std::format("{} is {} already", label, unlock ? "open" : "shut"));
        } else {
            console.print(unlock ? std::format("{} unlocked: it swings open", label)
                                 : std::format("{} locked: shut until a key is spent", label));
        }
        return;
    }
    if (!playScene()->setChestLocked(fixture.index, !unlock)) {
        console.print(std::format("{} is open already", label));
        return;
    }
    console.print(unlock ? std::format("{} unlocked: it opens without a key", label)
                         : std::format("{} locked: a key opens it", label));
}

std::vector<std::string> OfflineConsoleCommands::gotoWords(Arguments arguments) const {
    if (arguments.size() > 1) {
        return arguments.size() == 2 ? std::vector<std::string>{"preview"}
                                     : std::vector<std::string>{};
    }
    std::vector<std::string> words{"tower", "back"};
    for (const LevelCatalog::Realm& realm : m_ports.levels.realms()) {
        words.insert(words.end(), realm.levels.begin(), realm.levels.end());
    }
    const std::vector<std::string> legends = m_ports.legendsMapNames();
    words.insert(words.end(), legends.begin(), legends.end());
    return words;
}

void OfflineConsoleCommands::partyCount(DevConsole& console, Arguments arguments) {
    const auto describe = [&] {
        return m_ports.partyCount.has_value()
                   ? std::format("The level is set up for a party of {}", *m_ports.partyCount)
                   : std::string("The level is set up for the players present");
    };
    if (arguments.empty()) {
        console.print(describe());
        return;
    }
    const auto parsed = parsePartyCount(arguments);
    if (!parsed.has_value()) {
        console.print(parsed.error());
        return;
    }
    m_ports.partyCount = parsed->mode == PartyCountRequest::Mode::Fixed
                             ? std::optional{parsed->players}
                             : std::nullopt;
    if (playScene()->isOpen()) {
        playScene()->setPartyCountOverride(m_ports.partyCount);
    }
    console.print(describe());
}

} // namespace gdl::game
