#pragma once

#include <vector>

#include "engine/math/Math.h"

namespace gdl {
/** Bounded drawing-only scene checkpoint. Moving nodes are always included (even
 * empty parents); still nodes appear only for a visibility/opacity override.
 * Omitted still nodes return to the loaded layout, not the preceding checkpoint.
 * Local transforms preserve the authored hierarchy. No collision or trigger state
 * is restored by this type. layout is a consistency fingerprint, not authentication. */
struct SceneGeometry {
    static constexpr usize kMaxStates = 2048;
    struct Object {
        u32 index = 0;
        u32 continuity = 1;
        Mat4 local{1};
        f32 alpha = 1;
        bool visible = true;
        bool valid() const;
    };
    u64 layout = 0;
    u32 objectCount = 0;
    f32 darken = 0;
    std::vector<Object> objects; // strictly increasing layout indices
    bool valid() const;
};
} // namespace gdl
