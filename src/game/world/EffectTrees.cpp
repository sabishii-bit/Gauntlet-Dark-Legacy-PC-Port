#include "game/world/EffectTrees.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"

namespace gdl::game {

bool EffectTrees::start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                        const Vec3& position, f32 scale) {
    const auto index = archive.loaded() ? archive.trees.find(tree) : std::nullopt;
    if (!index.has_value()) {
        log::warn("Effects: no tree {} to play", tree);
        return false;
    }
    auto effect = std::make_unique<Effect>();
    effect->tree = &archive.trees.tree(*index);
    if (!effect->model.bind(*effect->tree, archive.models, archive.textures, device)) {
        return false;
    }
    effect->name = std::string(tree);
    effect->position = position;
    effect->scale = scale;
    effect->archive = &archive;
    if (effect->tree->sequences.empty()) {
        effect->pose.rest(*effect->tree);
        effect->secondsLeft = kStillSeconds;
    } else {
        effect->player.start(effect->tree->sequences[0], 0);
        effect->pose.evaluate(*effect->tree, 0, 0.0f);
        effect->model.setFrame(0, 0);
    }
    const bool known = std::ranges::any_of(
        m_motions, [&](const std::unique_ptr<Motion>& motion) { return motion->archive == &archive; });
    if (!known) {
        auto motion = std::make_unique<Motion>();
        motion->archive = &archive;
        motion->animator.bind(archive.trees.textureAnimations(), archive.textures, device);
        m_motions.push_back(std::move(motion));
    }
    m_effects.push_back(std::move(effect));
    return true;
}

void EffectTrees::update(f32 seconds) {
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const f32 whole = std::floor(m_frames);
    m_frames -= whole;
    for (const std::unique_ptr<Motion>& motion : m_motions) {
        if (whole > 0.0f) {
            motion->animator.step(static_cast<u32>(whole));
        }
    }
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        if (effect->tree->sequences.empty()) {
            effect->secondsLeft -= seconds;
        } else {
            effect->player.advance(seconds, false);
            effect->pose.evaluate(*effect->tree, effect->player.sequence(),
                                  effect->player.frame());
            effect->model.setFrame(effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
        }
        for (const std::unique_ptr<Motion>& motion : m_motions) {
            if (motion->archive != effect->archive) {
                continue;
            }
            for (usize i = 0; i < motion->animator.size(); ++i) {
                const TextureMotion moved = motion->animator.motion(i);
                if (moved.frame != nullptr) {
                    effect->model.setTextureFrame(moved.slot, moved.frame);
                } else {
                    effect->model.setTextureOffset(moved.slot, moved.offset);
                }
            }
        }
    }
    std::erase_if(m_effects, [](const std::unique_ptr<Effect>& effect) {
        return effect->tree->sequences.empty() ? effect->secondsLeft <= 0.0f
                                               : effect->player.finished();
    });
}

void EffectTrees::draw(RenderDevice& device, const Mat4& clip,
                       const WorldLighting& lighting) const {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        const Mat4 placed = glm::scale(glm::translate(Mat4{1.0f}, effect->position),
                                       Vec3{effect->scale, effect->scale, effect->scale});
        effect->model.draw(device, clip, placed, lighting, effect->pose.matrices());
    }
}

void EffectTrees::clear() {
    m_effects.clear();
    m_motions.clear();
    m_frames = 0.0f;
}

} // namespace gdl::game
