#pragma once

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {
/** The POWERUPS compass, fixed to the world axes at a screen-relative position. */
class CompassHud {
public:
    static constexpr Vec2 kFramePosition{64.0f, 128.0f};
    static constexpr Vec2 kReferenceFrame{640.0f, 448.0f};
    static constexpr f32 kStatusHeight = 64.0f;
    /** Bottom-up MB window coordinates projected into the top-down frame. */
    static constexpr Vec2 frameAnchor() {
        constexpr f32 kWindowAspect = 0.75f;
        const f32 centre = (kReferenceFrame.y + kStatusHeight) / 2.0f;
        const f32 viewCentre = (kReferenceFrame.y - kStatusHeight) / 2.0f;
        const f32 verticalScale = kWindowAspect * kReferenceFrame.x / kReferenceFrame.y;
        return {kFramePosition.x, viewCentre - (kFramePosition.y - centre) * verticalScale};
    }
    static constexpr f32 kDepth = 10.0f;
    static constexpr f32 kScale = 1.5f;
    static constexpr f32 kOpacity = 127.0f / 255.0f;

    /** Borrows the POWERUPS model and textures; clear before releasing that archive. */
    bool bind(RenderDevice& device, ItemArchive& powerups);
    void clear();
    bool bound() const { return m_model.bound(); }

    /** Unprojects the screen anchor without rotating the model's world-axis basis. */
    static Mat4 placement(const WorldCamera& camera, f32 horizontalFov, f32 aspect);
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera, f32 horizontalFov,
              f32 aspect, const WorldLighting& lighting, bool visible = true) const;

private:
    TreeModel m_model;
};
} // namespace gdl::game
