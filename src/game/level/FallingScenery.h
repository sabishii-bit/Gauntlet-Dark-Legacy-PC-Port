#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldLighting.h"

#include "game/level/FallingPiece.h"

namespace gdl {
class NativeStateDigest;
}

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
    static constexpr s32 kObstacle = ItemInfo::kObstacle;
    static constexpr s32 kFallAway = 40;                 ///< brushed, it falls
    static constexpr s32 kLeafFall = 49;                 ///< brushed, it drifts down
    static constexpr s32 kShootFall = 52;                ///< only a shot or a blow brings it down
    static constexpr s32 kRockSink = 53;                 ///< brushed, it sinks, hardly turning
    static constexpr f32 kSoundVolume = 224.0f / 255.0f; ///< sndFxPlay3DAtten's level

    struct PieceState {
        FallingPiece motion;
        FallingProfile profile;
        s32 subtype = 0;
        s32 minPlayers = 0;
        f32 radius = 0.0f;
        f32 height = 0.0f;
        Vec3 centre{0.0f}; ///< the record's collision offset from where it stands
        bool shown = true;
        bool started = false;
        bool operator==(const PieceState&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const PieceState& value) {
            digest.template fields<9>(value);
        }
    };
    struct Piece : PieceState {
        TreeModel model;
    };
    struct Clock {
        f32 remainder = 0;
        f32 updateSeconds = 0;
        bool operator==(const Clock&) const = default;
        template <class Digest> friend void visitNativeState(Digest& digest, const Clock& value) {
            digest.template fields<2>(value);
        }
    };
    class LocalSnapshot {
        friend class FallingScenery;
        std::vector<PieceState> m_pieces;
        Clock m_clock;
        std::weak_ptr<const u8> m_binding;
        const FallingScenery* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

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
    /** Exact read-only admission for shoot(), including already-started and
     * party-hidden pieces. Native missiles query surviving endpoints, not a sweep. */
    bool wouldShoot(const Vec3& position, f32 radius) const;
    void update(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              f32 presentationAlpha = -1.0f) const;
    usize size() const { return m_pieces.size(); }
    const Piece& piece(usize index) const { return m_pieces.at(index); }
    /** Native 30 Hz presentation clock, copied alongside the pieces when a
     * retained picture must outlive a simulation rewind. */
    f32 frameRemainder() const { return m_clock.remainder; }
    f32 updateSeconds() const { return m_clock.updateSeconds; }

    /** The realm's sound for a rock or limb giving way (fn_8009D91C's table by the level's
     * realm letter, with its two level overrides); empty where the realm has none. */
    static std::string_view breakSoundOf(std::string_view levelName);
    /** The realm's sound for a leaf giving way (fn_8009D8CC's table); empty for most. */
    static std::string_view leafSoundOf(std::string_view levelName);

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    static bool within(const Piece& piece, const Vec3& position, f32 radius);
    static bool canStart(const Piece& piece, const Vec3& position, f32 radius, bool shot);
    std::vector<FallingCue> start(const Vec3& position, f32 radius, bool shot);

    std::vector<Piece> m_pieces;
    std::string_view m_breakSound;
    std::string_view m_leafSound;
    Clock m_clock;
    f32 m_bottom = 0.0f;
    const WorldLayout* m_layout = nullptr;
    const ModelSet* m_models = nullptr;
    const TextureSet* m_textures = nullptr;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
