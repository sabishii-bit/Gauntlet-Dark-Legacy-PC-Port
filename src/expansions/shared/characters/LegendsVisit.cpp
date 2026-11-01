#include "expansions/shared/characters/LegendsVisit.h"

namespace gdl::game {

bool retainLegendsProgress(CharacterSave& restored, const CharacterSave& current,
                           LegendsPlatform platform) {
    if (restored.name != current.name || restored.character != current.character ||
        restored.color != current.color || current.character < 0 ||
        current.character >= kClassCount || static_cast<usize>(platform) >= kLegendsPlatformCount) {
        return false;
    }
    const auto& progress = current.progress().legendsProgress(platform);
    if (!progress.valid()) {
        return false;
    }
    restored.progress().legendsProgress(platform) = progress;
    return true;
}

bool commitLegendsVisit(CharacterSave& live, const CharacterSave& before,
                        const CharacterSave& after) {
    if (live != before || after.name != before.name || after.character != before.character ||
        after.color != before.color || after.character < 0 || after.character >= kClassCount) {
        return false;
    }
    for (const auto& progress : after.classes) {
        for (const auto& campaign : progress.legends) {
            if (!campaign.valid()) {
                return false;
            }
        }
    }
    live = after;
    return true;
}

} // namespace gdl::game
