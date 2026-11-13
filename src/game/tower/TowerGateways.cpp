#include "game/tower/TowerGateways.h"

#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"

namespace gdl::game {

bool beatenGarm(const CharacterSave& save) {
    return save.progress().relics.hasShard(LevelRef::orderOf(TowerAccess::kBattlefieldWorld));
}

} // namespace gdl::game
