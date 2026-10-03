#pragma once

#include <string_view>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

#include "game/world/FallingPiece.h"

namespace gdl::game {
/** A piece giving way: where it stood and the sound it makes, which the caller plays. */
struct FallingCue {
    Vec3 position{0.0f};
    std::string_view sound; ///< empty where the realm has none
};

/**
 * The scenery of a level that gives way: rocks that fall or sink from under a body that
 * brushes them, leaves that drift down, and rocks that a shot or a blow brings down. They
 * are item models of the level's own archive, never in anyone's way, never a floor, and
 * once started they fall as `FallingPiece` has them. Borrows level meshes and textures;
 * clear before their archives are released.
 */
class FallingScenery {
public:
    static constexpr s32 kObstacle = 10;
    static constexpr s32 kFallAway = 40;                 ///< brushed, it falls
    static constexpr s32 kLeafFall = 49;                 ///< brushed, it drifts down
    static constexpr s32 kShootFall = 52;                ///< only a shot or a blow brings it down
    static constexpr s32 kRockSink = 53;                 ///< brushed, it sinks, hardly turning
    static constexpr f32 kSoundVolume = 224.0f / 255.0f; ///< sndFxPlay3DAtten's level

    struct Piece {
        TreeModel model;
        FallingPiece motion;
        FallingProfile profile;
        s32 subtype = 0;
        s32 minPlayers = 0;
        f32 radius = 0.0f;
        f32 height = 0.0f;
        Vec3 centre{0.0f}; ///< the record's collision offset from where it stands
        bool shown = true;
        bool started = false;
    };

    /** Binds every falling piece of `layout` from the level's meshes; `levelName` (such as
     * F1) picks the realm's breaking sounds. */
    void bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
              TextureSet& textures, std::string_view levelName);
    void clear();
    void setPlayerCount(s32 players);
    /** A body of `radius` standing at `position` brushing the pieces that give way to a
     * touch: those it is within (the cylinder of the record grown by the radius) start,
     * once, and give their cue. */
    std::vector<FallingCue> touch(const Vec3& position, f32 radius);
    /** A shot or a blow of `radius` at `position` (a missile in flight, a strike, a swing)
     * bringing down the pieces only those start, the same way. */
    std::vector<FallingCue> shoot(const Vec3& position, f32 radius);
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f) const;
    usize size() const { return m_pieces.size(); }
    const Piece& piece(usize index) const { return m_pieces.at(index); }

    /** The realm's sound for a rock or limb giving way (fn_8009D91C's table by the level's
     * realm letter, with its two level overrides); empty where the realm has none. */
    static std::string_view breakSoundOf(std::string_view levelName);
    /** The realm's sound for a leaf giving way (fn_8009D8CC's table); empty for most. */
    static std::string_view leafSoundOf(std::string_view levelName);

private:
    static bool within(const Piece& piece, const Vec3& position, f32 radius);
    std::vector<FallingCue> start(const Vec3& position, f32 radius, bool shot);

    std::vector<Piece> m_pieces;
    std::string_view m_breakSound;
    std::string_view m_leafSound;
    f32 m_remainder = 0.0f;
    f32 m_updateSeconds = 0.0f;
    f32 m_bottom = 0.0f;
};
} // namespace gdl::game
