#pragma once

#include "game/enemies/Bosses.h"
#include "game/enemies/Critters.h"
#include "game/netplay/CombatSnapshot.h"

namespace gdl::game {
/** Copies render resources, never actors. The loading stage supplies a complete,
 * identically ordered family roster on both peers; packets cannot extend it. */
class FighterResources {
public:
    bool bind(std::span<const CombatantAssets* const> stocks);
    bool bind(const Critters& critters, const Bosses& bosses);
    std::optional<FighterMeshState> capture(const CombatantVisual& visual, u32 actor,
                                            u64 incarnation) const;
    bool accepts(const FighterMeshState& state) const;
    void draw(RenderDevice& device, const FighterMeshState& state, const Mat4& clip,
              const WorldLighting& lighting, const Texture* flash, const Texture* frozen,
              TreeModel::Pass pass = TreeModel::Pass::All);

private:
    struct Entry {
        const CombatantAssets* stock = nullptr;
        const TreeModel* source = nullptr;
        const TreeInfo* tree = nullptr;
        bool textured = false;
        TreeModel model;
        TextureAnimator textures;
        std::vector<const Texture*> skins;
    };
    std::vector<Entry> m_entries;
};
class FighterCapture {
public:
    static bool append(CombatSnapshot& snapshot, const FighterResources& resources,
                       const Critters& critters, const Bosses& bosses);
};
class ReplicaFighters {
public:
    bool begin(u64 epoch);
    void clear();
    bool show(const CombatSnapshot& snapshot, const FighterResources& resources);
    void draw(RenderDevice& device, FighterResources& resources, const Mat4& clip,
              const WorldLighting& lighting, const Texture* flash, const Texture* frozen,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    usize count() const { return m_meshes.size(); }

private:
    u64 m_epoch = 0;
    std::optional<u64> m_tick;
    std::vector<FighterMeshState> m_meshes;
};
} // namespace gdl::game
