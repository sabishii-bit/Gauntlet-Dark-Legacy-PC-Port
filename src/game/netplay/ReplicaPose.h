#pragma once

#include "engine/world/TreePose.h"

#include "game/netplay/CombatSnapshot.h"

namespace gdl::game {
/** Poses only: no AnimationPlayer, AI, release events or game clock. A transition
 * uses the last displayed pose when available; after a lost transition/start it
 * samples the current authored pose, not a guessed sequence or replayed action. */
class ReplicaPose {
public:
    static bool accepts(const TreeInfo* tree, const CombatAnimation& animation);
    void bind(const TreeInfo* tree);
    bool rest();
    bool show(const CombatAnimation& animation);
    const TreePose& pose() const { return m_pose; }

private:
    const TreeInfo* m_tree = nullptr;
    u64 m_generation = 0;
    TreePose m_pose;
    TreePose m_from;
};
} // namespace gdl::game
