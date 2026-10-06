#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldScene.h"

namespace gdl::game {

/** How the music goes over to an area: a zone's third parameter word (the original's
 * sMusicSubState). */
enum class MusicSwitch : u8 {
    AtPartEnd = 0, ///< once the part playing ends, its loop point included
    Faded = 1,     ///< the music fades out first and the new area rises from silence
    AtOnce = 2,    ///< cut over at the volume of the moment
};

/** A sound item that names a music area: a sphere that asks for that area's stream. */
struct MusicZone {
    s32 instance = -1;
    s32 parent = -1;
    s32 minPlayers = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    s32 area = 0; ///< the stream's index in the realm's audio record, from nought
    MusicSwitch how = MusicSwitch::AtPartEnd;
};

/** What the zones ask the music for. */
struct MusicCue {
    s32 area = 0;
    MusicSwitch how = MusicSwitch::AtPartEnd;
};

/**
 * The level's music areas: its sound items (type 13) whose second parameter word names an
 * area, from one. Each update the nearest listener's distance to every zone picks the
 * highest area whose radius holds it, and that is asked for when it is not the area asked for
 * already; outside every zone nothing changes, so a crossing is reported once.
 */
class MusicAreas {
public:
    static constexpr s32 kSoundItem = 13;
    static constexpr s32 kNoArea = -1;

    /** The area a sound item's parameters name, from one; nought and under is no zone. */
    static s32 areaOf(const ItemInstance& instance);
    static MusicSwitch switchOf(const ItemInstance& instance);

    /** Takes every sound item naming an area; false when there is none. */
    bool bind(const WorldLayout& layout, const WorldScene* world = nullptr);
    /** ItemVisible counts joined slots, not the number of listeners in range. */
    void setPlayerCount(s32 count) { m_players = count; }
    void clear();
    /** The highest area whose zone holds the nearest listener, with the zone's way over;
     * nothing outside every zone or without a listener. */
    std::optional<MusicCue> pick(std::span<const Vec3> listeners,
                                 const WorldScene* world = nullptr) const;
    /** The pick when it is an area other than `current`, the one asked for already. */
    std::optional<MusicCue> update(std::span<const Vec3> listeners, s32 current,
                                   const WorldScene* world = nullptr) const;

    usize size() const { return m_zones.size(); }
    const MusicZone& zone(usize index) const { return m_zones[index]; }

private:
    std::vector<MusicZone> m_zones;
    s32 m_players = 1;
};

} // namespace gdl::game
