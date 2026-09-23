#include "game/world/SumnerFigure.h"

#include <algorithm>
#include <cstddef>

#include "engine/core/Log.h"

namespace gdl::game {

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
    for (std::size_t i = 0; i < kSequences.size(); ++i) {
        const auto sequence = m_tree->findSequence(kSequences[i]);
        m_sequences[i] = sequence.has_value() ? static_cast<int>(*sequence) : -1;
    }
    // Like the start markers, the lookout's heading points the way he came: a half turn
    // round faces him at the party.
    m_position = lookout->position;
    m_yaw = lookout->rotation.y + kPi;
    m_transform =
        glm::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
    m_index = 0;
    m_cutIn = false;
    const unsigned int first = sequenceFor(0);
    m_player.start(m_tree->sequences[first], first);
    m_pose.evaluate(*m_tree, first, 0.0f);
    return true;
}

void SumnerFigure::clear() {
    m_model.clear();
    m_tree = nullptr;
    m_sequences.fill(-1);
    m_index = 0;
    m_cutIn = false;
    m_player.stop();
}

/** The sequence an index asks for, the stance standing in for any the tree lacks. */
unsigned int SumnerFigure::sequenceFor(int index) const {
    int sequence = -1;
    if (index >= 0 && static_cast<std::size_t>(index) < m_sequences.size()) {
        sequence = m_sequences[static_cast<std::size_t>(index)];
    }
    if (sequence < 0) {
        sequence = std::max(m_sequences[0], 0);
    }
    return static_cast<unsigned int>(sequence);
}

void SumnerFigure::play(int index) {
    if (!loaded()) {
        return;
    }
    m_index = index;
    m_cutIn = true;
}

void SumnerFigure::update(float seconds) {
    if (!loaded()) {
        return;
    }
    // The original plays the asked-for sequence once the current one has ended (at once for
    // the gesture) and, whenever one ends or changes, asks for the next of the cycle.
    m_player.advance(seconds, m_tree->sequences[m_player.sequence()].repeats);
    const bool done = m_player.finished();
    const unsigned int wanted = sequenceFor(m_index);
    const bool different = wanted != m_player.sequence();
    bool restarted = false;
    if (m_cutIn ? (done || different) : (done && different)) {
        m_player.start(m_tree->sequences[wanted], wanted);
        restarted = true;
    }
    if (restarted || done) {
        m_index = m_index + 1 > 2 ? 0 : m_index + 1;
        m_cutIn = false;
    }
    m_pose.evaluate(*m_tree, m_player.sequence(), m_player.frame());
}

void SumnerFigure::draw(RenderDevice& device, const Mat4& clip,
                        const WorldLighting& lighting) const {
    if (!loaded()) {
        return;
    }
    m_model.draw(device, clip, m_transform, lighting, m_pose.matrices());
}

} // namespace gdl::game
