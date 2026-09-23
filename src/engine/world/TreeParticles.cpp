#include "engine/world/TreeParticles.h"

#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl {
void TreeParticles::bind(const TreeInfo& tree, ItemArchive& archive, RenderDevice& device,
                         const Mat4& root, std::span<const Mat4> pose) {
    m_field.clear();
    m_nodes.clear();
    const auto& templates = archive.trees.particleTemplates();
    for (usize i = 0; i < tree.nodes.size(); ++i) {
        const auto& node = tree.nodes[i];
        if (node.particle < 0 || static_cast<usize>(node.particle) >= templates.size()) {
            continue;
        }
        const auto& source = templates[static_cast<usize>(node.particle)];
        auto descriptor = ParticleDescriptor::fromTemplate(source);
        if (!source.sets(ParticleTemplate::kDirection) && node.direction != Vec3{0}) {
            descriptor.direction = node.direction;
        }
        const Texture* texture = &device.whiteTexture();
        const auto index = archive.textures.find(descriptor.texture);
        if (index.has_value()) {
            try {
                texture = &archive.textures.texture(device, *index);
            } catch (const std::exception& e) {
                log::warn("Tree particles: texture {}: {}", descriptor.texture, e.what());
            }
        } else {
            log::warn("Tree particles: texture {} missing from {}", descriptor.texture, tree.name);
        }
        const Mat4 at =
            i < pose.size() ? root * pose[i] : glm::translate(root, tree.worldPosition(i));
        m_field.start(descriptor, at, texture, static_cast<u32>(i + 1));
        m_nodes.push_back({i, index});
    }
}

void TreeParticles::step(f32 seconds, const Mat4& root, std::span<const Mat4> pose) {
    for (usize i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].node < pose.size()) {
            m_field.setNode(i, root * pose[m_nodes[i].node]);
        }
    }
    m_field.step(seconds);
}

void TreeParticles::setTextureFrame(u32 slot, const Texture& texture) {
    for (usize i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].texture == slot) {
            m_field.setTexture(i, texture);
        }
    }
}

void TreeParticles::draw(RenderDevice& device, const Mat4& clip, const Vec3& right,
                         const Vec3& up) const {
    m_field.draw(device, clip, right, up);
}
} // namespace gdl
