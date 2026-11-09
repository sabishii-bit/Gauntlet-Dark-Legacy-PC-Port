#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

std::unique_ptr<ItemArchive> LegendsLevel::loadEnemyAssets(s32 kind) const {
    if (!hasEnemyAssets()) {
        return {};
    }
    return platform() == LegendsPlatform::Dreamcast ? loadDreamcastEnemyAssets(kind)
                                                    : loadN64EnemyAssets(kind);
}

bool LegendsLevel::playEnemyFeedback(const EnemyFeedback& feedback, f32 health) {
    return platform() == LegendsPlatform::Dreamcast ? playDreamcastEnemyFeedback(feedback)
                                                    : playN64EnemyFeedback(feedback, health);
}

bool LegendsLevel::playEnemyCue(const EnemyCue& cue) {
    return platform() == LegendsPlatform::Dreamcast ? playDreamcastEnemyCue(cue)
                                                    : playN64EnemyCue(cue);
}

bool LegendsLevel::playEnemyMelee(s32 kind, s32 tier) {
    return platform() == LegendsPlatform::Dreamcast ? playDreamcastEnemyMelee(kind, tier)
                                                    : playN64EnemyMelee(kind, tier);
}

bool LegendsLevel::playGeneratorFeedback(const GeneratorEvent& event) {
    return platform() == LegendsPlatform::Dreamcast ? playDreamcastGeneratorFeedback(event)
                                                    : playN64GeneratorFeedback(event);
}

} // namespace gdl::game
