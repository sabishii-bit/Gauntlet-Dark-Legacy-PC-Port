#pragma once
#include <string>
#include <string_view>

#include "game/console/ConsoleActionSink.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleWords.h"
#include "game/level/LevelCatalog.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SharedConsoleVerbs.h"

namespace gdl::game {

/**
 * A shared verb as the console registers it: help and the usage say what the verb and the forms
 * listed on it say, and a line the verb's check admits goes to the sink as the verb's name and
 * the words as typed, for the game in play to run. Words the check refuses, and words a parked
 * scene cannot take, go nowhere.
 */
class SharedConsoleCommand final : public ConsoleCommand {
public:
    /** `registry` is the console's, whose forms on the verb's name help and the usage read. */
    SharedConsoleCommand(const SharedConsoleVerb& verb, ConsoleActionSink& sink,
                         const LevelCatalog& levels, const ConsoleRegistry& registry);

    std::string_view name() const override { return m_verb.name(); }
    ConsoleReach reach() const override { return ConsoleReach::Shared; }
    /** The verb's forms and those of the forms listed on it, joined by ", " and a last " or ",
     * then the verb's tail. */
    std::string summary() const override;
    ConsoleWords words(Arguments arguments) const override;
    void run(DevConsole& console, Arguments arguments) override;
    /** The verb's usage rows and those of the forms listed on it, under the last such form's
     * note, else the verb's. */
    std::string usage() const;

private:
    const SharedConsoleVerb& m_verb;
    ConsoleActionSink& m_sink;
    const LevelCatalog& m_levels;
    const ConsoleRegistry& m_registry;
};

/** Registers every shared verb with `host` in the wire's order, their lines going to `sink`;
 * `registry` is the console's, whose forms help and the usage read. */
void registerSharedConsoleCommands(ConsoleHost& host, const ConsoleRegistry& registry,
                                   ConsoleActionSink& sink, const LevelCatalog& levels);

/** The game a console session's lines run in: offline play for the offline seat, else the
 * shared game. The netplay menus run no line: the console is held closed there. */
PlayScene::ConsolePlay consolePlayOf(const ConsoleSession& session);

} // namespace gdl::game
