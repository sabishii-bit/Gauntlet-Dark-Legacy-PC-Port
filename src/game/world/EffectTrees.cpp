#include "game/world/EffectTrees.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

Mat4 EffectTrees::Effect::transform() const {
    Mat4 basis = attachment.value_or(glm::rotate(Mat4{1.0f}, yaw, Vec3{0, 1, 0}));
    basis[3] = Vec4{position, 1.0f};
    return glm::scale(basis, Vec3{scale});
}

void EffectTrees::placeAt(u32 id, const Mat4& attachment) {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        if (effect->id == id) {
            effect->attachment = attachment;
            effect->position = Vec3{attachment[3]};
            return;
        }
    }
}

bool EffectTrees::start(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                        const Vec3& position, f32 scale) {
    Setting setting;
    setting.scale = scale;
    return startSet(device, archive, tree, position, setting) != 0;
}

bool EffectTrees::bindVisuals(Effect& effect) {
    const bool mesh = effect.model.bind(*effect.tree, effect.archive->models,
                                        effect.archive->textures, *effect.device);
    const bool wantsMesh = std::ranges::any_of(effect.tree->nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
    if (wantsMesh && !mesh) {
        return false;
    }
    effect.model.setAppearance(effect.unlit, effect.tint, effect.depthWrite);
    effect.particles.bind(*effect.tree, *effect.archive, *effect.device, effect.transform(),
                          effect.pose.matrices());
    return mesh || effect.particles.field().size() > 0;
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

void EffectTrees::shortenLifetime(u32 id, f32 secondsLost, f32 maximum) {
    for (auto& effect : m_effects) {
        if (effect->id != id) {
            continue;
        }
        const f32 remaining =
            effect->timed || effect->tree->sequences.empty()
                ? effect->secondsLeft
                : (static_cast<f32>(effect->player.frameCount()) - effect->player.frame()) *
                      effect->player.secondsPerFrame() / std::max(effect->playbackRate, 1e-6f);
        // The cap applies before the loss: a long-lived reflected shot is set
        // to ten seconds; only subsequent contacts subtract a second.
        effect->secondsLeft =
            std::max(0.0f, remaining > maximum ? maximum : remaining - secondsLost);
        effect->timed = true;
        return;
    }
}

void EffectTrees::attachTrail(u32 id, const ParticleDescriptor& descriptor,
                              const Texture& texture) {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        if (effect->id == id) {
            effect->trails.start(descriptor,
                                 effect->attachment.has_value()
                                     ? effect->transform()
                                     : glm::translate(Mat4{1.0f}, effect->position),
                                 &texture, id);
            return;
        }
    }
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
    effect->name = std::string(tree);
    effect->id = m_nextId++;
    effect->position = position;
    effect->scale = setting.scale;
    effect->yaw = setting.yaw;
    effect->unlit = setting.unlit;
    effect->depthWrite = setting.depthWrite;
    effect->tint = setting.tint;
    effect->playbackRate = setting.playbackRate;
    effect->velocity = setting.velocity;
    effect->timed = setting.seconds > 0.0f;
    effect->repeats = effect->timed && setting.then.empty() && setting.loop;
    effect->then = setting.then;
    effect->device = &device;
    effect->archive = &archive;
    if (setting.seconds > 0.0f) {
        effect->secondsLeft = setting.seconds;
    }
    if (effect->tree->sequences.empty()) {
        effect->pose.rest(*effect->tree);
        if (!effect->timed) {
            effect->secondsLeft = kStillSeconds;
        }
    } else {
        effect->player.start(effect->tree->sequences[0], 0);
        effect->pose.evaluate(*effect->tree, 0, 0.0f);
    }
    if (!bindVisuals(*effect)) {
        return 0;
    }
    effect->model.setFrame(0, 0);
    const bool known = std::ranges::any_of(m_motions, [&](const std::unique_ptr<Motion>& motion) {
        return motion->archive == &archive;
    });
    if (!known) {
        auto motion = std::make_unique<Motion>();
        motion->archive = &archive;
        motion->animator.bind(archive.trees.textureAnimations(), archive.textures, device);
        m_motions.push_back(std::move(motion));
    }
    const u32 id = effect->id;
    for (const auto& motion : m_motions) {
        if (motion->archive == &archive) {
            motion->animator.apply(effect->model, *effect->tree, 0, 0);
        }
    }
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
        for (usize i = 0; i < effect->trails.size(); ++i) {
            effect->trails.setNode(i, effect->attachment.has_value()
                                          ? effect->transform()
                                          : glm::translate(Mat4{1.0f}, effect->position));
        }
        effect->trails.step(seconds);
        if (effect->tree->sequences.empty() || effect->timed) {
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
                effect->tree = &tree;
                effect->name = name;
                if (tree.sequences.empty()) {
                    effect->pose.rest(tree);
                } else {
                    effect->player.start(tree.sequences[0], 0);
                    effect->pose.evaluate(tree, 0, 0);
                }
                if (!bindVisuals(*effect)) {
                    effect->secondsLeft = 0;
                }
            }
        }
        if (!effect->tree->sequences.empty()) {
            effect->player.advance(seconds * effect->playbackRate, effect->repeats);
            effect->pose.evaluate(*effect->tree, effect->player.sequence(), effect->player.frame());
            effect->model.setFrame(effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
        }
        effect->particles.step(seconds, effect->transform(), effect->pose.matrices());
        for (const std::unique_ptr<Motion>& motion : m_motions) {
            if (motion->archive != effect->archive) {
                continue;
            }
            const auto show = [&](const TextureMotion& moved) {
                if (moved.frame != nullptr) {
                    effect->particles.setTextureFrame(moved.slot, *moved.frame);
                }
            };
            motion->animator.apply(effect->model, *effect->tree, effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
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
                    if (const auto moved =
                            motion->animator.motionAt(sequence.textureAnimationStart + i, frame)) {
                        show(*moved);
                    }
                }
                for (const TreeNodeInfo& node : effect->tree->nodes) {
                    if (const auto moved =
                            motion->animator.motionAt(node.textureAnimation, frame)) {
                        show(*moved);
                    }
                }
            }
        }
    }
    std::erase_if(m_effects, [](const std::unique_ptr<Effect>& effect) {
        const bool timed = effect->tree->sequences.empty() || effect->timed;
        return timed ? effect->secondsLeft <= 0.0f : effect->player.finished();
    });
}

void EffectTrees::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const CameraFrame* camera) const {
    const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        const Mat4 placed = effect->transform();
        effect->model.draw(device, clip, placed, lighting, effect->pose.matrices(), camera);
        effect->particles.draw(device, clip, frame.right, frame.up);
        effect->trails.draw(device, clip, frame.right, frame.up);
    }
}

void EffectTrees::clear() {
    m_effects.clear();
    m_motions.clear();
    m_frames = 0.0f;
    m_nextId = 1;
}

} // namespace gdl::game
