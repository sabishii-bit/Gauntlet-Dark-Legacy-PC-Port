#pragma once

#include "game/critters/CombatEvents.h"
#include "game/effects/EffectTrees.h"

namespace gdl::game {
/** Resolved native move artwork. The archive is borrowed from a preloaded level. */
struct CritterCueEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;

    static CritterCueEmission resolve(const CombatCue& /*cue*/, ItemArchive* /*archive*/,
                                      ItemArchive& weapons);
};
} // namespace gdl::game
