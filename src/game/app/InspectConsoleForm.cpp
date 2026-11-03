#include "game/app/InspectConsoleForm.h"

#include <format>
#include <utility>

#include "engine/core/Assert.h"

#include "game/console/DevConsole.h"
#include "game/console/GiftRequest.h"

namespace gdl::game {

InspectConsoleForm::InspectConsoleForm(InspectConsoleAccess access) : m_access(std::move(access)) {}

bool InspectConsoleForm::claims(Arguments arguments) const {
    const auto gift = parseGift(arguments);
    return gift.has_value() && gift->kind == GiftRequest::Kind::Info;
}

void InspectConsoleForm::run(DevConsole& console, Arguments arguments) {
    const auto gift = parseGift(arguments);
    GDL_VERIFY(gift.has_value() && gift->kind == GiftRequest::Kind::Info,
               "get info's form claims its own words alone");
    const std::optional<SceneSelection> what =
        gift->name.empty() ? m_access.selected() : SceneSelection::parse(gift->name);
    if (!what.has_value()) {
        console.print(gift->name.empty()
                          ? "Nothing is selected: click something in the view while the "
                            "console is down, then get info $SELECTED"
                          : std::format("{} names nothing; $SELECTED or a token such as "
                                        "object#12 or enemy#3 does",
                                        gift->name));
        return;
    }
    for (const std::string& line : m_access.describe(*what)) {
        console.print(line);
    }
}

} // namespace gdl::game
