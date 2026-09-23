#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** The boss arenas' type-10/subtype-41 cover. These are static models, not animation
 * trees: name + health tier, with the original L1 / L1ROOT lookup fallbacks. */
class SafeRocks {
public:
    static constexpr int kItemType = 10;
    static constexpr int kSubtype = 41;
    static constexpr int kWhole = 3;

    struct Rock {
        int instance = -1;
        int health = 0;
        int baseHealth = 0;
        int armor = 0;
        int tier = 0;
        int minPlayers = 0;
        bool shown = true;
        Vec3 position{0.0f};
        Obstacle obstacle;
        Mat4 placement{1.0f};
        std::array<TreeModel, 4> models;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items);
    void clear();
    void setPlayerCount(int players);
    std::size_t size() const { return m_rocks.size(); }
    const Rock& rock(std::size_t index) const { return *m_rocks[index]; }
    bool standing(std::size_t index) const;
    /** Armour reduces damage, with a minimum of one. True only on the destroying blow. */
    bool strike(std::size_t index, float power);
    /** Restore all three health tiers, as the boss's reactivation does. */
    void activate(std::size_t index);
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Rock>> m_rocks;
};

} // namespace gdl::game
