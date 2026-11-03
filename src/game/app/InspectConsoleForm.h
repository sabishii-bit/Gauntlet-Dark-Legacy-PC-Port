#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleReach.h"
#include "game/scenes/SceneSelection.h"

namespace gdl::game {

/** What get info reads of the game, each read as it runs. */
struct InspectConsoleAccess {
    /** What is selected in the scene the console reads; none with nothing selected or in play. */
    std::function<std::optional<SceneSelection>()> selected;
    /** What the console says of a thing selected or named. */
    std::function<std::vector<std::string>(const SceneSelection& selection)> describe;
};

/** get info: what the thing selected, or named by its token, is. It is answered on the asking
 * machine alone, in any session its console is open to. */
class InspectConsoleForm final : public ConsoleForm {
public:
    explicit InspectConsoleForm(InspectConsoleAccess access);

    bool claims(Arguments arguments) const override;
    ConsoleReach reach() const override { return ConsoleReach::Local; }
    void run(DevConsole& console, Arguments arguments) override;

private:
    InspectConsoleAccess m_access;
};

} // namespace gdl::game
