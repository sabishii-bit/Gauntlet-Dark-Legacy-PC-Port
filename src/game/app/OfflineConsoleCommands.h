#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/DevConsole.h"
#include "game/console/FixtureRequest.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

/**
 * The developer console's commands this machine answers by itself: the level's switches, doors
 * and chests, the party size a level sets itself up for, the listings, and the words goto
 * completes to. They reach the application only through its ports, each read at the moment a
 * command runs.
 */
class OfflineConsoleCommands {
public:
    using Arguments = ConsoleCommand::Arguments;

    /** What the commands read and change of the application. */
    struct Ports {
        std::function<PlayScene*()> playScene; ///< the offline session's scene, open or not
        /** The scene the console reads: the shared game's online, else the offline one while
         * it is open; null with nothing in play. */
        std::function<const PlayScene*()> consoleScene;
        std::function<const LevelWorld*()> consoleWorld; ///< the world of that scene
        /** The player the console speaks for; -1 with nobody in. */
        std::function<s32()> consolePlayer;
        const LevelCatalog& levels;
        const std::filesystem::path& unpackedRoot;
        const GameConfig& config;
        SaveSlots& saves;               ///< opened when first needed, for the class unlocks
        std::optional<s32>& partyCount; ///< party_count's across levels; none: the party present
        /** list maps' lines for the Legends maps installed, each platform's found afresh. */
        std::function<std::vector<std::string>()> legendsMapLines;
        /** The Legends maps' names goto completes to: the N64's, then the Dreamcast's. */
        std::function<std::vector<std::string>()> legendsMapNames;
    };

    explicit OfflineConsoleCommands(Ports ports);

    /** activate: presses a switch of the level in play, named by token, id or name. */
    void activate(DevConsole& console, Arguments arguments);
    /** lock: a door of the level in play shut or a chest wanting a key again, or a character
     * hidden again by its code. */
    void lock(DevConsole& console, Arguments arguments);
    /** unlock: a door or chest of the level in play opened without a key, or a character
     * offered by its code. */
    void unlock(DevConsole& console, Arguments arguments);
    /** party_count: the party size the level sets itself up for, or off. */
    void partyCount(DevConsole& console, Arguments arguments);
    /** list: the maps, items, inventory, enemies, critters, generators, objects, switches or
     * characters there are. */
    void list(DevConsole& console, Arguments arguments);
    std::vector<std::string> activateWords(Arguments arguments) const;
    std::vector<std::string> lockWords(Arguments arguments) const;
    /** The words `goto` takes, for completion: the levels, the tower, the Legends maps
     * installed and the way back from one. */
    std::vector<std::string> gotoWords(Arguments arguments) const;
    /** The switches of the world in play, with their state. */
    std::vector<SwitchEntry> consoleSwitches() const;

private:
    /** A door or chest of the level in play the console can name. */
    struct ConsoleFixture {
        SceneSelection::Kind kind = SceneSelection::Kind::Gate;
        usize index = 0;
        NamedEntry entry;
    };
    /** What `list` reads of a level: the one in play's, or a named map's loaded for it. */
    struct ConsoleLevel {
        std::string name;
        const WorldLayout* layout = nullptr;
        std::span<const LevelEnemy> roster;
        WorldLayout loaded; ///< a named map's, when it is not the one in play
        WorldData data;
    };

    PlayScene* playScene() const { return m_ports.playScene(); }
    const PlayScene* consoleScene() const { return m_ports.consoleScene(); }
    const LevelWorld* consoleWorld() const { return m_ports.consoleWorld(); }
    s32 consolePlayer() const { return m_ports.consolePlayer(); }
    /** lock's or unlock's body, as `verb` says. */
    void changeLock(DevConsole& console, Arguments arguments, LockVerb verb);
    /** Fills `level` for `map`, or for the level in play without one; false, having said
     * why, for a map unknown or not unpacked. */
    bool consoleLevel(DevConsole& console, const std::optional<std::string>& map,
                      ConsoleLevel& level);
    std::vector<ConsoleFixture> consoleFixtures() const;

    Ports m_ports;
};

} // namespace gdl::game
