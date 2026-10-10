#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** Someone who can open a chest: where they stand, how wide they are, and the keys they have
 * to spend. */
struct ChestVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    s32 keys = 0;
    bool xray = false;
    Vec3 step{0.0f, 0.0f, 0.0f}; ///< how far it moved since the last update
};

/** What a chest did this update. */
struct ChestEvent {
    enum class Kind : u8 {
        Unlocked, ///< a visitor's key went into it: spend it and sound the chest
        Refused,  ///< touched without a key
        Opened,   ///< its lid is up: what was in it comes out
        Collected ///< a visitor takes an open gold chest's treasure
    };
    Kind kind = Kind::Unlocked;
    usize chest = 0;
    usize visitor = 0;     ///< who unlocked it (for Opened, who had)
    s32 contents = -1;     ///< Unlocked/Opened: the resolved pickup record, or -1
    s32 gold = 0;          ///< Collected: what a chest of gold pays its visitor
    bool explodes = false; ///< Opened: it was a trapped chest
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's chests, worked the way the original works them: each is a container item whose
 * first parameter names the item record inside it (or a list to pick one from at random). A
 * chest stands shut and solid until someone against it has a key to spend; it then plays its
 * opening with its pickup riding NULL1; its lid up, it makes that pickup collectible, lets a
 * visitor take its gold when it is a chest of gold, or blasts when it is trapped.
 */
class Chests {
public:
    static constexpr s32 kBarrel = 43; ///< container subtypes
    static constexpr s32 kTrappedChest = 44;
    static constexpr s32 kChest = 46;
    static constexpr s32 kGoldChest = 47;
    static constexpr s32 kRandomChest = 48; ///< the silver chest, of the contents' choice list
    static constexpr u32 kLocked = 0x10;    ///< of a record's active type: a key opens it
    static constexpr s32 kShut = 0;         ///< the figure's sequences
    static constexpr s32 kOpening = 1;
    static constexpr s32 kOpen = 2;
    static constexpr s32 kSeedStep = 439; ///< what each random pick moves the seed on by
    static constexpr f32 kCollectedSeconds = 8.0f / 60.0f; ///< ItemTouch's retirement delay
    static constexpr f32 kRefusalSeconds = 2.5f;
    static constexpr s32 kTransmuted = 1;        ///< the subtype a conversion leaves in the record
    static constexpr f32 kWobblePerPower = 3.0f; ///< ticks of rocking a point of magic
    static constexpr f32 kTicksPerSecond = 60.0f;
    static constexpr f32 kRockPitch = 0.0523599f; ///< three degrees
    static constexpr f32 kRockYaw = 0.0698132f;   ///< four degrees

    /** One chest. */
    struct Chest {
        s32 instance = -1;
        s32 info = -1;
        s32 subtype = 0;
        s32 contents = -1; ///< an item record, once any list has been picked from
        s32 count = 0;     ///< how many keys or how much it holds, when its instance says
        bool locked = true;
        s32 state = kShut;
        s32 opener = -1;
        f32 refusalLeft = 0.0f;
        s32 minPlayers = 0;
        bool shown = true;
        s32 held = -1;     ///< the dropped item lying in it, open, until someone takes it
        bool gone = false; ///< emptied, it is no longer there
        bool revealed = false;
        s32 previewContents = -1;
        f32 wobble = 0.0f;  ///< ticks it is still to rock for, shut
        ItemFigure preview; ///< visual only: never a collectible or an RNG draw
        ItemFigure figure;
        Obstacle box;
        struct Floor {
            s32 object = -1;
            Mat4 local{1.0f};
        };
        std::optional<Floor> floor;
        std::optional<f32> collectedSeconds;
    };

    /** Stands a chest at every chest item of the layout (barrels are not chests). */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, ItemArchive* realmItems = nullptr);
    void clear();
    usize size() const { return m_chests.size(); }
    const Chest& chest(usize index) const { return *m_chests[index]; }
    void setPlayerCount(s32 players);
    /** Carries the container and its obstacle with its authored supporting platform. */
    void syncFloors();

    /** Steps the chests under the party; what happened is returned for the game to act on. */
    std::vector<ChestEvent> update(f32 seconds, std::span<const ChestVisitor> party);
    /** Retail do_see_thru: one nearest eligible shut chest per X-Ray wearer,
     * within ten units. Returns the number of newly revealed chests for the cue. */
    usize updateXray(RenderDevice& device, ItemArchive& items, ItemArchive& powerups, f32 seconds,
                     std::span<const ChestVisitor> party, ItemArchive* realmItems = nullptr);
    /** Blocking boxes; open pickup chests are passable until their contents are taken. */
    std::vector<Obstacle> obstacles() const;
    /** Magic on a shut chest (fn_8005C1DC): what is in it becomes `record` and it rocks for
     * three ticks a point of `power`. The subtype the original writes lands in the record
     * every chest of its kind shares, so they all lose their trapped, gold and silver ways. */
    bool transmute(usize index, s32 record, f32 power);
    /** Potion magic's perk on a shut chest's contents (fn_8005BA1C): they become `record`,
     * and with a `figure` named that tree of `items` stands in its place. */
    bool changeContents(usize index, s32 record, RenderDevice& device, const WorldLayout& layout,
                        ItemArchive& items, const WorldCollision* collision,
                        std::string_view figure = {});
    /** What came out of an opened chest lies in it as dropped item number `item`. */
    void hold(usize chest, s32 item);
    /** The open chest `visitor` is against that still holds something (touching it is how
     * what is inside is reached), or -1. */
    s32 holdingTouchedBy(const ChestVisitor& visitor) const;
    /** An emptied chest goes, as the original's does. */
    void remove(usize chest);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr, f32 presentationAlpha = -1.0f) const;
    void capturePresentation();

    /** The item record a container's first parameter leads to: itself, or the pick from a
     * list by the original's rule, which moves `seed` on. */
    static s32 resolveContents(std::span<const ItemInfo> infos, s32 record, usize itemIndex,
                               u32& seed);

private:
    static void rock(Chest& chest, f32 seconds);

    std::vector<std::unique_ptr<Chest>> m_chests;
    std::vector<ItemInfo> m_infos;
    u32 m_seed = 0;
    const WorldCollision* m_collision = nullptr;
};

} // namespace gdl::game
