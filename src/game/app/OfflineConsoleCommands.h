#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"

#include "game/characters/CharacterSave.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleCommands.h"
#include "game/console/DevConsole.h"
#include "game/console/NoclipMotion.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

/**
 * The developer console's commands this machine answers by itself: offline play's god,
 * notarget, noclip, get, goto, spawn, suicide and unlock_tower, the level's switches, doors and
 * chests, the party size a level sets itself up for, and the listings. They reach the
 * application only through its ports, each read at the moment a command runs.
 */
class OfflineConsoleCommands {
public:
    using Arguments = ConsoleCommand::Arguments;
    using Cheats =
        std::array<PlayScene::ConsoleCheats, static_cast<usize>(PlayScene::kPlayerCount)>;
    using Flights = std::array<NoclipMotion, static_cast<usize>(PlayScene::kPlayerCount)>;

    /** What the commands read and change of the application. */
    struct Ports {
        std::function<PlayScene*()> playScene;  ///< the offline session's scene, open or not
        std::function<LevelWorld*()> playWorld; ///< the offline session's world
        /** The scene the console reads: the shared game's online, else the offline one while
         * it is open; null with nothing in play. */
        std::function<const PlayScene*()> consoleScene;
        std::function<const LevelWorld*()> consoleWorld; ///< the world of that scene
        /** The player the console speaks for; -1 with nobody in. */
        std::function<s32()> consolePlayer;
        std::function<RenderDevice&()> device;
        const LevelCatalog& levels;
        std::filesystem::path unpackedRoot;
        const GameConfig& config;
        SaveSlots& saves;               ///< opened when first needed, for the class unlocks
        Cheats& cheats;                 ///< each player's, carried into the scene every tick
        Flights& noclip;                ///< each player's noclip flight
        std::optional<s32>& partyCount; ///< party_count's across levels; none: the party present
        /** Takes the party to a level through the loading picture, standing it at a point
         * facing a way when given; the scene closes at once. */
        std::function<void(const LevelRef& level, std::optional<Vec3> position,
                           std::optional<f32> yaw)>
            travel;
        /** list maps' lines for the Legends maps installed, each platform's found afresh. */
        std::function<std::vector<std::string>()> legendsMapLines;
        /** The Legends maps' names goto completes to: the N64's, then the Dreamcast's. */
        std::function<std::vector<std::string>()> legendsMapNames;
    };

    explicit OfflineConsoleCommands(Ports ports);

    /** god: nothing harms the console's player, or everything does again. */
    void god(DevConsole& console);
    /** notarget: the enemies stop seeing the console's player, or see it again. */
    void noTarget(DevConsole& console);
    /** noclip: flies the console's player through everything, or sets it down again. */
    void noclip(DevConsole& console);
    /** get's gifts for the console's player; `get info` is the caller's to answer. */
    void get(DevConsole& console, Arguments arguments);
    /** goto a level, a point or the tower; the Legends maps and the way back from one are the
     * caller's. */
    void gotoPlace(DevConsole& console, const ConsoleCommands::Destination& destination);
    /** spawn: enemies, great ones, generators, pickups or objects before the console's player. */
    void spawn(DevConsole& console, Arguments arguments);
    /** suicide: fells everyone standing. */
    void suicide(DevConsole& console);
    /** unlock_tower in Dark Legacy's world: opens the tower to every save in, reopening it
     * where the console's player stands when it is the tower. */
    void unlockTower(DevConsole& console);
    /** activate: presses a switch of the level in play, named by token, id or name. */
    void activate(DevConsole& console, Arguments arguments);
    /** lock and unlock: a character by its code, or a door or chest of the level in play. */
    void lock(DevConsole& console, Arguments arguments, bool unlock);
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
    std::vector<ConsoleCommands::SwitchEntry> consoleSwitches() const;

private:
    /** A door or chest of the level in play the console can name. */
    struct ConsoleFixture {
        SceneSelection::Kind kind = SceneSelection::Kind::Gate;
        usize index = 0;
        ConsoleCommands::NamedEntry entry;
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
    LevelWorld* playWorld() const { return m_ports.playWorld(); }
    const PlayScene* consoleScene() const { return m_ports.consoleScene(); }
    const LevelWorld* consoleWorld() const { return m_ports.consoleWorld(); }
    s32 consolePlayer() const { return m_ports.consolePlayer(); }
    void toggleCheat(DevConsole& console, bool PlayScene::ConsoleCheats::*flag, std::string_view on,
                     std::string_view off);
    /** Lends the swarm `level`'s voices for the visitors brought in from it. */
    void consoleBorrowVoices(DevConsole& console, const LevelRef& level);
    /** Fills `level` for `map`, or for the level in play without one; false, having said
     * why, for a map unknown or not unpacked. */
    bool consoleLevel(DevConsole& console, const std::optional<std::string>& map,
                      ConsoleLevel& level);
    std::vector<ConsoleFixture> consoleFixtures() const;

    Ports m_ports;
};

} // namespace gdl::game
