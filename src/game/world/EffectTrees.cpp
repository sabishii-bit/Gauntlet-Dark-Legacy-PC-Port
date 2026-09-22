#include "game/world/EffectTrees.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"

namespace gdl::game {

bool EffectTrees::start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                        const Vec3& position, f32 scale) {
    Setting setting;
    setting.scale = scale;
    return startSet(device, archive, tree, position, setting) != 0;
}

void EffectTrees::stop(u32 id) {
    std::erase_if(m_effects,
                  [id](const std::unique_ptr<Effect>& effect) { return effect->id == id; });
}

void EffectTrees::moveTo(u32 id, const Vec3& position) {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        if (effect->id == id) {
            effect->position = position;
        }
    }
}

bool EffectTrees::playing(u32 id) const {
    return std::ranges::any_of(
        m_effects, [id](const std::unique_ptr<Effect>& effect) { return effect->id == id; });
}

u32 EffectTrees::startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                          const Vec3& position, const Setting& setting) {
    const auto index = archive.loaded() ? archive.trees.find(tree) : std::nullopt;
    if (!index.has_value()) {
        log::warn("Effects: no tree {} to play", tree);
        return 0;
    }
    auto effect = std::make_unique<Effect>();
    effect->tree = &archive.trees.tree(*index);
    if (!effect->model.bind(*effect->tree, archive.models, archive.textures, device)) {
        return 0;
    }
    effect->name = std::string(tree);
    effect->id = m_nextId++;
    effect->position = position;
    effect->scale = setting.scale;
    effect->yaw = setting.yaw;
    effect->velocity = setting.velocity;
    effect->repeats = setting.seconds > 0.0f && setting.then.empty();
    effect->then = setting.then;
    effect->device = &device;
    effect->archive = &archive;
    if (setting.seconds > 0.0f) {
        effect->secondsLeft = setting.seconds;
    }
    if (effect->tree->sequences.empty()) {
        effect->pose.rest(*effect->tree);
        if (!effect->repeats) {
            effect->secondsLeft = kStillSeconds;
        }
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
    const u32 id = effect->id;
    m_effects.push_back(std::move(effect));
    return id;
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
        effect->position += effect->velocity * seconds;
        const bool timed = effect->repeats || !effect->then.empty();
        if (effect->tree->sequences.empty() || timed) {
            effect->secondsLeft -= seconds;
        }
        // Played through, it gives way to the tree that repeats in its place.
        if (!effect->then.empty() &&
            (effect->tree->sequences.empty() || effect->player.finished())) {
            const auto next = effect->archive->trees.find(effect->then);
            const std::string name = std::move(effect->then);
            effect->then.clear();
            effect->repeats = true;
            if (next.has_value() && effect->device != nullptr) {
                const TreeInfo& tree = effect->archive->trees.tree(*next);
                if (effect->model.bind(tree, effect->archive->models, effect->archive->textures,
                                       *effect->device)) {
                    effect->tree = &tree;
                    effect->name = name;
                    if (tree.sequences.empty()) {
                        effect->pose.rest(tree);
                    } else {
                        effect->player.start(tree.sequences[0], 0);
                    }
                }
            }
        }
        if (!effect->tree->sequences.empty()) {
            effect->player.advance(seconds, effect->repeats);
            effect->pose.evaluate(*effect->tree, effect->player.sequence(),
                                  effect->player.frame());
            effect->model.setFrame(effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
        }
        for (const std::unique_ptr<Motion>& motion : m_motions) {
            if (motion->archive != effect->archive) {
                continue;
            }
            const auto show = [&](const TextureMotion& moved) {
                if (moved.frame != nullptr) {
                    effect->model.setTextureFrame(moved.slot, moved.frame);
                } else {
                    effect->model.setTextureOffset(moved.slot, moved.offset);
                }
            };
            // The archive's own animations run on the clock; the tree's texture nodes and
            // the sequence's own animations are read off at the frame the tree has reached.
            for (usize i = 0; i < motion->animator.size(); ++i) {
                if (!motion->animator.keyed(i)) {
                    show(motion->animator.motion(i));
                }
            }
            if (!effect->tree->sequences.empty()) {
                const auto frame = static_cast<s32>(effect->player.frame());
                const TreeSequenceInfo& sequence =
                    effect->tree->sequences[effect->player.sequence()];
                for (s32 i = 0; i < sequence.textureAnimationCount; ++i) {
                    if (const auto moved = motion->animator.motionAt(sequence.textureAnimationStart + i, frame)) {
                        show(*moved);
                    }
                }
                for (const TreeNodeInfo& node : effect->tree->nodes) {
                    if (const auto moved = motion->animator.motionAt(node.textureAnimation, frame)) {
                        show(*moved);
                    }
                }
            }
        }
    }
    std::erase_if(m_effects, [](const std::unique_ptr<Effect>& effect) {
        const bool timed = effect->tree->sequences.empty() || effect->repeats ||
                           !effect->then.empty();
        return timed ? effect->secondsLeft <= 0.0f : effect->player.finished();
    });
}

void EffectTrees::draw(RenderDevice& device, const Mat4& clip,
                       const WorldLighting& lighting) const {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        const Mat4 turned = glm::rotate(glm::translate(Mat4{1.0f}, effect->position),
                                        effect->yaw, Vec3{0.0f, 1.0f, 0.0f});
        const Mat4 placed =
            glm::scale(turned, Vec3{effect->scale, effect->scale, effect->scale});
        effect->model.draw(device, clip, placed, lighting, effect->pose.matrices());
    }
}

void EffectTrees::clear() {
    m_effects.clear();
    m_motions.clear();
    m_frames = 0.0f;
    m_nextId = 1;
}

} // namespace gdl::game
