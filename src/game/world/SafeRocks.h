#pragma once

#include <array>
#include <memory>
#include <vector>

#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** The boss arenas' type-10/subtype-41 cover. These are static models, not animation
 * trees: name + health tier, with the original L1 / L1ROOT lookup fallbacks. */
class SafeRocks {
public:
    static constexpr s32 kItemType = 10;
    static constexpr s32 kSubtype = 41;
    static constexpr s32 kWhole = 3;

    struct Rock {
        s32 instance = -1;
        s32 health = 0;
        s32 baseHealth = 0;
        s32 armor = 0;
        s32 tier = 0;
        s32 minPlayers = 0;
        bool shown = true;
        Vec3 position{0.0f};
        Obstacle obstacle;
        Mat4 placement{1.0f};
        std::array<TreeModel, 4> models;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items);
    void clear();
    void setPlayerCount(s32 players);
    usize size() const { return m_rocks.size(); }
    const Rock& rock(usize index) const { return *m_rocks[index]; }
    bool standing(usize index) const;
    /** Armour reduces damage, with a minimum of one. True only on the destroying blow. */
    bool strike(usize index, f32 power);
    /** Restore all three health tiers, as the boss's reactivation does. */
    void activate(usize index);
    std::vector<Obstacle> obstacles() const;
    bool blocksBreath(const Vec3& from, const Vec3& to) const;
    bool blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Rock>> m_rocks;
};

} // namespace gdl::game
