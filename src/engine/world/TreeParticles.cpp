#include "engine/world/TreeParticles.h"

#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl {
void TreeParticles::bind(const TreeInfo& tree, ItemArchive& archive, RenderDevice& device,
                         const Mat4& root, std::span<const Mat4> pose,
                         std::span<TextureSet* const> lenders) {
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
        auto index = archive.textures.find(descriptor.texture);
        TextureSet* owner = &archive.textures;
        if (!index.has_value()) {
            for (TextureSet* lender : lenders) {
                if (lender != nullptr) {
                    index = lender->find(descriptor.texture);
                    if (index.has_value()) {
                        owner = lender;
                        break;
                    }
                }
            }
        }
        if (index.has_value()) {
            try {
                texture = &owner->texture(device, *index);
            } catch (const std::exception& e) {
                log::warn("Tree particles: texture {}: {}", descriptor.texture, e.what());
            }
        } else {
            log::warn("Tree particles: texture {} missing from {}", descriptor.texture, tree.name);
        }
        const Mat4 at =
            i < pose.size() ? root * pose[i] : glm::translate(root, tree.worldPosition(i));
        m_field.start(descriptor, at, texture, static_cast<u32>(i + 1));
        // Borrowed indices are not slots of the owning archive's animator.
        m_nodes.push_back({i, owner == &archive.textures ? index : std::nullopt});
    }
}

void TreeParticles::setEmitting(bool emitting) {
    for (usize i = 0; i < m_nodes.size(); ++i) {
        m_field.setEmitting(i, emitting);
    }
}

void TreeParticles::setLocalScales(std::span<const NodePose> poses) {
    // MBDrawPsys (800CC880) multiplies width by node->scale.y. MBTreeSetScale
    // writes only that node, not its descendants. The composed world matrix
    // already positions the emitter; extracting its scale here double-counts
    // ancestors and incorrectly fattens effects attached to enlarged actors.
    for (usize i = 0; i < m_nodes.size(); ++i) {
        const usize node = m_nodes[i].node;
        m_field.setSpriteScale(i, node < poses.size() ? poses[node].scale.y : 1.0f);
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

void TreeParticles::stop() {
    for (usize i = 0; i < m_field.size(); ++i) {
        m_field.stop(i);
    }
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
