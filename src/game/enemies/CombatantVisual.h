#pragma once

#include <vector>

#include "engine/world/TreeModel.h"

namespace gdl::game {
struct CombatantAssets;

/** Read-only draw description. Poses include gaze, held limbs, independent heads
 * and camera-facing attachments. Gathering it never runs gameplay or loads assets. */
struct CombatantVisual {
    struct Node {
        Mat4 transform{1};
        u64 generation = 0;
        u32 sequence = 0;
        f32 frame = 0;
        f32 alpha = 1;
        bool flash = false;
    };
    const CombatantAssets* stock = nullptr;
    const TreeModel* model = nullptr;
    const TreeInfo* textureTree = nullptr;
    const Texture* skin = nullptr;
    u32 part = 0;
    Mat4 placement{1};
    u32 sequence = 0;
    f32 frame = 0;
    f32 textureClock = 0;
    f32 alpha = 1;
    Color tint = Color::white();
    bool unlit = false;
    bool flash = false;
    bool frozen = false;
    std::vector<Node> nodes;
};
} // namespace gdl::game
