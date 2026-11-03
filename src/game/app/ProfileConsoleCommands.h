#pragma once
#include <functional>

#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"

namespace gdl::game {

class PlayScene;
class SaveSlots;
struct GameConfig;

/** What the profile's console words read and change of the game, each read as a word runs. */
struct ProfileConsoleAccess {
    SaveSlots& saves; ///< opened when first needed, for the class unlocks
    const GameConfig& config;
    /** Offline play's scene while it is open; null with nothing in play. */
    std::function<PlayScene*()> scene;
};

/**
 * Registers the profile's console words, which run in offline play alone: the character forms
 * of unlock and lock, which offer a character on the select screen by its code or hide it
 * again, and list's characters.
 */
void registerProfileConsoleCommands(ConsoleHost& host, ConsoleListings& listings,
                                    const ProfileConsoleAccess& access);

} // namespace gdl::game
