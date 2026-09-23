#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** Someone who can open a chest: where they stand, how wide they are, and the keys they have
 * to spend. */
struct ChestVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.75f;
    std::int32_t keys = 0;
};

/** What a chest did this update. */
struct ChestEvent {
    enum class Kind : std::uint8_t {
        Unlocked, ///< a visitor's key went into it: spend it and sound the chest
        Refused,  ///< touched without a key
        Opened    ///< its lid is up: what was in it comes out
    };
    Kind kind = Kind::Unlocked;
    std::size_t chest = 0;
    std::size_t visitor = 0;    ///< who unlocked it (for Opened, who had)
    std::int32_t contents = -1; ///< Opened: the item record that was inside, or -1
    std::int32_t gold = 0;      ///< Opened: what a chest of gold pays its opener
    bool explodes = false;      ///< Opened: it was a trapped chest
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's chests, worked the way the original works them: each is a container item whose
 * first parameter names the item record inside it (or a list to pick one from at random). A
 * chest stands shut and solid until someone against it has a key to spend; it then plays its
 * opening and, its lid up, gives up what it held: a pickup dropped where it stands, gold
 * paid straight to its opener when it is a chest of gold, or a blast when it is trapped.
 */
class Chests {
public:
    static constexpr std::int32_t kBarrel = 43; ///< container subtypes
    static constexpr std::int32_t kTrappedChest = 44;
    static constexpr std::int32_t kChest = 46;
    static constexpr std::int32_t kGoldChest = 47;
    static constexpr std::uint32_t kLocked = 0x10; ///< of a record's active type: a key opens it
    static constexpr std::int32_t kShut = 0;       ///< the figure's sequences
    static constexpr std::int32_t kOpening = 1;
    static constexpr std::int32_t kOpen = 2;
    static constexpr std::int32_t kSeedStep = 439; ///< what each random pick moves the seed on by
    static constexpr float kRefusalSeconds = 2.5f;

    /** One chest. */
    struct Chest {
        std::int32_t instance = -1;
        std::int32_t info = -1;
        std::int32_t subtype = 0;
        std::int32_t contents = -1; ///< an item record, once any list has been picked from
        std::int32_t count = 0;     ///< how many keys or how much it holds, when its instance says
        bool locked = true;
        std::int32_t state = kShut;
        std::int32_t opener = -1;
        float refusalLeft = 0.0f;
        std::int32_t minPlayers = 0;
        bool shown = true;
        std::int32_t held = -1; ///< the dropped item lying in it, open, until someone takes it
        bool gone = false;      ///< emptied, it is no longer there
        ItemFigure figure;
        Obstacle box;
    };

    /** Stands a chest at every chest item of the layout (barrels are not chests). */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision);
    void clear();
    std::size_t size() const { return m_chests.size(); }
    const Chest& chest(std::size_t index) const { return *m_chests[index]; }
    void setPlayerCount(std::int32_t players);

    /** Steps the chests under the party; what happened is returned for the game to act on. */
    std::vector<ChestEvent> update(float seconds, std::span<const ChestVisitor> party);
    /** The boxes of the chests in sight, which nothing walks through. */
    std::vector<Obstacle> obstacles() const;
    /** What came out of an opened chest lies in it as dropped item number `item`. */
    void hold(std::size_t chest, std::int32_t item);
    /** The open chest `visitor` is against that still holds something (touching it is how
     * what is inside is reached), or -1. */
    std::int32_t holdingTouchedBy(const ChestVisitor& visitor) const;
    /** An emptied chest goes, as the original's does. */
    void remove(std::size_t chest);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    /** The item record a container's first parameter leads to: itself, or the pick from a
     * list by the original's rule, which moves `seed` on. */
    static std::int32_t resolveContents(std::span<const ItemInfo> infos, std::int32_t record,
                                        std::size_t itemIndex, std::uint32_t& seed);

private:
    std::vector<std::unique_ptr<Chest>> m_chests;
    std::vector<ItemInfo> m_infos;
    std::uint32_t m_seed = 0;
};

} // namespace gdl::game
