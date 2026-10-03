#include "game/menu/CompassHud.h"

#include <cmath>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
bool CompassHud::bind(RenderDevice& device, ItemArchive& powerups) {
    clear();
    if (!powerups.models.find("COMPASS")) {
        log::warn("POWERUPS compass model is missing");
        return false;
    }
    TreeInfo tree;
    tree.name = "COMPASS";
    TreeNodeInfo node;
    node.name = tree.name;
    node.object = tree.name;
    tree.nodes.push_back(node);
    return m_model.bind(tree, powerups.models, powerups.textures, device);
}

void CompassHud::clear() {
    m_model.clear();
}

Mat4 CompassHud::placement(const WorldCamera& camera, f32 horizontalFov, f32 aspect) {
    const f32 halfWidth = kDepth * std::tan(horizontalFov / 2.0f);
    const f32 halfHeight = halfWidth / aspect;
    const Vec2 anchor = frameAnchor() / kReferenceFrame;
    const Vec3 position = camera.position + camera.forward() * kDepth +
                          camera.right() * ((2.0f * anchor.x - 1.0f) * halfWidth) +
                          camera.up() * ((1.0f - 2.0f * anchor.y) * halfHeight);
    return glm::scale(glm::translate(Mat4{1.0f}, position), Vec3{kScale});
}

void CompassHud::draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
                      f32 horizontalFov, f32 aspect, const WorldLighting& lighting,
                      bool visible) const {
    if (visible && bound()) {
        m_model.draw(device, clip, placement(camera, horizontalFov, aspect), lighting, {}, nullptr,
                     kOpacity);
    }
}
} // namespace gdl::game
