#pragma once

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/TextureAnimator.h"

namespace gdl::game {

/** Cropped hourglass sand, with a free-running falling-sand sprite. */
class ChallengeHud {
public:
    struct Sand {
        Rect upper;
        Rect upperUv;
        Rect lower;
        Rect lowerUv;
    };
    bool bind(RenderDevice& device, ItemArchive& archive);
    void clear();
    void step(f32 seconds);
    void draw(Canvas& canvas, f32 remaining, f32 duration, bool running) const;
    static Sand sand(f32 remaining, f32 duration);

private:
    const Texture* m_frame = nullptr;
    const Texture* m_sand = nullptr;
    TextureAnimator m_falling;
    f32 m_frames = 0;
};

} // namespace gdl::game
