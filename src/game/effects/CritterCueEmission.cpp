#include "game/effects/CritterCueEmission.h"

namespace gdl::game {
CritterCueEmission CritterCueEmission::resolve(const CombatCue& cue, ItemArchive* archive,
                                               ItemArchive& weapons) {
    CritterCueEmission result;
    result.position = cue.position;
    if (cue.tree.empty()) {
        return result;
    }
    if (archive == nullptr || !archive->trees.find(cue.tree)) {
        archive = weapons.loaded() && weapons.trees.find(cue.tree) ? &weapons : nullptr;
    }
    result.archive = archive;
    result.tree = cue.tree;
    result.setting.scale = cue.scale;
    result.setting.yaw = cue.yaw;
    result.setting.seconds = cue.life;
    result.setting.loop = cue.loop;
    return result;
}
} // namespace gdl::game
