#pragma once
#include <optional>
#include <span>
#include <string_view>

#include "engine/core/Types.h"

#include "game/console/ConsoleReach.h"

namespace gdl::game {

/** Who is at the console: offline play, the netplay menus, or a seat of an online game. */
enum class ConsoleSeat : u8 { Offline, NetplayMenus, Host, Guest };
/** Whether the session lets this machine's console run commands at all. */
enum class ConsoleAccess : u8 { Open, Closed };
/** Whether the scene the console speaks to is stepping, or held behind another view. */
enum class SceneHold : u8 { Live, Parked };

/** What the application is doing, as the console's policy judges it; set before each update. */
struct ConsoleSession {
    ConsoleSeat seat = ConsoleSeat::Offline;
    ConsoleAccess access = ConsoleAccess::Open;
    SceneHold hold = SceneHold::Live;
    /** While not empty, the only names admitted besides offline-only ones. */
    std::span<const std::string_view> modal;
};

/** What the application runs, as the plain facts a console session is made from. */
struct ConsoleSituation {
    bool online = false;  ///< an online game is open
    bool host = false;    ///< this machine hosts that game
    bool allowed = false; ///< that game lets this machine's console run commands
    bool netplay = false; ///< the netplay menus are open
    std::span<const std::string_view> preview; ///< what an open Legends map's preview admits
};

/** Which commands and forms a console session runs, judged by where each says it runs. */
class ConsolePolicy {
public:
    static constexpr std::string_view kRefused = "This command is not available in this session.";

    /** The session `situation` makes: an online game's seat outranks the netplay menus it was
     * entered from, and away from an online game the console may run commands. */
    static ConsoleSession session(const ConsoleSituation& situation);

    /** Why command `name` at `reach` is refused in `session`, or nothing when it runs.
     * `formRefusal` is what a form says when refused for running only offline. */
    static std::optional<std::string_view> refusal(const ConsoleSession& session,
                                                   ConsoleReach reach, std::string_view name,
                                                   std::string_view formRefusal = {});
};

} // namespace gdl::game
