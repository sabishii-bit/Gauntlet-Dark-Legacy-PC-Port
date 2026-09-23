#include "game/enemies/Gargoyle.h"

#include <string>

#include "engine/core/Strings.h"
namespace gdl::game {
CombatantDefinition Gargoyle::definition(std::string_view form) {
    CombatantDefinition out;
    out.name = form.empty() ? "GAR_EAGL" : normalizeAssetName(form);
    out.kind = CombatantKind::Gargoyle;
    const auto underscore = out.name.find('_');
    out.dropForm = underscore == std::string::npos ? out.name : out.name.substr(underscore + 1);
    return out;
}
} // namespace gdl::game
