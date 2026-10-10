#pragma once

#include "game/netplay/CombatSnapshot.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
/** Per-seat draw resources. Never creates a familiar, runs its attack logic or
 * loads an asset while receiving a snapshot. Borrowed archives outlive this catalog. */
class CompanionResources {
public:
    void bindEarned(const PlayerFigure& figure);
    bool bindPowerups(RenderDevice& device, ItemArchive& powerups, ItemArchive* weapons);
    bool accepts(usize slot, const CompanionState& state) const;
    void draw(RenderDevice& device, usize slot, const CompanionState& state, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera, TreeModel::Pass pass) const;

private:
    struct Entry {
        const TreeInfo* tree = nullptr;
        mutable TreeModel model;
        TextureAnimator textures;
    };
    const Entry* entry(usize slot, u32 form) const;
    u32 m_earnedForm = 0;
    Entry m_earned;
    std::array<Entry, 7> m_powerups;
};
} // namespace gdl::game
