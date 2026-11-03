#pragma once
#include <functional>
#include <vector>

#include "game/console/ConsoleHost.h"
#include "game/console/FixtureRequest.h"

namespace gdl::game {

class LevelWorld;
class PlayScene;

/** What the fixture commands read and change of the game, each read as a command runs. */
struct FixtureConsoleAccess {
    /** Offline play's scene while it is open; null with nothing in play. */
    std::function<PlayScene*()> scene;
    /** The world the console reads; null with nothing in play. */
    std::function<const LevelWorld*()> world;
};

/** The switches of the level in play, with their state as it stands. */
std::vector<SwitchEntry> switchesInPlay(const LevelWorld& world);

/**
 * Registers the commands over the level's own fixtures, which run in offline play alone:
 * activate presses a switch as if stood on, unlock opens a door or chest without a key and
 * lock shuts one again. A character's code after lock or unlock is the profile's form, never
 * these commands'.
 */
void registerFixtureConsoleCommands(ConsoleHost& host, const FixtureConsoleAccess& access);

} // namespace gdl::game
