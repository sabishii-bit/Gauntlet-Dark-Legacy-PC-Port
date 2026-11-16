#include "expansions/n64/hud/LegendsN64HudText.h"

#include <format>
#include <string>

#include "engine/core/Types.h"

namespace gdl::game {

std::string legendsN64HudTextKey(s32 native) {
    return std::format("legends.n64.hud.text.{}", native);
}

} // namespace gdl::game
