#include "game/world/PlayerFamiliar.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace gdl::game {
bool PlayerFamiliar::bind(RenderDevice& device, ItemArchive& archive, s32 level, const Vec3& offset,
                          std::span<TextureSet* const> lenders) {
    m_tier = tierFor(level);
    return bindTree(device, archive, m_tier > 0 ? std::format("FAMILIAR{}", m_tier) : "", offset,
                    lenders);
}
bool PlayerFamiliar::bindTree(RenderDevice& device, ItemArchive& archive, std::string_view name,
                              const Vec3& offset, std::span<TextureSet* const> lenders) {
    m_tree = nullptr;
    m_model.clear();
    m_textures.clear();
    m_player.stop();
    m_pose = TreePose{};
    m_previousPose = TreePose{};
    m_offset = offset;
    m_presentationAdvanced = false;
    const auto index = archive.trees.find(name);
    if (name.empty() || !index.has_value()) {
        return false;
    }
    const auto& tree = archive.trees.tree(*index);
    if (tree.sequences.empty() ||
        !m_model.bind(tree, archive.models, archive.textures, device, lenders)) {
        return false;
    }
    m_tree = &tree;
    m_textures.bind(archive.trees.textureAnimations(), archive.textures, device, lenders);
    m_player.start(tree.sequences[0], 0);
    update(0, false);
    return true;
}
void PlayerFamiliar::update(f32 seconds, bool attack) {
    if (m_tree == nullptr) {
        return;
    }
    m_previousPose = m_pose;
    m_previousGeneration = m_player.generation();
    m_previousFrame = m_player.presentationFrame();
    m_presentationAdvanced = seconds > 0;
    if (attack && m_tree->sequences.size() > 1 && m_player.sequence() == 0) {
        m_player.start(m_tree->sequences[1], 1);
    }
    m_player.advance(seconds, m_player.sequence() == 0);
    if (m_player.finished() && m_player.sequence() != 0) {
        m_player.start(m_tree->sequences[0], 0);
    }
    const auto frame = static_cast<s32>(m_player.frame());
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.presentationFrame(), false, true);
    m_model.setFrame(m_player.sequence(), frame);
    m_textures.advance(seconds);
    m_textures.apply(m_model, *m_tree, m_player.sequence(), frame);
}
std::optional<CompanionVisual> PlayerFamiliar::visual(const Mat4& body, f32 alpha) const {
    if (m_tree == nullptr) {
        return std::nullopt;
    }
    return CompanionVisual{m_tree,
                           &m_model,
                           &m_textures,
                           static_cast<u32>(m_tier),
                           glm::translate(body, m_offset),
                           m_player.sequence(),
                           m_player.generation(),
                           m_player.presentationFrame(),
                           static_cast<f32>(m_textures.frame()) +
                               m_textures.presentationOffset(1).value_or(0),
                           alpha};
}
void PlayerFamiliar::draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
                          const WorldLighting& lighting, f32 alpha, const CameraFrame* camera,
                          f32 renderAlpha, TreeModel::Pass pass) const {
    if (m_tree != nullptr) {
        const f32 blend = renderAlpha < 0 || m_presentationAdvanced ? renderAlpha : 1.0f;
        f32 frame = blend >= 0 ? m_player.presentationFrame() : m_player.frame();
        m_drawPose = m_pose;
        if (blend >= 0 && blend < 1.0f && m_previousPose.posed() &&
            m_previousGeneration == m_player.generation()) {
            const f32 amount = std::clamp(blend, 0.0f, 1.0f);
            m_drawPose.blend(m_previousPose, amount, true);
            frame = std::lerp(m_previousFrame, frame, amount);
        }
        m_model.setPresentationFrame(m_player.sequence(), frame);
        m_textures.apply(m_model, *m_tree, m_player.sequence(), frame,
                         m_textures.presentationOffset(blend));
        m_model.draw(device, clip, glm::translate(body, m_offset), lighting, m_drawPose.matrices(),
                     camera, alpha, pass);
    }
}
} // namespace gdl::game
