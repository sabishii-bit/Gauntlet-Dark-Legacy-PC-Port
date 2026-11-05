#include "game/tower/SumnerFigure.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {

void SumnerFigure::appendState(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("SumnerFigure.native.1");
    digest.resource(m_tree);
    digest.values(m_sequences, m_position, m_yaw, m_transform);
    digest.fields<7>(state);
}

bool SumnerFigure::appendNativeDigest(NativeStateDigest& digest) const {
    appendState(digest, m_sim);
    return digest.good();
}

bool SumnerFigure::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, binding] = saved;
    appendState(digest, state);
    return digest.good();
}

void SumnerFigure::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
}

bool SumnerFigure::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding;
}

bool SumnerFigure::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

bool SumnerFigure::load(RenderDevice& device, ItemArchive& items, const WorldLayout& layout) {
    clear();
    if (!items.loaded()) {
        log::warn("Sumner: the item archive is not unpacked");
        return false;
    }
    const auto tree = items.trees.find(kTree);
    if (!tree.has_value() ||
        !m_model.bind(items.trees.tree(*tree), items.models, items.textures, device)) {
        log::warn("Sumner: the {} figure could not be built", kTree);
        return false;
    }
    const WorldLocator* lookout = nullptr;
    for (const WorldLocator& locator : layout.locators()) {
        if ((locator.kind == LocatorKind::Event || locator.kind == LocatorKind::Sentry) &&
            locator.delay == kLookout) {
            lookout = &locator;
            break;
        }
    }
    if (lookout == nullptr) {
        log::warn("Sumner: the level has no lookout {} for him", kLookout);
        m_model.clear();
        return false;
    }
    m_tree = &items.trees.tree(*tree);
    for (usize i = 0; i < kSequences.size(); ++i) {
        const auto sequence = m_tree->findSequence(kSequences[i]);
        m_sequences[i] = sequence.has_value() ? static_cast<s32>(*sequence) : -1;
    }
    // Like the start markers, the lookout's heading points the way he came: a half turn
    // round faces him at the party.
    m_position = lookout->position;
    m_yaw = lookout->rotation.y + kPi;
    m_transform =
        portable::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
    m_sim.index = 0;
    m_sim.cutIn = false;
    const u32 first = sequenceFor(0);
    m_sim.player.start(m_tree->sequences[first], first);
    m_sim.pose.evaluate(*m_tree, first, 0.0f);
    m_sim.previousFrame = m_sim.player.presentationFrame();
    m_sim.previousGeneration = m_sim.player.generation();
    return true;
}

void SumnerFigure::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_model.clear();
    m_tree = nullptr;
    m_sequences.fill(-1);
    m_sim.index = 0;
    m_sim.cutIn = false;
    m_sim.player.stop();
    m_sim.pose = {};
    m_sim.previousFrame = 0;
    m_sim.previousGeneration = 0;
    m_sim.presentationAdvanced = false;
}

/** The sequence an index asks for, the stance standing in for any the tree lacks. */
u32 SumnerFigure::sequenceFor(s32 index) const {
    s32 sequence = -1;
    if (index >= 0 && static_cast<usize>(index) < m_sequences.size()) {
        sequence = m_sequences[static_cast<usize>(index)];
    }
    if (sequence < 0) {
        sequence = std::max(m_sequences[0], 0);
    }
    return static_cast<u32>(sequence);
}

void SumnerFigure::play(s32 index) {
    if (!loaded()) {
        return;
    }
    m_sim.index = index;
    m_sim.cutIn = true;
}

void SumnerFigure::update(f32 seconds) {
    m_sim.presentationAdvanced = seconds > 0 && loaded();
    if (!loaded()) {
        return;
    }
    m_sim.previousFrame = m_sim.player.presentationFrame();
    m_sim.previousGeneration = m_sim.player.generation();
    // The original plays the asked-for sequence once the current one has ended (at once for
    // the gesture) and, whenever one ends or changes, asks for the next of the cycle.
    m_sim.player.advance(seconds, m_tree->sequences[m_sim.player.sequence()].repeats);
    const bool done = m_sim.player.finished();
    const u32 wanted = sequenceFor(m_sim.index);
    const bool different = wanted != m_sim.player.sequence();
    bool restarted = false;
    if (m_sim.cutIn ? (done || different) : (done && different)) {
        m_sim.player.start(m_tree->sequences[wanted], wanted);
        restarted = true;
    }
    if (restarted || done) {
        m_sim.index = m_sim.index + 1 > 2 ? 0 : m_sim.index + 1;
        m_sim.cutIn = false;
    }
    m_sim.pose.evaluate(*m_tree, m_sim.player.sequence(), m_sim.player.frame());
}

void SumnerFigure::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                        f32 frameBlend) const {
    if (!loaded()) {
        return;
    }
    TreePose visualPose;
    const TreePose* pose = &m_sim.pose;
    f32 frame = m_sim.player.frame();
    const f32 blend = frameBlend < 0 || m_sim.presentationAdvanced ? frameBlend : 1.0f;
    if (blend >= 0 && m_sim.player.playing()) {
        frame = m_sim.previousGeneration == m_sim.player.generation()
                    ? portable::lerp(m_sim.previousFrame, m_sim.player.presentationFrame(),
                                     std::clamp(blend, 0.0f, 1.0f))
                    : m_sim.player.presentationFrame();
        visualPose.evaluate(*m_tree, m_sim.player.sequence(), frame, TreePose::Sampling::Smooth);
        pose = &visualPose;
    }
    m_model.setPresentationFrame(m_sim.player.sequence(), frame);
    m_model.draw(device, clip, m_transform, lighting, pose->matrices());
}

} // namespace gdl::game
