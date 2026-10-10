#include "game/netplay/ReplicaPose.h"

namespace gdl::game {
bool ReplicaPose::accepts(const TreeInfo* tree, const CombatAnimation& animation) {
    if (tree == nullptr || !animation.valid() || animation.generation == 0 ||
        animation.sequence >= tree->sequences.size()) {
        return false;
    }
    const auto frames = tree->sequences[animation.sequence].frames;
    // Some native player release entries (ARC THROW1) have zero duration.
    // The host still evaluates their frame-zero tracks during the transition;
    // these are not missing sequences and must not disconnect a remote view.
    return frames >= 0 && animation.frame <= static_cast<f32>(frames > 0 ? frames - 1 : 0);
}
void ReplicaPose::bind(const TreeInfo* tree) {
    m_tree = tree;
    m_generation = 0;
    m_pose = {};
    m_from = {};
}
bool ReplicaPose::rest() {
    m_generation = 0;
    m_from = {};
    if (m_tree == nullptr) {
        m_pose = {};
        return false;
    }
    m_pose.rest(*m_tree);
    return true;
}
bool ReplicaPose::show(const CombatAnimation& animation) {
    if (!accepts(m_tree, animation)) {
        m_pose = {};
        m_from = {};
        m_generation = 0;
        return false;
    }
    if (m_generation != animation.generation) {
        m_from = m_pose;
        m_generation = animation.generation;
    }
    m_pose.evaluate(*m_tree, animation.sequence, animation.frame, false, true);
    if (animation.transition < 1 && m_from.posed()) {
        m_pose.blend(m_from, animation.transition);
    }
    return true;
}
} // namespace gdl::game
