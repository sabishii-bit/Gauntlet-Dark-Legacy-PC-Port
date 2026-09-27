#include "game/world/BlobShadow.h"

#include <string>

namespace gdl::game {
namespace {
constexpr f32 kFlatEnough = 0.0001f; ///< a normal this short, or across the x axis, lies flat
} // namespace

bool BlobShadow::bind(RenderDevice& device, ItemArchive& archive, std::string_view object) {
    clear();
    if (!archive.models.find(object).has_value()) {
        return false;
    }
    TreeNodeInfo node;
    node.name = std::string{object};
    node.object = std::string{object};
    node.objectFlags = TreeNodeInfo::kNoDepthWriteFlag;
    m_tree.name = std::string{object};
    m_tree.nodes.push_back(node);
    if (!m_model.bind(m_tree, archive.models, archive.textures, device)) {
        clear();
        return false;
    }
    // The console lays the quad face down; seen from above it must still show.
    m_model.setDoubleSided(true);
    return true;
}

void BlobShadow::clear() {
    m_model.clear();
    m_tree = {};
}

Mat4 BlobShadow::placement(const Vec3& ground, const Vec3& normal, f32 size) {
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (glm::length(normal) > kFlatEnough) {
        up = glm::normalize(normal);
    }
    // The floor's frame: its up the normal, its z kept square to the world's x axis.
    Vec3 back = glm::cross(Vec3{1.0f, 0.0f, 0.0f}, up);
    if (glm::length(back) < kFlatEnough) {
        up = Vec3{0.0f, 1.0f, 0.0f};
        back = Vec3{0.0f, 0.0f, 1.0f};
    }
    back = glm::normalize(back);
    const Vec3 across = glm::cross(up, back);
    Mat4 frame{1.0f};
    frame[0] = Vec4{across * size, 0.0f};
    frame[1] = Vec4{up * size, 0.0f};
    frame[2] = Vec4{back * size, 0.0f};
    frame[3] = Vec4{ground + up * kLift, 1.0f};
    return frame;
}

Mat4 BlobShadow::pulledToward(const Mat4& placement, const Vec3& eye) {
    const f32 distance = glm::distance(Vec3{placement[3]}, eye);
    if (distance <= kPull) {
        return placement;
    }
    // Every point slides toward the eye by the same share, keeping to its line of sight.
    const f32 share = 1.0f - (kPull / distance);
    return glm::translate(Mat4{1.0f}, eye) * glm::scale(Mat4{1.0f}, Vec3{share}) *
           glm::translate(Mat4{1.0f}, -eye) * placement;
}

void BlobShadow::draw(RenderDevice& device, const Mat4& clip, const Vec3& eye, const Vec3& ground,
                      const Vec3& normal, const WorldLighting& lighting, f32 alpha,
                      f32 size) const {
    if (m_model.bound()) {
        m_model.draw(device, clip, pulledToward(placement(ground, normal, size), eye), lighting, {},
                     nullptr, alpha);
    }
}
} // namespace gdl::game
