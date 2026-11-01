#pragma once

#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** Console inspection of native scenery without inventing GameCube layout records. */
struct ImportedWorldObject {
    s32 id = -1;
    std::string name;
    Vec3 position{0};
    u32 flags = 0;
    u32 drawFlags = 0;
    usize triangles = 0;
    bool visible = true;
};

struct ImportedObjectHit {
    ImportedWorldObject object;
    f32 distance = 0;
};

} // namespace gdl::game
