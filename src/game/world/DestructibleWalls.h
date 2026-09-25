#pragma once

#include <optional>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"

#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** Damageable level meshes and their authored collision surfaces. Level models/textures
 * and collision outlive this owner; target snapshots borrow its stable surface storage. */
class DestructibleWalls {
public:
    struct Wall {
        TreeModel model;
        Mat4 transform{1};
        std::vector<CollisionTriangle> surface;
        MissileTarget bounds;
        s32 health = 0;
        s32 armor = 0;
        s32 minPlayers = 0;
        s32 object = -1;
        bool shown = true;
        f32 flash = 0;
    };
    void bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
              TextureSet& textures, WorldCollision& collision);
    void clear() { m_walls.clear(); }
    void setPlayerCount(s32 players, WorldCollision& collision);
    void update(f32 seconds);
    usize size() const { return m_walls.size(); }
    const Wall& wall(usize index) const { return m_walls.at(index); }
    bool standing(usize index) const;
    MissileTarget target(usize index, s32 id) const;
    /** Remaining health after an accepted hit; zero is the single destruction event. */
    std::optional<s32> strike(usize index, f32 power, WorldCollision& collision, u32 flags = 0);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<Wall> m_walls;
    const Texture* m_white = nullptr;
};
} // namespace gdl::game
