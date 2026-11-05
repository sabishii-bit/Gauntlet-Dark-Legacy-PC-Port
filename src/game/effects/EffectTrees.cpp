#include "game/effects/EffectTrees.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/effects/DynamicLights.h"
#include "game/effects/ThermalTextures.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {
namespace {
// Larger than a normal single-tick projectile/attachment step. A discontinuous
// caller relocation should appear at its destination, not streak across the level.
constexpr f32 kPresentationCutDistance = 32.0f;

void appendLenders(NativeStateDigest& digest, std::span<TextureSet* const> lenders) {
    digest.wide(lenders.size());
    for (const auto* lender : lenders) {
        digest.resource(lender);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
void appendEffect(NativeStateDigest& digest, const EffectTrees::EffectState& effect,
                  std::vector<const EffectTrees::EffectState*>& ancestors) {
    if (std::ranges::find(ancestors, &effect) != ancestors.end()) {
        digest.invalidate(); // A malformed retained prototype must not recurse forever.
        return;
    }
    ancestors.push_back(&effect);
    const auto& [instance, continuity, name, id, position, scale, stretch, fadeSeconds, yaw,
                 velocity, repeats, timed, persistent, emitParticles, missingObjectsAreEmpty,
                 additive, then, morphIn, holdForMorph, shrinks, retiring, device, tree, archive,
                 lenders, model, pose, player, secondsLeft, unlit, depthWrite, tint, alpha,
                 playbackRate, light, lived, trails, particles, attachment, flightDirection,
                 particleTextureSlot, previousTransform, previousDirection, previousTree,
                 previousGeneration, previousFrame, presentationCaptured, provenance,
                 morphPrototype] = effect;
    digest.values(instance, continuity, name, id, position, scale, stretch, fadeSeconds, yaw,
                  velocity, repeats, timed, persistent, emitParticles, missingObjectsAreEmpty,
                  additive, then, morphIn, holdForMorph, shrinks, retiring);
    // Device identity and the shared model's last draw sample are not native
    // state. The model is derived from tree/archive/lenders and sparse-bind flags;
    // device presence is retained because a future morph checks that capability.
    digest.flag(device != nullptr);
    digest.resource(tree);
    digest.resource(archive);
    appendLenders(digest, lenders);
    digest.flag(model != nullptr);
    digest.values(pose, player, secondsLeft, unlit, depthWrite, tint, alpha, playbackRate);
    digest.flag(light.has_value());
    if (light) {
        digest.fields<3>(*light);
    }
    digest.values(lived, trails, particles, attachment, flightDirection, particleTextureSlot,
                  previousTransform, previousDirection);
    digest.resource(previousTree);
    digest.values(previousGeneration, previousFrame, presentationCaptured);
    digest.flag(provenance.has_value());
    if (provenance) {
        digest.fields<4>(*provenance);
    }
    digest.flag(morphPrototype != nullptr);
    if (morphPrototype) {
        // Diagnostic preparations can retain a CPU birth prototype. It is an
        // actual future native branch, not an address or an omitted cache.
        appendEffect(digest, *morphPrototype, ancestors);
    }
    ancestors.pop_back();
}

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
        out = glm::mat4_cast(portable::slerp(glm::quat_cast(from), glm::quat_cast(to), blend));
        out = glm::scale(out, glm::mix(fromScale, toScale, blend));
    }
    out[3] = glm::mix(previous[3], current[3], blend);
    return out;
}
} // namespace

EffectTrees::EffectTrees(EffectTrees&& other) noexcept {
    *this = std::move(other);
}
EffectTrees& EffectTrees::operator=(EffectTrees&& other) noexcept {
    if (this != &other) {
        // Owner tokens stay with their addresses.
        m_effects = std::move(other.m_effects);
        m_motions = std::move(other.m_motions);
        m_motionCache = std::move(other.m_motionCache);
        m_visualCache = std::move(other.m_visualCache);
        ++m_localEpoch;
        ++other.m_localEpoch;
        m_lenders = std::move(other.m_lenders);
        m_presentationAdvanced = std::exchange(other.m_presentationAdvanced, false);
        m_nextId = std::exchange(other.m_nextId, 1);
        m_nextInstance = other.m_nextInstance;
    }
    return *this;
}

Mat4 EffectTrees::EffectState::transform(const Mat4* presentedParent) const {
    Mat4 basis = attachment.value_or(portable::rotate(Mat4{1.0f}, yaw, Vec3{0, 1, 0}));
    basis[3] = Vec4{position, 1.0f};
    if (presentedParent != nullptr && attachment) {
        basis = *presentedParent;
        // Own velocity, stretch and end-of-life shrink still belong to the
        // effect. Replace only the parent basis and its translation, retaining
        // any movement since the last gameplay attachment update.
        basis[3] = Vec4{Vec3{(*presentedParent)[3]} + position - Vec3{(*attachment)[3]}, 1};
    }
    const f32 remaining = secondsLeftOf(*this);
    const f32 shrink = shrinks && remaining < 0.2f ? std::max(5 * remaining + 0.001f, 0.0f) : 1;
    return glm::scale(basis, Vec3{scale * shrink} * stretch);
}

f32 EffectTrees::EffectState::opacity() const {
    return alpha *
           (fadeSeconds > 0 ? std::clamp(secondsLeftOf(*this) / fadeSeconds, 0.0f, 1.0f) : 1);
}

const TextureAnimator* EffectTrees::textureAnimator(const ItemArchive* archive,
                                                    std::span<TextureSet* const> lenders) const {
    for (const auto& motion : m_motions) {
        if (motion->archive == archive && std::ranges::equal(motion->lenders, lenders)) {
            return &motion->animator;
        }
    }
    return nullptr;
}

f32 EffectTrees::textureFrame(const Effect& effect) const {
    for (const auto& motion : m_motions) {
        if (motion->archive == effect.archive && motion->lenders == effect.lenders) {
            return static_cast<f32>(motion->animator.frame()) +
                   motion->animator.presentationOffset(1).value_or(0);
        }
    }
    return 0;
}

std::optional<f32> EffectTrees::textureFrame(const ItemArchive& archive,
                                             std::span<TextureSet* const> textureLenders) const {
    for (const auto& motion : m_motions) {
        if (motion->archive != &archive ||
            motion->lenders.size() != textureLenders.size() + m_lenders.size()) {
            continue;
        }
        const std::span<TextureSet* const> lenders{motion->lenders};
        if (std::ranges::equal(lenders.first(textureLenders.size()), textureLenders) &&
            std::ranges::equal(lenders.subspan(textureLenders.size()), m_lenders)) {
            return static_cast<f32>(motion->animator.frame()) +
                   motion->animator.presentationOffset(1).value_or(0);
        }
    }
    return std::nullopt;
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
            effect->yaw = portable::atan2(velocity.x, velocity.z);
            effect->presentationCaptured = false; // contact redirects are cuts, not curves
            ++effect->continuity;
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
    if (effect.morphPrototype) {
        // The prototype already holds this future tree's authenticated native
        // birth state. A replayed morph must not load/bind artwork in the
        // middle of a speculative tick. Keep the native pose/clock branch below
        // update() identical; only its resource construction uses the catalog.
        const auto& prototype = *effect.morphPrototype;
        if (prototype.tree != effect.tree) {
            return false;
        }
        effect.model = prototype.model;
        if (effect.emitParticles) {
            effect.particles = prototype.particles;
            effect.particles.setLocalScales(effect.pose.poses());
            effect.particles.step(0, effect.transform(), effect.pose.matrices());
        }
        return effect.model->bound() || effect.particles.field().size() > 0;
    }
    const auto cached = std::ranges::find_if(m_visualCache, [&](const Visual& visual) {
        return visual.archive == effect.archive && visual.device == effect.device &&
               visual.tree == effect.tree && visual.lenders == effect.lenders &&
               visual.missingObjectsAreEmpty == effect.missingObjectsAreEmpty &&
               visual.emitParticles == effect.emitParticles;
    });
    if (cached != m_visualCache.end()) {
        effect.model = cached->model;
        effect.particles = cached->particles;
        effect.particles.setLocalScales(effect.pose.poses());
        effect.particles.step(0, effect.transform(), effect.pose.matrices());
        return effect.model->bound() || effect.particles.field().size() > 0;
    }
    effect.model = std::make_shared<TreeModel>();
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
    const bool mesh = effect.model->bind(geometry, effect.archive->models, effect.archive->textures,
                                         *effect.device, effect.lenders);
    const bool wantsMesh = std::ranges::any_of(geometry.nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
    if (wantsMesh && !mesh) {
        return false;
    }
    if (effect.emitParticles) {
        effect.particles.bind(*effect.tree, *effect.archive, *effect.device, effect.transform(),
                              effect.pose.matrices(), effect.lenders);
        effect.particles.setLocalScales(effect.pose.poses());
    }
    if (!mesh && effect.particles.field().size() == 0) {
        return false;
    }
    m_visualCache.push_back({effect.device, effect.archive, effect.tree, effect.lenders,
                             effect.missingObjectsAreEmpty, effect.emitParticles, effect.model,
                             effect.particles});
    return true;
}

EffectTrees::Motion& EffectTrees::nativeBank(RenderDevice& device, ItemArchive& archive,
                                             std::span<TextureSet* const> lenders) {
    const auto matches = [&](const auto& motion) {
        return motion->archive == &archive && std::ranges::equal(motion->lenders, lenders);
    };
    if (const auto active = std::ranges::find_if(m_motions, matches); active != m_motions.end()) {
        return **active;
    }
    auto cached = std::ranges::find_if(m_motionCache, matches);
    if (cached == m_motionCache.end()) {
        auto motion = std::make_unique<Motion>();
        motion->archive = &archive;
        motion->lenders.assign(lenders.begin(), lenders.end());
        motion->animator.bind(archive.trees.textureAnimations(), archive.textures, device, lenders);
        m_motionCache.push_back(std::move(motion));
        cached = std::prev(m_motionCache.end());
    } else {
        // A rewind can precede the first use of this bank. Its resources stay
        // cached, but it begins again at the original native zero clock.
        (*cached)->animator.restoreClock({});
    }
    m_motions.push_back(cached->get());
    return **cached;
}

void EffectTrees::saveLocal(LocalSnapshot& state) const {
    state.m_owner.reset();
    state.m_effects.resize(m_effects.size());
    for (usize i = 0; i < m_effects.size(); ++i) {
        state.m_effects[i] = static_cast<const EffectState&>(*m_effects[i]);
    }
    state.m_motions = m_motions;
    state.m_clocks.resize(m_motions.size());
    for (usize i = 0; i < m_motions.size(); ++i) {
        state.m_clocks[i] = m_motions[i]->animator.clock();
    }
    state.m_lenders = m_lenders;
    state.m_presentationAdvanced = m_presentationAdvanced;
    state.m_nextId = m_nextId;
    state.m_nextInstance = m_nextInstance;
    state.m_epoch = m_localEpoch;
    state.m_owner = m_restoreOwner;
}

bool EffectTrees::canRestoreLocal(const LocalSnapshot& state) const {
    return !state.m_owner.expired() && state.m_owner.lock() == m_restoreOwner &&
           state.m_epoch == m_localEpoch;
}

bool EffectTrees::restoreLocal(const LocalSnapshot& state) {
    if (!canRestoreLocal(state)) {
        return false;
    }
    m_effects.resize(state.m_effects.size());
    for (usize i = 0; i < state.m_effects.size(); ++i) {
        if (!m_effects[i]) {
            m_effects[i] = std::make_unique<Effect>();
        }
        static_cast<EffectState&>(*m_effects[i]) = state.m_effects[i];
    }
    m_motions = state.m_motions;
    for (usize i = 0; i < m_motions.size(); ++i) {
        m_motions[i]->animator.restoreClock(state.m_clocks[i]);
    }
    m_lenders = state.m_lenders;
    m_presentationAdvanced = state.m_presentationAdvanced;
    m_nextId = state.m_nextId;
    m_nextInstance = state.m_nextInstance;
    return true;
}

void EffectTrees::appendMotionDigest(NativeStateDigest& digest, const Motion& motion,
                                     const TextureAnimator::Clock& clock) {
    const auto& [archive, lenders, animator] = motion;
    digest.resource(archive);
    appendLenders(digest, lenders);
    animator.appendBindingDigest(digest);
    digest.fields<3>(clock);
}

bool EffectTrees::appendNativeDigest(NativeStateDigest& digest) const {
    const auto& [effects, motions, motionCache, visualCache, lenders, advanced, nextId,
                 nextInstance, restoreOwner, localEpoch] = *this;
    digest.string("EffectTrees.1");
    digest.wide(effects.size());
    std::vector<const EffectState*> ancestors;
    for (const auto& effect : effects) {
        if (!effect) {
            digest.invalidate();
            return false;
        }
        appendEffect(digest, *effect, ancestors);
    }
    digest.wide(motions.size());
    for (const auto* motion : motions) {
        if (!motion) {
            digest.invalidate();
            return false;
        }
        appendMotionDigest(digest, *motion, motion->animator.clock());
    }
    appendLenders(digest, lenders);
    digest.values(advanced, nextId, nextInstance);
    // Retained binding caches survive rewind and are not active lifetimes.
    // Their resources are authenticated by registerNativeResources, not hashed
    // by speculative cache insertion order. Owner/epoch only guard local saves.
    static_cast<void>(motionCache);
    static_cast<void>(visualCache);
    static_cast<void>(restoreOwner);
    static_cast<void>(localEpoch);
    return digest.good();
}

bool EffectTrees::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& state) const {
    if (!canRestoreLocal(state) || state.m_motions.size() != state.m_clocks.size()) {
        digest.invalidate();
        return false;
    }
    const auto& [effects, motions, clocks, lenders, advanced, nextId, nextInstance, owner, epoch] =
        state;
    digest.string("EffectTrees.1");
    digest.wide(effects.size());
    std::vector<const EffectState*> ancestors;
    for (const auto& effect : effects) {
        appendEffect(digest, effect, ancestors);
    }
    digest.wide(motions.size());
    for (usize index = 0; index < motions.size(); ++index) {
        if (!motions[index]) {
            digest.invalidate();
            return false;
        }
        appendMotionDigest(digest, *motions[index], clocks[index]);
    }
    appendLenders(digest, lenders);
    digest.values(advanced, nextId, nextInstance);
    static_cast<void>(owner);
    static_cast<void>(epoch);
    return digest.good();
}

bool EffectTrees::registerNativeResources(NativeStateResources& registry) const {
    std::vector<const ItemArchive*> archives;
    std::vector<const TextureSet*> banks;
    const auto refreshArchive = [&](const ItemArchive* archive) {
        if ((archive == nullptr) || std::ranges::find(archives, archive) != archives.end()) {
            return true;
        }
        archives.push_back(archive);
        return registry.refreshArchive(*archive);
    };
    const auto refreshLenders = [&](std::span<TextureSet* const> lenders) {
        for (const auto* lender : lenders) {
            if ((lender != nullptr) && std::ranges::find(banks, lender) == banks.end()) {
                banks.push_back(lender);
                if (!registry.refreshTextures(*lender)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!refreshLenders(m_lenders)) {
        return false;
    }
    for (const auto& motion : m_motionCache) {
        if (!motion || !refreshArchive(motion->archive) || !refreshLenders(motion->lenders)) {
            return false;
        }
    }
    for (const auto& visual : m_visualCache) {
        if (!refreshArchive(visual.archive) || !refreshLenders(visual.lenders)) {
            return false;
        }
    }
    std::vector<const EffectState*> ancestors;
    // NOLINTNEXTLINE(misc-no-recursion): subsystem checkpoints recurse into their parts by design
    const auto refreshEffect = [&](const auto& self, const EffectState& effect) -> bool {
        if (std::ranges::find(ancestors, &effect) != ancestors.end() ||
            !refreshArchive(effect.archive) || !refreshLenders(effect.lenders)) {
            return false;
        }
        ancestors.push_back(&effect);
        const bool valid = !effect.morphPrototype || self(self, *effect.morphPrototype);
        ancestors.pop_back();
        return valid;
    };
    for (const auto& effect : m_effects) {
        if (!effect || !refreshEffect(refreshEffect, *effect)) {
            return false;
        }
    }
    return registry.good();
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

bool EffectTrees::tag(u32 id, const Provenance& provenance) {
    if (provenance.player < 0 || provenance.player >= 4 || provenance.generation == 0) {
        return false;
    }
    for (const auto& effect : m_effects) {
        if (effect->id == id) {
            if (effect->provenance && *effect->provenance != provenance) {
                return false;
            }
            effect->provenance = provenance;
            return true;
        }
    }
    return false;
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

f32 EffectTrees::secondsLeftOf(const EffectState& effect) {
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
    if (m_nextInstance == 0) {
        return 0;
    }
    auto effect = std::make_unique<Effect>();
    effect->instance = m_nextInstance++;
    effect->id = m_nextId++;
    effect->archive = &archive;
    initializeParticles(*effect, descriptor, archive.textures.texture(device, textureSlot),
                        textureSlot, attachment, effect->id);
    nativeBank(device, archive, {});
    const u32 id = effect->id;
    m_effects.push_back(std::move(effect));
    return id;
}

void EffectTrees::initializeParticles(Effect& effect, const ParticleDescriptor& descriptor,
                                      const Texture& texture, u32 textureSlot,
                                      const Mat4& attachment, u32 seed) {
    effect.name = descriptor.texture;
    effect.attachment = attachment;
    effect.position = Vec3{attachment[3]};
    effect.particleTextureSlot = textureSlot;
    effect.trails.start(descriptor, attachment, &texture, seed);
}

void EffectTrees::initializeTree(Effect& value, const Setting& setting) {
    auto* effect = &value;
    effect->scale = setting.scale;
    effect->stretch = setting.stretch;
    effect->fadeSeconds = setting.fadeSeconds;
    effect->yaw = setting.yaw;
    effect->unlit = setting.unlit;
    effect->depthWrite = setting.depthWrite;
    effect->tint = setting.tint;
    effect->alpha = std::clamp(setting.alpha, 0.0f, 1.0f);
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
}

u32 EffectTrees::startSet(RenderDevice& device, ItemArchive& archive, std::string_view tree,
                          const Vec3& position, const Setting& setting,
                          std::span<TextureSet* const> textureLenders) {
    if (m_nextInstance == 0) {
        return 0;
    }
    const auto index = archive.loaded() ? archive.trees.find(tree) : std::nullopt;
    if (!index.has_value()) {
        log::warn("Effects: no tree {} to play", tree);
        return 0;
    }
    auto effect = std::make_unique<Effect>();
    effect->instance = m_nextInstance++;
    effect->tree = &archive.trees.tree(*index);
    effect->name = std::string(tree);
    effect->id = m_nextId++;
    effect->position = position;
    effect->device = &device;
    effect->archive = &archive;
    effect->lenders.assign(textureLenders.begin(), textureLenders.end());
    effect->lenders.insert(effect->lenders.end(), m_lenders.begin(), m_lenders.end());
    initializeTree(*effect, setting);
    if (!bindVisuals(*effect)) {
        return 0;
    }
    effect->model->setFrame(0, static_cast<s32>(effect->player.frame()));
    nativeBank(device, archive, effect->lenders);
    const u32 id = effect->id;
    for (const auto& motion : m_motions) {
        if (motion->archive == &archive && motion->lenders == effect->lenders) {
            motion->animator.apply(*effect->model, *effect->tree, 0,
                                   static_cast<s32>(effect->player.frame()));
            motion->animator.apply(effect->particles, *effect->tree, 0,
                                   static_cast<s32>(effect->player.frame()));
        }
    }
    m_effects.push_back(std::move(effect));
    return id;
}

u32 EffectTrees::startPrepared(const Prepared& prepared, const Vec3& position,
                               const Setting& setting, const Mat4& particleAttachment,
                               std::optional<u32> particleSeed) {
    if (m_nextInstance == 0 || m_nextId == 0 || prepared.archive == nullptr ||
        prepared.textures == nullptr || !setting.then.empty() || setting.morphIn ||
        setting.holdForMorph) {
        return 0;
    }
    auto effect = std::make_unique<Effect>();
    effect->archive = prepared.archive;
    effect->device = prepared.device;
    effect->lenders.assign(prepared.lenders.begin(), prepared.lenders.end());
    if (prepared.tree != nullptr) {
        if (prepared.model == nullptr || prepared.particles == nullptr ||
            prepared.descriptor != nullptr ||
            (!prepared.model->bound() && prepared.particles->field().size() == 0)) {
            return 0;
        }
        effect->tree = prepared.tree;
        effect->name = prepared.tree->name;
        effect->position = position;
        initializeTree(*effect, setting);
        effect->model = std::make_shared<TreeModel>(*prepared.model);
        effect->model->setAppearance({.unlit = effect->unlit,
                                      .tint = effect->tint,
                                      .depthWrite = effect->depthWrite,
                                      .additive = effect->additive});
        effect->model->setFrame(0, static_cast<s32>(effect->player.frame()));
        if (effect->emitParticles) {
            effect->particles = *prepared.particles;
            // The prototype has not advanced. Updating its nodes at zero seconds
            // recreates bind's placement without reseeding or allocating artwork.
            effect->particles.setLocalScales(effect->pose.poses());
            effect->particles.step(0, effect->transform(), effect->pose.matrices());
        }
        if (!effect->model->bound() && effect->particles.field().size() == 0) {
            return 0;
        }
    } else {
        if (prepared.descriptor == nullptr || prepared.texture == nullptr ||
            !prepared.textureSlot || !prepared.lenders.empty() || !particleSeed) {
            return 0;
        }
        initializeParticles(*effect, *prepared.descriptor, *prepared.texture, *prepared.textureSlot,
                            particleAttachment, *particleSeed);
    }
    if (!prepareBank(prepared)) {
        return 0;
    }
    if (effect->tree != nullptr) {
        for (const auto& motion : m_motions) {
            if (motion->archive == effect->archive && motion->lenders == effect->lenders) {
                motion->animator.apply(*effect->model, *effect->tree, 0, effect->player.frame());
                motion->animator.apply(effect->particles, *effect->tree, 0, effect->player.frame());
            }
        }
    }
    effect->instance = m_nextInstance++;
    effect->id = m_nextId++;
    const u32 id = effect->id;
    m_effects.push_back(std::move(effect));
    return id;
}

u32 EffectTrees::startPreparedParticles(const Prepared& prepared, const Mat4& attachment) {
    if (prepared.tree != nullptr) {
        return 0;
    }
    return startPrepared(prepared, Vec3{0}, Setting{}, attachment, m_nextId);
}

bool EffectTrees::prepareBank(const Prepared& prepared) {
    if (prepared.archive == nullptr || prepared.textures == nullptr) {
        return false;
    }
    const bool known = std::ranges::any_of(m_motions, [&](const auto& motion) {
        return motion->archive == prepared.archive &&
               std::ranges::equal(motion->lenders, prepared.lenders);
    });
    if (!known) {
        const auto cached = std::ranges::find_if(m_motionCache, [&](const auto& motion) {
            return motion->archive == prepared.archive &&
                   std::ranges::equal(motion->lenders, prepared.lenders);
        });
        if (cached != m_motionCache.end()) {
            (*cached)->animator.restoreClock(prepared.textures->clock());
            m_motions.push_back(cached->get());
            return true;
        }
        auto motion = std::make_unique<Motion>();
        motion->archive = prepared.archive;
        motion->lenders.assign(prepared.lenders.begin(), prepared.lenders.end());
        motion->animator = *prepared.textures;
        m_motions.push_back(motion.get());
        m_motionCache.push_back(std::move(motion));
    }
    return true;
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
            ++effect->continuity;
            effect->presentationCaptured = false;
            return;
        }
    }
}

void EffectTrees::update(f32 seconds) {
    m_presentationAdvanced = seconds > 0;
    for (auto* motion : m_motions) {
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
            const TreeInfo* nextTree = nullptr;
            if (effect->morphPrototype) {
                nextTree = effect->morphPrototype->tree;
            } else if (const auto next = effect->archive->trees.find(effect->then)) {
                nextTree = &effect->archive->trees.tree(*next);
            }
            const std::string name = std::move(effect->then);
            effect->then.clear();
            effect->repeats = true;
            if (nextTree != nullptr && effect->device != nullptr) {
                // A replacement tree starts a new pose, even when it reuses the
                // same artwork. Do not interpolate through the previous phase.
                ++effect->continuity;
                effect->presentationCaptured = false;
                const TreeInfo& tree = *nextTree;
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
            effect->model->setFrame(effect->player.sequence(),
                                    static_cast<s32>(effect->player.frame()));
            if (effect->persistent && !effect->repeats && effect->player.finished()) {
                effect->particles.stop();
            }
        }
        effect->particles.setLocalScales(effect->pose.poses());
        effect->particles.step(seconds, effect->transform(), effect->pose.matrices());
        for (const auto* motion : m_motions) {
            if (motion->archive != effect->archive || motion->lenders != effect->lenders) {
                continue;
            }
            motion->animator.apply(*effect->model, *effect->tree, effect->player.sequence(),
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
                       const CameraFrame* camera, f32 frameBlend,
                       std::span<const DrawAttachment> attachments) const {
    const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
    const f32 blend = std::clamp(frameBlend, 0.0f, 1.0f);
    for (const std::unique_ptr<Effect>& effect : m_effects) {
        auto direction = effect->flightDirection;
        const auto attachment = std::ranges::find(attachments, effect->id, &DrawAttachment::effect);
        const Mat4* parent =
            attachment != attachments.end() && effect->attachment ? &attachment->parent : nullptr;
        Mat4 placement = effect->transform(parent);
        const bool continuous = frameBlend >= 0 && effect->presentationCaptured &&
                                effect->previousTree == effect->tree &&
                                glm::distance(Vec3{effect->previousTransform[3]},
                                              Vec3{placement[3]}) <= kPresentationCutDistance;
        const TreePose* pose = &effect->pose;
        f32 visualFrame = effect->player.frame();
        if (continuous) {
            if (parent == nullptr) {
                placement = blendPlacement(effect->previousTransform, placement, blend);
            }
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
                                                  visualFrame, TreePose::Sampling::Smooth);
                pose = &effect->presentationPose;
            }
        }
        const Mat4 placed = direction.has_value() ? frame.along(placement, *direction) : placement;
        if (!effect->retiring && effect->tree != nullptr) {
            effect->model->resetTextures();
            effect->model->setAppearance({.unlit = effect->unlit,
                                          .tint = effect->tint,
                                          .depthWrite = effect->depthWrite,
                                          .additive = effect->additive});
            effect->model->setPresentationFrame(effect->player.sequence(), visualFrame);
            for (const auto& motion : m_motions) {
                if (motion->archive == effect->archive && motion->lenders == effect->lenders) {
                    motion->animator.apply(*effect->model, *effect->tree, effect->player.sequence(),
                                           visualFrame,
                                           motion->animator.presentationOffset(frameBlend));
                }
            }
            // Its last moments fade it out (ProcessEffects' fxfade).
            const f32 alpha = effect->opacity();
            if (camera != nullptr) {
                // Traverse facing parents before their children, as for any
                // other local transform. Facing only the final mesh loses
                // offsets/rotations under Garm's laser-ribbon effect nodes.
                const auto matrices = pose->drawMatrices(placed, *camera);
                effect->model->draw(device, clip, Mat4{1}, lighting, matrices, nullptr, alpha);
            } else {
                effect->model->draw(device, clip, placed, lighting, pose->matrices(), nullptr,
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
        effect->particles.draw(device, clip, frame.right, frame.up, particleBlend,
                               kThermalTextures);
        effect->trails.draw(device, clip, frame.right, frame.up, particleBlend, kThermalTextures);
    }
}

void EffectTrees::reset() {
    m_effects.clear();
    m_motions.clear();
    m_lenders.clear();
    m_presentationAdvanced = false;
    m_nextId = 1;
}

void EffectTrees::clear() {
    reset();
    ++m_localEpoch;
    m_motionCache.clear();
    m_visualCache.clear();
}

} // namespace gdl::game
