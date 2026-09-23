#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** What striking a barrel came to. */
struct BreakableStrike {
    enum class Kind : std::uint8_t { Plain, Holding, Exploding, Poison };

    std::size_t index = 0;
    Kind kind = Kind::Plain;
    bool broken = false; ///< its last hit point went; else it only took the blow
    Vec3 position{0.0f, 0.0f, 0.0f};
    std::int32_t contents = -1; ///< the item record a holding barrel gives up, broken
    std::int32_t count = 0;
};

/**
 * A level's barrels, the things of it that blows break: plain ones, ones that hold an item
 * (containers of the barrel kind), ones that blow up and ones full of poison gas. Each
 * stands solid with its record's hit points and armour; a blow takes its power less the
 * armour (never under one) off them, as the original's does, and at none it plays its
 * breaking sequence, no longer in anyone's way, and leaves its staves lying (one that blew
 * up or gassed leaves nothing).
 */
class Breakables {
public:
    static constexpr std::int32_t kBreakable = 10; ///< the item type of barrels and breakable walls
    static constexpr std::int32_t kBarrel = 43;    ///< of either type
    static constexpr std::int32_t kExploding = 44;
    static constexpr std::int32_t kPoison = 45;
    static constexpr std::int32_t kWhole = 0; ///< its sequences, in order
    static constexpr std::int32_t kBreaking = 1;
    static constexpr std::int32_t kBroken = 2;
    static constexpr std::uint32_t kSeedStart = 7919; ///< its random picks' own seed

    /** One barrel. */
    struct Barrel {
        std::int32_t instance = -1;
        BreakableStrike::Kind kind = BreakableStrike::Kind::Plain;
        std::int32_t health = 0;
        std::int32_t armor = 0;
        std::int32_t contents = -1;
        std::int32_t count = 0;
        std::int32_t minPlayers = 0;
        std::int32_t state = kWhole;
        bool shown = true;
        bool gone = false;
        float radius = 1.0f;
        float height = 3.0f;
        ItemFigure figure;
        Obstacle box;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision);
    void clear();
    std::size_t size() const { return m_barrels.size(); }
    const Barrel& barrel(std::size_t index) const { return *m_barrels[index]; }
    void setPlayerCount(std::int32_t players);

    /** Whether a barrel still stands to be hit. */
    bool standing(std::size_t index) const;
    /** The standing barrel a body of `radius` moving from `from` to `to` runs into. */
    std::optional<std::size_t> struckBy(const Vec3& from, const Vec3& to, float radius) const;
    /** The standing barrels within `radius` of `centre`. */
    std::vector<std::size_t> within(const Vec3& centre, float radius) const;
    /** A blow of `power` on a standing barrel; nothing for one that no longer stands. */
    std::optional<BreakableStrike> strike(std::size_t index, float power);

    void update(float seconds);
    /** The boxes of those still in the way. */
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Barrel>> m_barrels;
    std::vector<ItemInfo> m_infos;
    std::uint32_t m_seed = kSeedStart;
};

} // namespace gdl::game
