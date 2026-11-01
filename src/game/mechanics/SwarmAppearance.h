#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** How the living swarm is shown this frame. The defaults change nothing. */
struct SwarmLook {
    Color tint = Color::white(); ///< multiplies each living body's colour
    bool marked = false;         ///< whether a highlight rims each living body
    Color rim = Color::white();  ///< the highlight's rim, when marked
    Color wash = Color::white(); ///< and the wash over its faces
    f32 rimWidth = 0;            ///< the rim's width, a share of the view's height
};

/** Presentation a mechanic lays over the swarm; it never reads back into simulation. */
class SwarmAppearance {
public:
    virtual ~SwarmAppearance() = default;
    /** The look at simulation `frame`, a beat the presentation may keep time by. */
    virtual SwarmLook look(u32 frame) const = 0;
    /** How much larger than its kind a body of `affinity` is drawn. */
    virtual f32 bodyScale(s32 affinity) const = 0;
};

} // namespace gdl::game
