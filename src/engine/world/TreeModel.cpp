#include "engine/world/TreeModel.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

#include "engine/core/Log.h"

namespace gdl {

bool TreeModel::bind(const TreeInfo& tree, ModelSet& models, TextureSet& textures,
                     RenderDevice& device) {
    clear();
    bool first = true;
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        const TreeNodeInfo& info = tree.nodes[i];
        if (info.object.empty()) {
            continue;
        }
        const auto model = models.find(info.object);
        if (!model.has_value()) {
            log::warn("Tree model {}: object {} is missing", tree.name, info.object);
            clear();
            return false;
        }
        try {
            Node node;
            node.mesh = &models.mesh(*model);
            node.index = i;
            node.offset = tree.worldPosition(i);
            node.chrome = info.chrome();
            node.additive = info.additive();
            node.depthWrite = info.writesDepth();
            node.facing = CameraFrame::facingOf(info.objectFlags);
            for (const MeshPart& part : node.mesh->parts) {
                if (part.texture >= textures.size()) {
                    throw std::runtime_error("mesh refers to a texture outside the set");
                }
                node.textures.push_back(&textures.texture(device, part.texture));
                node.translucent.push_back(textures.entry(part.texture).translucent());
            }
            for (const MeshVertex& v : node.mesh->vertices) {
                const Vec3 position = v.position + node.offset;
                m_min = first ? position : glm::min(m_min, position);
                m_max = first ? position : glm::max(m_max, position);
                first = false;
            }
            m_nodes.push_back(std::move(node));
        } catch (const std::exception& e) {
            log::warn("Tree model {}: {}", tree.name, e.what());
            clear();
            return false;
        }
    }
    return !m_nodes.empty();
}

void TreeModel::draw(RenderDevice& device, const Mat4& clip, const Mat4& model,
                     const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                     const CameraFrame* camera, f32 alpha) const {
    if (alpha <= 0.0f) {
        return;
    }
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, false);
    drawParts(device, clip, model, lighting, nodeTransforms, camera, alpha, true);
}

void TreeModel::drawParts(RenderDevice& device, const Mat4& clip, const Mat4& model,
                          const WorldLighting& lighting, std::span<const Mat4> nodeTransforms,
                          const CameraFrame* camera, f32 alpha, bool translucent) const {
    const bool fading = alpha < 1.0f;
    for (const Node& node : m_nodes) {
        Mat4 placement = node.index < nodeTransforms.size()
                             ? model * nodeTransforms[node.index]
                             : glm::translate(model, node.offset);
        if (camera != nullptr && node.facing != 0) {
            placement = camera->face(placement, node.facing);
        }
        const Mat3 normalMatrix{placement};
        for (usize p = 0; p < node.mesh->parts.size(); ++p) {
            // A fading figure blends its solid parts too, so the whole of it thins together.
            const bool blended = node.translucent[p] || node.additive || fading;
            if (blended != translucent) {
                continue;
            }
            const MeshPart& part = node.mesh->parts[p];
            m_batch.clear();
            m_batch.begin(PrimitiveTopology::TriangleList);
            for (const u32 index : part.indices) {
                const MeshVertex& v = node.mesh->vertices[index];
                const Vec3 normal = glm::normalize(normalMatrix * v.normal);
                const Vec2 uv =
                    node.chrome ? Vec2{0.5f * (1.0f - normal.x), 0.5f * (1.0f - normal.y)} : v.uv;
                const Vec4 placed = placement * Vec4{v.position, 1.0f};
                Color color = lighting.shade(normal);
                if (fading) {
                    color.a = static_cast<u8>(static_cast<f32>(color.a) * alpha);
                }
                m_batch.vertex(Vec3{placed}, color, uv);
            }
            m_batch.end();
            DrawState state;
            state.cullBack = true;
            state.blend = node.additive ? BlendMode::Additive : BlendMode::Alpha;
            state.alphaTest = blended ? DrawState::kTranslucentAlphaTest : 0.0f;
            state.depthWrite = node.depthWrite && !fading;
            device.draw(m_batch, *node.textures[p], clip, state);
        }
    }
}

} // namespace gdl
