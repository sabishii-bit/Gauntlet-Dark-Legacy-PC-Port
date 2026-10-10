#pragma once

#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"

namespace gdl::game {
/** Borrowed end-of-tick draw recipe. Observing it does not advance animation,
 * release a projectile, or change the owner's inventory. */
struct CompanionVisual {
    const TreeInfo* tree = nullptr;
    const TreeModel* model = nullptr;
    const TextureAnimator* textures = nullptr;
    u32 form = 0;
    Mat4 placement{1};
    u32 sequence = 0;
    u64 generation = 0;
    f32 frame = 0;
    f32 textureClock = 0;
    f32 alpha = 1;
};
} // namespace gdl::game
