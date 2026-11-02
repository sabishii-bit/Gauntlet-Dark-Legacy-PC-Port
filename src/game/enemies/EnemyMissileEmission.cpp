#include "game/enemies/EnemyMissileEmission.h"

#include "game/audio/LevelSoundscape.h"

namespace gdl::game {
EnemyMissileEmission EnemyMissileEmission::resolve(const EnemyMissileHit& hit,
                                                   ItemArchive& weapons) {
    EnemyMissileEmission result;
    result.position = hit.position;
    if (hit.ricochet) {
        result.sound = "S_RICOCHET";
        result.level = 127;
        return result;
    }
    const auto tree = hit.effect();
    if (!tree.empty() && weapons.loaded() && weapons.trees.find(tree)) {
        result.archive = &weapons;
        result.tree = tree;
    }
    result.setting.unlit = true;
    result.setting.depthWrite = false;
    if (hit.burstRadius <= 0) {
        result.setting.tint.a = 96;
    }
    result.sound = hit.sound();
    result.level = hit.liquid ? LevelSoundscape::kSplashLevel : 224;
    return result;
}

} // namespace gdl::game
