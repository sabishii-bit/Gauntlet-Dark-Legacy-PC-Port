#pragma once

#include <span>
#include <string>
#include <vector>

#include "formats/AnimationTree.h"
#include "formats/WorldFile.h"

namespace gdl::formats {

/** Dreamcast's native animation archive uses the shared tree/keyframe schema,
 * but version zero has no particle-template list. No file rewriting is needed. */
AnimationFile parseDreamcastAnimation(std::span<const u8> bytes);

/** Native scene animation and hierarchy, separate from gameplay fixture logic.
 * All object identities index the original world table, including meshless nodes. */
struct LegendsSceneMotion {
    AnimationFile archive;
    std::vector<WorldObjectRecord> objects;
    std::vector<s32> parents;
    std::vector<WorldAnimationRecord> worldAnimations;
    std::vector<ParticleTemplateRecord> worldParticles;
    std::vector<std::string> warnings;

    static LegendsSceneMotion parseDreamcast(std::span<const u8> worlds,
                                             std::span<const u8> animations = {});
};

} // namespace gdl::formats
