#include "game/app/OfflineConsoleCommands.h"

#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Assert.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/Chests.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LockedGates.h"
#include "game/level/TowerAccess.h"
#include "game/players/ItemPickup.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {

OfflineConsoleCommands::OfflineConsoleCommands(Ports ports) : m_ports(std::move(ports)) {}

void OfflineConsoleCommands::noclip(DevConsole& console) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& flying = m_ports.cheats[static_cast<usize>(player)].noclip;
    flying = !flying;
    m_ports.noclip[static_cast<usize>(player)].reset();
    console.print(flying ? std::format("Noclip on: {}", NoclipMotion::kHelp) : "Noclip off");
}

void OfflineConsoleCommands::toggleCheat(DevConsole& console, bool PlayScene::ConsoleCheats::*flag,
                                         std::string_view on, std::string_view off) {
    const s32 player = consolePlayer();
    if (player < 0) {
        console.print("Nobody is in.");
        return;
    }
    bool& granted = m_ports.cheats[static_cast<usize>(player)].*flag;
    granted = !granted;
    console.print(granted ? on : off);
}

void OfflineConsoleCommands::god(DevConsole& console) {
    toggleCheat(console, &PlayScene::ConsoleCheats::god, "God mode on", "God mode off");
}

void OfflineConsoleCommands::noTarget(DevConsole& console) {
    toggleCheat(console, &PlayScene::ConsoleCheats::noTarget, "The enemies no longer see you",
                "The enemies see you again");
}

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
    using Listing = ConsoleCommands::Listing;
    const auto say = [&](const std::vector<std::string>& lines) {
        for (const std::string& line : lines) {
            console.print(line);
        }
    };
    const Listing listing = ConsoleCommands::listingOf(arguments);
    const std::optional<std::string> map = ConsoleCommands::listedMap(arguments);
    switch (listing) {
    case Listing::Maps:
        say(ConsoleCommands::mapLines(m_ports.levels));
        // The Legends maps installed stand with them, each platform's on a line; nothing is
        // said of a platform without any.
        say(m_ports.legendsMapLines());
        return;
    case Listing::Inventory:
        if (const PlayerActor* actor =
                consoleScene() ? consoleScene()->actor(consolePlayer()) : nullptr) {
            say(ConsoleCommands::inventoryLines(actor->save()));
        } else {
            console.print("Nobody is in.");
        }
        return;
    case Listing::Unknown: console.print(ConsoleCommands::listUsage()); return;
    case Listing::Characters:
        if (m_ports.saves.opened() ||
            m_ports.saves.open(m_ports.config.saveDirectory(), m_ports.config.save.slots)) {
            say(ConsoleCommands::characterLines(m_ports.saves.classUnlocks()));
        } else {
            console.print("The saves cannot be opened, so what is unlocked is unknown.");
        }
        return;
    case Listing::Switches:
        if (!map.has_value() && consoleWorld() != nullptr) {
            console.print(std::format("In {}:", consoleWorld()->ref().name));
            say(ConsoleCommands::switchLines(consoleSwitches()));
            return;
        }
        break;
    case Listing::Items:
        if (!map.has_value()) {
            say(ConsoleCommands::itemLines());
        }
        break;
    case Listing::Enemies:
        if (!map.has_value()) {
            say(ConsoleCommands::enemyKindLines());
        }
        break;
    case Listing::Critters: say(ConsoleCommands::critterLines()); break;
    case Listing::Generators:
    case Listing::Objects: break;
    }
    ConsoleLevel level;
    if (!consoleLevel(console, map, level)) {
        return;
    }
    console.print(std::format("In {}:", level.name));
    switch (listing) {
    case Listing::Items: say(ConsoleCommands::pickupLines(*level.layout)); break;
    case Listing::Enemies:
        say(ConsoleCommands::rosterLines(level.roster));
        say(ConsoleCommands::generatorLines(*level.layout));
        say(ConsoleCommands::placedEnemyLines(*level.layout));
        break;
    case Listing::Critters: say(ConsoleCommands::placedCritterLines(*level.layout)); break;
    case Listing::Generators:
        say(ConsoleCommands::generatorLines(*level.layout));
        console.print("spawn generator <enemy> [tier] stands another");
        break;
    case Listing::Objects: say(ConsoleCommands::objectLines(*level.layout)); break;
    case Listing::Switches:
        say(ConsoleCommands::switchLines(ConsoleCommands::switchesOf(*level.layout)));
        break;
    case Listing::Maps:
    case Listing::Inventory:
    case Listing::Characters:
    case Listing::Unknown: break;
    }
}

std::vector<ConsoleCommands::SwitchEntry> OfflineConsoleCommands::consoleSwitches() const {
    std::vector<ConsoleCommands::SwitchEntry> switches;
    const LevelWorld* world = consoleWorld();
    if (world == nullptr) {
        return switches;
    }
    const WorldLayout& layout = world->layout();
    const LevelTriggers& triggers = world->triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& trigger = triggers.trigger(i);
        ConsoleCommands::SwitchEntry entry;
        entry.index = static_cast<s32>(i);
        entry.id = trigger.id;
        entry.name = ConsoleCommands::instanceName(layout, trigger.instance);
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
            fixtures.push_back({SceneSelection::Kind::Gate,
                                i,
                                {std::format("gate#{}", i),
                                 ConsoleCommands::instanceName(layout, gates.gate(i).instance)}});
        }
    }
    const Chests& chests = scene->chests();
    for (usize i = 0; i < chests.size(); ++i) {
        if (chests.chest(i).shown && !chests.chest(i).gone) {
            fixtures.push_back({SceneSelection::Kind::Chest,
                                i,
                                {std::format("chest#{}", i),
                                 ConsoleCommands::instanceName(layout, chests.chest(i).instance)}});
        }
    }
    return fixtures;
}

std::vector<std::string> OfflineConsoleCommands::activateWords(Arguments arguments) const {
    std::vector<std::string> words;
    if (arguments.size() > 1) {
        return words;
    }
    for (const ConsoleCommands::NamedEntry& entry :
         ConsoleCommands::switchEntries(consoleSwitches())) {
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
        console.print(ConsoleCommands::activateUsage());
        return;
    }
    if (consoleScene() == nullptr || playScene() == nullptr || !playScene()->isOpen()) {
        console.print("Nothing is in play.");
        return;
    }
    const auto switches = consoleSwitches();
    const auto entries = ConsoleCommands::switchEntries(switches);
    const std::vector<usize> found = ConsoleCommands::entriesNamed(entries, arguments[0]);
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
    const ConsoleCommands::SwitchEntry& entry = switches[found[0]];
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

void OfflineConsoleCommands::lock(DevConsole& console, Arguments arguments, bool unlock) {
    const std::string_view verb = unlock ? "unlock" : "lock";
    std::string error;
    const auto request = ConsoleCommands::parseLockRequest(arguments, error, verb);
    if (!request.has_value()) {
        console.print(error);
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
            for (s32 player = 0; player < 4; ++player) {
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
    std::vector<ConsoleCommands::NamedEntry> entries;
    entries.reserve(fixtures.size());
    for (const ConsoleFixture& fixture : fixtures) {
        entries.push_back(fixture.entry);
    }
    const std::vector<usize> found = ConsoleCommands::entriesNamed(entries, request->what);
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

void OfflineConsoleCommands::consoleBorrowVoices(DevConsole& console, const LevelRef& level) {
    WorldData data;
    const LevelInfo* info =
        data.load(m_ports.unpackedRoot / level.worldDataFile()) ? data.level(level.name) : nullptr;
    if (info == nullptr ||
        !playScene()->borrowOpponentSounds(*info, data.audio(info->audioIndex))) {
        console.print(std::format("({} lends no voices)", level.name));
    }
}

void OfflineConsoleCommands::spawn(DevConsole& console, Arguments arguments) {
    constexpr f32 kAhead = 6.0f; ///< units before the character
    constexpr f32 kApart = 3.0f; ///< between several
    constexpr f32 kProbeAbove = 3.0f;
    constexpr f32 kProbeBelow = 10.0f;
    const PlayerActor* actor = playScene()->actor(consolePlayer());
    if (actor == nullptr) {
        console.print("Nobody is in to spawn before.");
        return;
    }
    std::string error;
    const auto spawn = ConsoleCommands::parseSpawn(arguments, error);
    if (!spawn.has_value()) {
        console.print(error);
        return;
    }
    // The map named, when one is: where a visitor's voices, costume or object come from.
    std::optional<LevelRef> from;
    if (spawn->map.has_value()) {
        from = *spawn->map == "TOWER" ? std::optional{LevelRef::tower()}
                                      : m_ports.levels.byName(*spawn->map);
        if (!from.has_value()) {
            console.print(std::format("No map named {}; list maps names them", *spawn->map));
            return;
        }
    }
    const Vec3 facing = actor->facing();
    const f32 yawToPlayer = actor->yaw() + kPi;
    const auto spotAt = [&](s32 index) {
        Vec3 spot = actor->position() + facing * (kAhead + kApart * static_cast<f32>(index));
        if (const auto floor = playWorld()->collision().floorAt(
                spot + Vec3{0.0f, kProbeAbove, 0.0f}, kProbeAbove, kProbeBelow)) {
            spot.y = floor->y;
        }
        return spot;
    };
    PlayScene& scene = *playScene();
    switch (spawn->kind) {
    case ConsoleCommands::Spawn::Kind::Enemy: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        s32 stood = 0;
        const std::vector<EnemyView> views = scene.opponents().views(scene.participants());
        for (s32 i = 0; i < spawn->count; ++i) {
            if (scene.enemies().loadKind(spawn->enemyKind) &&
                scene.enemies()
                    .spawn(EnemySpawn{.kind = spawn->enemyKind,
                                      .tier = spawn->tier,
                                      .position = spotAt(i),
                                      .direction = -facing,
                                      .placed = true},
                           views)
                    .has_value()) {
                ++stood;
            }
        }
        console.print(
            stood > 0 ? std::format("Spawned {} {} at tier {}", stood, spawn->name, spawn->tier)
                      : std::format("No room for {} here, or its kind is missing", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Critter: {
        const char realm =
            from.has_value() && !from->name.empty()
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(from->name.front())))
                : '\0';
        std::optional<s32> id;
        switch (spawn->critter) {
        case CombatantKind::Golem:
            id = scene.critters().spawnGolem(spotAt(0), yawToPlayer, realm);
            break;
        case CombatantKind::General:
            id = scene.critters().spawnGeneral(spotAt(0), yawToPlayer, 0.0f, realm);
            break;
        case CombatantKind::Gargoyle:
            id = scene.critters().spawnGargoyle(spotAt(0), yawToPlayer, spawn->form, realm);
            break;
        case CombatantKind::Unknown:
        case CombatantKind::Boss: break;
        }
        const std::string& what = spawn->form.empty() ? spawn->name : spawn->form;
        console.print(id.has_value()
                          ? std::format("Spawned a {}", what)
                          : std::format("No room for a {} here, or its kind is missing", what));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Generator: {
        if (from.has_value()) {
            consoleBorrowVoices(console, *from);
        }
        s32 kind = spawn->enemyKind;
        ItemArchive* realmItems = nullptr;
        const ItemInfo* record = nullptr;
        ConsoleLevel source;
        if (spawn->brood) {
            // A mixed brood is its realm's: its bodies and record come from that realm's
            // level, the one in play when it is one of them.
            const s32 realm = from.has_value() ? from->realmId : playWorld()->ref().realmId;
            if (realm != TowerAccess::kTempleWorld && realm != TowerAccess::kUnderworldWorld) {
                console.print("A mixed brood is the temple's (E) or the underworld's (F); name "
                              "a map of either");
                break;
            }
            kind = realm == TowerAccess::kTempleWorld ? Generators::kTempleBrood
                                                      : Generators::kHellBrood;
            if (from.has_value()) {
                realmItems = scene.borrowItems(*from);
                if (realmItems == nullptr) {
                    console.print(std::format("{} lends no item archive", from->name));
                    break;
                }
                if (consoleLevel(console, spawn->map, source)) {
                    record = ConsoleCommands::generatorRecord(*source.layout, "SPECIAL");
                }
            }
        }
        const auto id =
            scene.generators().spawn(m_ports.device(), scene.enemies(), kind, spawn->tier,
                                     spotAt(0), yawToPlayer, realmItems, record);
        console.print(id.has_value()
                          ? std::format("Stood a tier {} {} generator", spawn->tier, spawn->name)
                          : std::format("No {} generator could stand here", spawn->name));
        break;
    }
    case ConsoleCommands::Spawn::Kind::Item:
        if (from.has_value()) {
            console.print("Pickups come from the level in play alone; name one without a map");
        } else if (playWorld()->placeItem(m_ports.device(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
        } else {
            console.print(
                std::format("No pickup record named {} here; list items names them", spawn->name));
        }
        break;
    case ConsoleCommands::Spawn::Kind::Object: {
        const LevelRef source = from.has_value() ? *from : playWorld()->ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, source.name, source.name));
        }
        break;
    }
    case ConsoleCommands::Spawn::Kind::Thing: {
        if (!from.has_value() && playWorld()->placeItem(m_ports.device(), spawn->name, spotAt(0))) {
            console.print(std::format("Dropped {}", spawn->name));
            break;
        }
        const LevelRef source = from.has_value() ? *from : playWorld()->ref();
        if (scene.spawnObject(source, spawn->name, spotAt(0), yawToPlayer)) {
            console.print(std::format("Stood {} of {}", spawn->name, source.name));
        } else if (from.has_value()) {
            console.print(std::format("Nothing named {} in {}; list objects {} names what there is",
                                      spawn->name, from->name, from->name));
        } else {
            console.print(std::format("Nothing named {} here; list items, objects, enemies or "
                                      "critters name what there is",
                                      spawn->name));
        }
        break;
    }
    }
}

void OfflineConsoleCommands::suicide(DevConsole& console) {
    if (playWorld()->isTower()) {
        console.print("Nobody falls in the tower.");
        return;
    }
    s32 fallen = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (playScene()->kill(player)) {
            ++fallen;
        }
    }
    console.print(fallen > 0 ? std::format("{} fallen", fallen) : "Nobody was standing.");
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
    std::string error;
    const auto parsed = ConsoleCommands::parsePartyCount(arguments, error);
    if (!parsed.has_value()) {
        console.print(error);
        return;
    }
    m_ports.partyCount = *parsed;
    if (playScene()->isOpen()) {
        playScene()->setPartyCountOverride(m_ports.partyCount);
    }
    console.print(describe());
}

void OfflineConsoleCommands::gotoPlace(DevConsole& console,
                                       const ConsoleCommands::Destination& destination) {
    if (playScene()->leaving()) {
        console.print("The party is already on its way.");
        return;
    }
    if (!destination.level.has_value()) {
        const Vec3& at = *destination.position;
        if (playScene()->relocate(at)) {
            console.print(std::format("Moved to {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z));
        } else {
            console.print("Nobody is standing to move.");
        }
        return;
    }
    const std::optional<LevelRef> level = *destination.level == "TOWER"
                                              ? std::optional{LevelRef::tower()}
                                              : m_ports.levels.byName(*destination.level);
    if (!level.has_value()) {
        console.print(std::format("No map named {}; list maps names them", *destination.level));
        return;
    }
    console.print(std::format("Going to {} ({})", level->name, level->title));
    m_ports.travel(*level, destination.position, std::nullopt);
}

void OfflineConsoleCommands::unlockTower(DevConsole& console) {
    s32 opened = 0;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (CharacterSave* save = playScene()->liveSave(player)) {
            ConsoleCommands::unlockTower(*save);
            ++opened;
        }
    }
    if (opened == 0) {
        console.print("Nobody is in to open it for.");
        return;
    }
    console.print(
        std::format("The tower stands open to {} character{}.", opened, opened == 1 ? "" : "s"));
    if (playWorld()->isTower()) {
        // The portals read the party's progress as the tower opens: open it again, where
        // the party stands.
        const PlayerActor* actor = playScene()->actor(consolePlayer());
        m_ports.travel(LevelRef::tower(),
                       actor != nullptr ? std::optional{actor->position()} : std::nullopt,
                       actor != nullptr ? std::optional{actor->yaw()} : std::nullopt);
    }
}

void OfflineConsoleCommands::get(DevConsole& console, Arguments arguments) {
    std::string error;
    const auto gift = ConsoleCommands::parseGet(arguments, error);
    if (!gift.has_value()) {
        console.print(error);
        return;
    }
    GDL_VERIFY(gift->kind != ConsoleCommands::Gift::Kind::Info,
               "get info is answered before the gifts");
    const s32 player = consolePlayer();
    CharacterSave* save = playScene()->liveSave(player);
    if (save == nullptr) {
        console.print("Nobody is in to give it to.");
        return;
    }
    switch (gift->kind) {
    case ConsoleCommands::Gift::Kind::Experience:
        playScene()->awardExperience(player, gift->amount, false);
        playScene()->restat(player);
        console.print(std::format("{} experience: level {}", gift->amount,
                                  experienceLevel(save->experience())));
        break;
    case ConsoleCommands::Gift::Kind::Level:
        save->progress().experience = levelExperience(gift->amount);
        playScene()->restat(player);
        console.print(std::format("Level {}", gift->amount));
        break;
    case ConsoleCommands::Gift::Kind::Item:
        // At one times the strength: the seconds asked for are the seconds given.
        console.print(ConsoleCommands::report(*gift, takeItem(*save, gift->offer, 1.0f, true)));
        break;
    }
}

} // namespace gdl::game
