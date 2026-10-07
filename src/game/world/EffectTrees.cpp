#include "game/world/EffectTrees.h"

#include <algorithm>
#include <cmath>
#include <ranges>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/DynamicLights.h"

namespace gdl::game {
namespace {
// Larger than a normal single-tick projectile/attachment step. A discontinuous
// caller relocation should appear at its destination, not streak across the level.
constexpr f32 kPresentationCutDistance = 32.0f;

Mat4 blendPlacement(const Mat4& previous, const Mat4& current, f32 blend) {
    Mat3 from{previous};
    Mat3 to{current};
    Vec3 fromScale{0};
    Vec3 toScale{0};
    for (s32 axis = 0; axis < 3; ++axis) {
        fromScale[axis] = glm::length(from[axis]);
        toScale[axis] = glm::length(to[axis]);
        if (fromScale[axis] < 1e-6f || toScale[axis] < 1e-6f) {
            return current;
        }
        from[axis] /= fromScale[axis];
        to[axis] /= toScale[axis];
    }
    // Attachments can contain shear or reflection. Do not turn those into an
    // unrelated quaternion rotation; preserve their authored basis instead.
    const auto orthogonal = [](const Mat3& basis) {
        return std::abs(glm::determinant(basis) - 1.0f) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[1])) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[2])) < 1e-3f &&
               std::abs(glm::dot(basis[1], basis[2])) < 1e-3f;
    };
    Mat4 out = current;
    if (orthogonal(from) && orthogonal(to)) {
        out = glm::mat4_cast(glm::slerp(glm::quat_cast(from), glm::quat_cast(to), blend));
        out = glm::scale(out, glm::mix(fromScale, toScale, blend));
    }
    out[3] = glm::mix(previous[3], current[3], blend);
    return out;
}
} // namespace

Mat4 EffectTrees::Effect::transform() const {
    Mat4 basis = attachment.value_or(glm::rotate(Mat4{1.0f}, yaw, Vec3{0, 1, 0}));
    basis[3] = Vec4{position, 1.0f};
    const f32 remaining = secondsLeftOf(*this);
    const f32 shrink = shrinks && remaining < 0.2f ? std::max(5 * remaining + 0.001f, 0.0f) : 1;
    return glm::scale(basis, Vec3{scale * shrink} * stretch);
}

void EffectTrees::placeAt(u32 id, const Mat4& attachment, std::optional<Vec3> flightDirection) {
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        if (effect->id == id) {
            effect->attachment = attachment;
            effect->position = Vec3{attachment[3]};
            effect->flightDirection = flightDirection;
            return;
        }
    }
}

void EffectTrees::redirect(u32 id, const Vec3& position, const Vec3& velocity) {
    for (const auto& effect : m_effects) {
        if (effect->id == id) {
            effect->position = position;
            effect->velocity = velocity;
            effect->yaw = std::atan2(velocity.x, velocity.z);
            effect->presentationCaptured = false; // contact redirects are cuts, not curves
            if (effect->flightDirection) {
                effect->flightDirection = velocity;
            }
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
    std::optional<TreeInfo> sparseTree;
    if (effect.missingObjectsAreEmpty) {
        // AtreeNodeInit -> MBOX_ReallyFindObject(create=1) substitutes AAANULLOBJ.
        // Several WEAP_TW trees intentionally omit XNWEAP while retaining their
        // particle children. Keep node indices/poses; never mutate the shared archive.
        sparseTree = *effect.tree;
        for (auto& node : sparseTree->nodes) {
            if (!node.object.empty() && !effect.archive->models.find(node.object)) {
                node.object.clear();
            }
        }
    }
    const auto& geometry = sparseTree ? *sparseTree : *effect.tree;
    const bool mesh = effect.model.bind(geometry, effect.archive->models, effect.archive->textures,
                                        *effect.device, effect.lenders);
    const bool wantsMesh = std::ranges::any_of(geometry.nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
    if (wantsMesh && !mesh) {
        return false;
    }
    effect.model.setAppearance(effect.unlit, effect.tint, effect.depthWrite, effect.additive);
    if (effect.emitParticles) {
        effect.particles.bind(*effect.tree, *effect.archive, *effect.device, effect.transform(),
                              effect.pose.matrices(), effect.lenders);
        effect.particles.setLocalScales(effect.pose.poses());
    }
    return mesh || effect.particles.field().size() > 0;
}

void EffectTrees::stop(u32 id) {
    std::erase_if(m_effects,
                  [id](const std::unique_ptr<Effect>& effect) { return effect->id == id; });
}

void EffectTrees::setTextureLenders(std::span<TextureSet* const> lenders) {
    m_lenders.assign(lenders.begin(), lenders.end());
}

void EffectTrees::finish(u32 id) {
    for (const auto& effect : m_effects) {
        if (effect->id == id) {
            effect->retiring = true;
            effect->particles.stop();
            for (usize i = 0; i < effect->trails.size(); ++i) {
                effect->trails.stop(i);
            }
        }
    }
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

f32 EffectTrees::secondsLeftOf(const Effect& effect) {
    return effect.timed || effect.tree == nullptr || effect.tree->sequences.empty()
               ? effect.secondsLeft
               : (static_cast<f32>(effect.player.frameCount()) - effect.player.frame()) *
                     effect.player.secondsPerFrame() / std::max(effect.playbackRate, 1e-6f);
}

std::optional<f32> EffectTrees::remaining(u32 id) const {
    for (const auto& effect : m_effects) {
        if (effect->id == id) {
            return std::max(secondsLeftOf(*effect), 0.0f);
        }
    }
    return std::nullopt;
}

void EffectTrees::shortenLifetime(u32 id, f32 secondsLost, f32 maximum) {
    for (auto& effect : m_effects) {
        if (effect->id != id) {
            continue;
        }
        const f32 remaining = secondsLeftOf(*effect);
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

u32 EffectTrees::startParticles(RenderDevice& device, ItemArchive& archive,
                                const ParticleDescriptor& descriptor, u32 textureSlot,
                                const Mat4& attachment) {
    auto effect = std::make_unique<Effect>();
    effect->id = m_nextId++;
    effect->name = descriptor.texture;
    effect->attachment = attachment;
    effect->position = Vec3{attachment[3]};
    effect->archive = &archive;
    effect->particleTextureSlot = textureSlot;
    effect->trails.start(descriptor, attachment, &archive.textures.texture(device, textureSlot),
                         effect->id);
    const bool known = std::ranges::any_of(m_motions, [&](const auto& motion) {
        return motion->archive == &archive && motion->lenders.empty();
    });
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

u32 EffectTrees::startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                          const Vec3& position, const Setting& setting,
                          std::span<TextureSet* const> textureLenders) {
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
    effect->stretch = setting.stretch;
    effect->fadeSeconds = setting.fadeSeconds;
    effect->yaw = setting.yaw;
    effect->unlit = setting.unlit;
    effect->depthWrite = setting.depthWrite;
    effect->tint = setting.tint;
    effect->playbackRate = setting.playbackRate;
    effect->light = setting.light;
    effect->velocity = setting.velocity;
    effect->timed = setting.seconds > 0.0f;
    effect->persistent = setting.persistent;
    effect->emitParticles = setting.emitParticles;
    effect->missingObjectsAreEmpty = setting.missingObjectsAreEmpty;
    effect->additive = setting.additive;
    effect->repeats = (effect->timed || effect->persistent) && setting.then.empty() && setting.loop;
    effect->then = setting.then;
    effect->morphIn = setting.morphIn;
    effect->holdForMorph = setting.holdForMorph;
    effect->shrinks = setting.shrinks;
    effect->device = &device;
    effect->archive = &archive;
    effect->lenders.assign(textureLenders.begin(), textureLenders.end());
    effect->lenders.insert(effect->lenders.end(), m_lenders.begin(), m_lenders.end());
    if (setting.seconds > 0.0f) {
        effect->secondsLeft = setting.seconds;
    }
    if (effect->tree->sequences.empty()) {
        effect->pose.rest(*effect->tree);
        if (!effect->timed) {
            effect->secondsLeft = kStillSeconds;
        }
    } else {
        const f32 frame = setting.settled
                              ? static_cast<f32>(std::max(effect->tree->sequences[0].frames - 1, 0))
                              : 0;
        effect->player.start(effect->tree->sequences[0], 0, 0, frame);
        effect->pose.evaluate(*effect->tree, 0, frame);
        // StartFXTree substitutes thirty frames for an empty sequence, using
        // that sequence's rate. It is a timed still, not a completed animation.
        const auto& sequence = effect->tree->sequences[0];
        if (sequence.frames == 0 && !effect->timed) {
            effect->timed = true;
            effect->secondsLeft = 30.0f * effect->player.secondsPerFrame();
        }
    }
    if (!bindVisuals(*effect)) {
        return 0;
    }
    effect->model.setFrame(0, static_cast<s32>(effect->player.frame()));
    const bool known = std::ranges::any_of(m_motions, [&](const std::unique_ptr<Motion>& motion) {
        return motion->archive == &archive && motion->lenders == effect->lenders;
    });
    if (!known) {
        auto motion = std::make_unique<Motion>();
        motion->archive = &archive;
        motion->lenders = effect->lenders;
        motion->animator.bind(archive.trees.textureAnimations(), archive.textures, device,
                              effect->lenders);
        m_motions.push_back(std::move(motion));
    }
    const u32 id = effect->id;
    for (const auto& motion : m_motions) {
        if (motion->archive == &archive && motion->lenders == effect->lenders) {
            motion->animator.apply(effect->model, *effect->tree, 0,
                                   static_cast<s32>(effect->player.frame()));
            motion->animator.apply(effect->particles, *effect->tree, 0,
                                   static_cast<s32>(effect->player.frame()));
        }
    }
    m_effects.push_back(std::move(effect));
    return id;
}

void EffectTrees::lights(std::vector<PointLight>& out) const {
    for (const std::unique_ptr<Effect>& newest : m_effects | std::views::reverse) {
        const Effect& effect = *newest;
        if (!effect.light.has_value() || effect.retiring || effect.light->radius <= 0.0f) {
            continue;
        }
        f32 share = 1.0f;
        if (effect.light->swells) {
            const f32 remaining = secondsLeftOf(effect);
            const f32 life = effect.lived + std::max(remaining, 0.0f);
            const f32 phase = life > 0.0f ? std::clamp(effect.lived / life, 0.0f, 1.0f) : 1.0f;
            share = phase < 0.5f ? 2.0f * phase : 2.0f * (1.0f - phase);
        }
        if (share <= 0.0f) {
            continue;
        }
        PointLight light;
        light.position = Vec3{effect.transform()[3]} + Vec3{0.0f, DynamicLights::kEffectLift, 0.0f};
        light.color = effect.light->color;
        light.radius = effect.light->radius * share;
        light.intensity = DynamicLights::kEffectIntensity;
        out.push_back(light);
    }
}

void EffectTrees::capturePresentation() {
    m_presentationAdvanced = false;
    for (const auto& motion : m_motions) {
        motion->animator.advance(0);
    }
    for (const auto& effect : m_effects) {
        effect->previousTransform = effect->transform();
        effect->previousDirection = effect->flightDirection;
        effect->previousTree = effect->tree;
        effect->previousGeneration = effect->player.generation();
        effect->previousFrame = effect->player.presentationFrame();
        effect->presentationCaptured = !effect->retiring;
    }
}

void EffectTrees::snapPresentation(u32 id) {
    for (const auto& effect : m_effects) {
        if (effect->id == id) {
            effect->presentationCaptured = false;
            return;
        }
    }
}

void EffectTrees::update(f32 seconds) {
    m_presentationAdvanced = seconds > 0;
    for (const std::unique_ptr<Motion>& motion : m_motions) {
        motion->animator.advance(seconds);
    }
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        effect->lived += seconds;
        if (effect->particleTextureSlot) {
            for (const auto& motion : m_motions) {
                if (motion->archive != effect->archive || !motion->lenders.empty()) {
                    continue;
                }
                for (usize i = 0; i < motion->animator.size(); ++i) {
                    if (!motion->animator.keyed(i) &&
                        motion->animator.slot(i) == *effect->particleTextureSlot) {
                        if (const auto state = motion->animator.motion(i); state.frame != nullptr) {
                            effect->trails.setTexture(0, *state.frame);
                        }
                    }
                }
            }
        }
        if (effect->retiring) {
            effect->particles.step(seconds, effect->transform(), effect->pose.matrices());
            effect->trails.step(seconds);
            for (const auto& motion : m_motions) {
                if (effect->tree != nullptr && motion->archive == effect->archive &&
                    motion->lenders == effect->lenders) {
                    motion->animator.apply(effect->particles, *effect->tree,
                                           effect->player.sequence(),
                                           static_cast<s32>(effect->player.frame()));
                }
            }
            continue;
        }
        const f32 beforeMorph = effect->morphIn.value_or(0.0f);
        const f32 motionSeconds = effect->holdForMorph && !effect->then.empty()
                                      ? std::max(seconds - beforeMorph, 0.0f)
                                      : seconds;
        effect->position += effect->velocity * motionSeconds;
        if (effect->morphIn) {
            *effect->morphIn -= seconds;
        }
        for (usize i = 0; i < effect->trails.size(); ++i) {
            effect->trails.setNode(i, effect->attachment.has_value()
                                          ? effect->transform()
                                          : glm::translate(Mat4{1.0f}, effect->position));
        }
        effect->trails.step(seconds);
        if (effect->tree == nullptr) {
            continue;
        }
        if (effect->tree->sequences.empty() || effect->timed) {
            effect->secondsLeft -= seconds;
        }
        // Played through, it gives way to the tree that repeats in its place.
        f32 animationSeconds = seconds;
        const bool morphReady = effect->morphIn
                                    ? *effect->morphIn <= 0
                                    : effect->tree->sequences.empty() || effect->player.finished();
        if (!effect->then.empty() && morphReady) {
            if (effect->morphIn) {
                animationSeconds = std::max(seconds - beforeMorph, 0.0f);
                effect->morphIn.reset();
            }
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
            effect->player.advance(animationSeconds * effect->playbackRate, effect->repeats);
            effect->pose.evaluate(*effect->tree, effect->player.sequence(), effect->player.frame());
            effect->model.setFrame(effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
            if (effect->persistent && !effect->repeats && effect->player.finished()) {
                effect->particles.stop();
            }
        }
        effect->particles.setLocalScales(effect->pose.poses());
        effect->particles.step(seconds, effect->transform(), effect->pose.matrices());
        for (const std::unique_ptr<Motion>& motion : m_motions) {
            if (motion->archive != effect->archive || motion->lenders != effect->lenders) {
                continue;
            }
            motion->animator.apply(effect->model, *effect->tree, effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
            motion->animator.apply(effect->particles, *effect->tree, effect->player.sequence(),
                                   static_cast<s32>(effect->player.frame()));
        }
    }
    std::erase_if(m_effects, [](const std::unique_ptr<Effect>& effect) {
        if (effect->tree == nullptr) {
            return !effect->trails.active(0);
        }
        if (effect->retiring) {
            return effect->particles.field().particleCount() == 0 &&
                   effect->trails.particleCount() == 0;
        }
        if (effect->persistent) {
            return false;
        }
        const bool timed = effect->tree->sequences.empty() || effect->timed;
        return timed ? effect->secondsLeft <= 0.0f : effect->player.finished();
    });
}

void EffectTrees::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const CameraFrame* camera, f32 frameBlend) const {
    const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
    const f32 blend = std::clamp(frameBlend, 0.0f, 1.0f);
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        auto direction = effect->flightDirection;
        Mat4 placement = effect->transform();
        const bool continuous = frameBlend >= 0 && effect->presentationCaptured &&
                                effect->previousTree == effect->tree &&
                                glm::distance(Vec3{effect->previousTransform[3]},
                                              Vec3{placement[3]}) <= kPresentationCutDistance;
        const TreePose* pose = &effect->pose;
        f32 visualFrame = effect->player.frame();
        if (continuous) {
            placement = blendPlacement(effect->previousTransform, placement, blend);
            const auto previousDirection = effect->previousDirection;
            if (direction && previousDirection && glm::dot(*direction, *previousDirection) > 0) {
                direction = glm::mix(*previousDirection, *direction, blend);
            }
            if (effect->tree != nullptr && effect->player.playing()) {
                visualFrame =
                    effect->previousGeneration == effect->player.generation()
                        ? glm::mix(effect->previousFrame, effect->player.presentationFrame(), blend)
                        : effect->player.presentationFrame();
                effect->presentationPose.evaluate(*effect->tree, effect->player.sequence(),
                                                  visualFrame, false, true);
                pose = &effect->presentationPose;
            }
        }
        const Mat4 placed = direction.has_value() ? frame.along(placement, *direction) : placement;
        if (!effect->retiring && effect->tree != nullptr) {
            effect->model.setPresentationFrame(effect->player.sequence(), visualFrame);
            for (const auto& motion : m_motions) {
                if (motion->archive == effect->archive && motion->lenders == effect->lenders) {
                    motion->animator.apply(effect->model, *effect->tree, effect->player.sequence(),
                                           visualFrame,
                                           motion->animator.presentationOffset(frameBlend));
                }
            }
            // Its last moments fade it out (ProcessEffects' fxfade).
            const f32 alpha =
                effect->fadeSeconds > 0.0f
                    ? std::clamp(secondsLeftOf(*effect) / effect->fadeSeconds, 0.0f, 1.0f)
                    : 1.0f;
            if (camera != nullptr) {
                // Traverse facing parents before their children, as for any
                // other local transform. Facing only the final mesh loses
                // offsets/rotations under Garm's laser-ribbon effect nodes.
                const auto matrices = pose->drawMatrices(placed, *camera);
                effect->model.draw(device, clip, Mat4{1}, lighting, matrices, nullptr, alpha);
            } else {
                effect->model.draw(device, clip, placed, lighting, pose->matrices(), nullptr,
                                   alpha);
            }
        }
        const f32 particleBlend = m_presentationAdvanced ? frameBlend : -1.0f;
        for (const auto& motion : m_motions) {
            if (motion->archive != effect->archive || motion->lenders != effect->lenders) {
                continue;
            }
            const auto offset = motion->animator.presentationOffset(particleBlend);
            if (offset && effect->tree != nullptr) {
                motion->animator.apply(effect->particles, *effect->tree, effect->player.sequence(),
                                       visualFrame, offset);
            }
            effect->trails.clearTextureBlends();
            const auto textureSlot = effect->particleTextureSlot;
            if (offset && textureSlot) {
                const usize slot = *textureSlot;
                for (usize i = 0; i < motion->animator.size(); ++i) {
                    if (!motion->animator.keyed(i) && motion->animator.slot(i) == slot) {
                        const auto sample = motion->animator.motion(i, offset);
                        if (sample.frame != nullptr) {
                            effect->trails.setTextureBlend(0, *sample.frame, sample.nextFrame,
                                                           sample.frameBlend);
                        }
                    }
                }
            }
        }
        effect->particles.draw(device, clip, frame.right, frame.up, particleBlend);
        effect->trails.draw(device, clip, frame.right, frame.up, particleBlend);
    }
}

void EffectTrees::clear() {
    m_effects.clear();
    m_motions.clear();
    m_lenders.clear();
    m_presentationAdvanced = false;
    m_nextId = 1;
}

} // namespace gdl::game
