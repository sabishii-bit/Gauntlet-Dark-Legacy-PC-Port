#include "engine/ui/ModelSprite.h"

#include <algorithm>
#include <cstddef>
#include <exception>

#include "engine/core/Log.h"

namespace gdl {

namespace {

constexpr float kAmbient = 0.55f;
constexpr float kDiffuse = 0.75f;
const Vec3 kLightDirection = glm::normalize(Vec3{0.3f, 0.6f, 1.0f});

} // namespace

bool ModelSprite::bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures,
                       RenderDevice& device) {
    m_nodes.clear();
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const TreeNodeInfo& info = tree.nodes[i];
        if (info.object.empty()) {
            continue;
        }
        const auto model = models.find(info.object);
        if (!model.has_value()) {
            log::warn("Model sprite {}: object {} is missing", tree.name, info.object);
            m_nodes.clear();
            return false;
        }
        try {
            Node node;
            node.mesh = &models.mesh(*model);
            node.offset = tree.worldPosition(i);
            node.chrome = info.chrome();
            for (const MeshPart& part : node.mesh->parts) {
                if (part.texture >= textures.size()) {
                    throw std::runtime_error("mesh refers to a texture outside the set");
                }
                node.textures.push_back(&textures.texture(device, part.texture));
            }
            m_nodes.push_back(std::move(node));
        } catch (const std::exception& e) {
            log::warn("Model sprite {}: {}", tree.name, e.what());
            m_nodes.clear();
            return false;
        }
    }
    return !m_nodes.empty();
}

void ModelSprite::draw(Canvas& canvas, Vec2 position, float scale, float pitch,
                       const Mat4& orientation) const {
    const Mat4 rotation = glm::rotate(Mat4{1.0f}, pitch, Vec3{1.0f, 0.0f, 0.0f}) * orientation;
    const Mat3 normalRotation{rotation};
    // Virtual screen space has y down; the model's y is up. Depth grows towards the viewer.
    Mat4 placement = glm::translate(Mat4{1.0f}, Vec3{position.x, position.y, kDepth});
    placement = glm::scale(placement, Vec3{scale, -scale, kDepthRange});
    const Mat4 local = placement * rotation;

    for (const Node& node : m_nodes) {
        for (std::size_t p = 0; p < node.mesh->parts.size(); ++p) {
            const MeshPart& part = node.mesh->parts[p];
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            for (const unsigned int index : part.indices) {
                const MeshVertex& v = node.mesh->vertices[index];
                const Vec3 normal = glm::normalize(normalRotation * v.normal);
                const float light =
                    std::clamp(kAmbient + kDiffuse * glm::dot(normal, kLightDirection), 0.0f, 1.0f);
                // Chrome samples the sheet by the view-space normal, mirrored as the original does.
                const Vec2 uv =
                    node.chrome ? Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)} : v.uv;
                m_batch.vertex(v.position + node.offset, Color::fromFloats(light, light, light),
                               uv);
            }
            m_batch.end();
            canvas.submit(m_batch, *node.textures[p], local);
        }
    }
}

} // namespace gdl
