#include "game/tower/TowerGateways.h"

#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"

namespace gdl::game {

std::vector<GatewayChoice> gatewayChoices(std::span<const GatewayWorld> worlds,
                                          GatewayDestination here) {
    std::vector<GatewayChoice> choices;
    choices.reserve(worlds.size() + 1);
    const GatewayDestination home;
    choices.push_back({home, std::string(kGatewayHomeText), here != home});
    for (const GatewayWorld& world : worlds) {
        const GatewayDestination destination{world.id};
        choices.push_back({destination, world.label, world.usable && here != destination});
    }
    return choices;
}

bool beatenGarm(const CharacterSave& save) {
    return save.progress().relics.hasShard(LevelRef::orderOf(TowerAccess::kBattlefieldWorld));
}

} // namespace gdl::game
