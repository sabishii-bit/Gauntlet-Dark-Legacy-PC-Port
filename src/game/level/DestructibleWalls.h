#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"

#include "game/combat/MissileTarget.h"

namespace gdl::game {
/** Damageable level meshes and their authored collision surfaces. Level models/textures
 * and collision outlive this owner; target snapshots borrow its stable surface storage. */
class DestructibleWalls {
public:
    struct WallState {
        s32 health = 0;
        bool shown = true;
        f32 flash = 0;
        s32 blinks = 0;     ///< white blinks still to come, revealing it
        f32 blinkTicks = 0; ///< into the present blink's cycle
        bool white = false; ///< exact last-applied model mask, including cycle boundaries
        bool operator==(const WallState&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const WallState& value) {
            digest.template fields<6>(value);
        }
    };
    struct Wall : WallState {
        TreeModel model;
        Mat4 transform{1};
        Vec3 collisionCentre{0}; ///< authored item offset, for hit/death feedback
        std::vector<CollisionTriangle> surface;
        MissileTarget bounds;
        s32 armor = 0;
        s32 minPlayers = 0;
        s32 object = -1;
    };
    /** Collision is restored independently; this holds only the walls' native values. */
    class LocalSnapshot {
        friend class DestructibleWalls;
        std::vector<WallState> m_walls;
        std::weak_ptr<const u8> m_binding;
        const DestructibleWalls* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr s32 kRevealBlinks = 3;
    static constexpr f32 kBlinkWhite = 30; ///< ticks of a cycle it shows white
    static constexpr f32 kBlinkCycle = 60; ///< past this the next cycle begins
    static constexpr f32 kTicksPerSecond = 60;
    void bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
              TextureSet& textures, WorldCollision& collision);
    void clear();
    void setPlayerCount(s32 players, WorldCollision& collision);
    void update(f32 seconds);
    usize size() const { return m_walls.size(); }
    const Wall& wall(usize index) const { return m_walls.at(index); }
    bool standing(usize index) const;
    MissileTarget target(usize index, s32 id) const;
    /** Remaining health after an accepted hit; zero is the single destruction event. */
    std::optional<s32> strike(usize index, f32 power, WorldCollision& collision, u32 flags = 0);
    /** Shows a standing wall up (fn_8005BA1C's lesser archer perk): three blinks of white,
     * half of each cycle of sixty ticks (fn_800606FC's obstacle flash). */
    void reveal(usize index);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    void showWhite(Wall& wall, bool white);
    std::vector<Wall> m_walls;
    const Texture* m_white = nullptr;
    const WorldLayout* m_layout = nullptr;
    const ModelSet* m_models = nullptr;
    const TextureSet* m_textures = nullptr;
    const WorldCollision* m_collision = nullptr;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
