#include "game/world/PlayerFamiliar.h"

#include <cmath>
#include <format>

namespace gdl::game {
bool PlayerFamiliar::bind(RenderDevice& device, ItemArchive& archive, s32 level,
                          const Vec3& offset) {
    m_tier = tierFor(level);
    return bindTree(device, archive, m_tier > 0 ? std::format("FAMILIAR{}", m_tier) : "", offset);
}
bool PlayerFamiliar::bindPhoenix(RenderDevice& device, ItemArchive& archive) {
    m_tier = 0;
    return bindTree(device, archive, "PHOENIX", Vec3{0});
}
bool PlayerFamiliar::bindTree(RenderDevice& device, ItemArchive& archive, std::string_view name,
                              const Vec3& offset) {
    m_tree = nullptr;
    m_model.clear();
    m_player.stop();
    m_offset = offset;
    m_frames = 0;
    const auto index = archive.trees.find(name);
    if (name.empty() || !index.has_value()) {
        return false;
    }
    const auto& tree = archive.trees.tree(*index);
    if (tree.sequences.empty() || !m_model.bind(tree, archive.models, archive.textures, device)) {
        return false;
    }
    m_tree = &tree;
    m_textures.bind(archive.trees.textureAnimations(), archive.textures, device);
    m_player.start(tree.sequences[0], 0);
    update(0, false);
    return true;
}
void PlayerFamiliar::update(f32 seconds, bool attack) {
    if (m_tree == nullptr) {
        return;
    }
    if (attack && m_tree->sequences.size() > 1 && m_player.sequence() == 0) {
        m_player.start(m_tree->sequences[1], 1);
    }
    m_player.advance(seconds, m_player.sequence() == 0);
    if (m_player.finished() && m_player.sequence() != 0) {
        m_player.start(m_tree->sequences[0], 0);
    }
    const auto frame = static_cast<s32>(m_player.frame());
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
    m_model.setFrame(m_player.sequence(), frame);
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const auto elapsed = static_cast<u32>(std::floor(m_frames));
    m_frames -= static_cast<f32>(elapsed);
    m_textures.step(elapsed);
    m_textures.apply(m_model, *m_tree, m_player.sequence(), frame);
}
void PlayerFamiliar::draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
                          const WorldLighting& lighting, f32 alpha,
                          const CameraFrame* camera) const {
    if (m_tree != nullptr) {
        m_model.draw(device, clip, glm::translate(body, m_offset), lighting, m_pose.matrices(),
                     camera, alpha);
    }
}
} // namespace gdl::game
