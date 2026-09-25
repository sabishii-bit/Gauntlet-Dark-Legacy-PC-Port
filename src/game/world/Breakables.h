#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** What striking a barrel came to. */
struct BreakableStrike {
    enum class Kind : u8 { Plain, Holding, Exploding, Poison };

    usize index = 0;
    Kind kind = Kind::Plain;
    bool broken = false; ///< its last hit point went; else it only took the blow
    Vec3 position{0.0f, 0.0f, 0.0f};
    s32 contents = -1; ///< the item record a holding barrel gives up, broken
    s32 count = 0;
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
    static constexpr s32 kBreakable = 10; ///< the item type of barrels and breakable walls
    static constexpr s32 kBarrel = 43;    ///< of either type
    static constexpr s32 kExploding = 44;
    static constexpr s32 kPoison = 45;
    static constexpr s32 kWhole = 0; ///< its sequences, in order
    static constexpr s32 kBreaking = 1;
    static constexpr s32 kBroken = 2;
    static constexpr u32 kSeedStart = 7919; ///< its random picks' own seed

    /** One barrel. */
    struct Barrel {
        s32 instance = -1;
        BreakableStrike::Kind kind = BreakableStrike::Kind::Plain;
        s32 health = 0;
        s32 armor = 0;
        s32 contents = -1;
        s32 count = 0;
        s32 minPlayers = 0;
        s32 state = kWhole;
        bool shown = true;
        bool gone = false;
        f32 radius = 1.0f;
        f32 height = 3.0f;
        ItemFigure figure;
        Obstacle box;
    };

    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, ItemArchive* realmItems = nullptr);
    void clear();
    usize size() const { return m_barrels.size(); }
    const Barrel& barrel(usize index) const { return *m_barrels[index]; }
    void setPlayerCount(s32 players);

    /** Whether a barrel still stands to be hit. */
    bool standing(usize index) const;
    /** The standing barrel a body of `radius` moving from `from` to `to` runs into. */
    std::optional<usize> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    /** The standing barrels within `radius` of `centre`. */
    std::vector<usize> within(const Vec3& centre, f32 radius) const;
    /** A blow of `power` on a standing barrel; nothing for one that no longer stands. */
    std::optional<BreakableStrike> strike(usize index, f32 power);

    void update(f32 seconds);
    /** The boxes of those still in the way. */
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    std::vector<std::unique_ptr<Barrel>> m_barrels;
    std::vector<ItemInfo> m_infos;
    u32 m_seed = kSeedStart;
};

} // namespace gdl::game
