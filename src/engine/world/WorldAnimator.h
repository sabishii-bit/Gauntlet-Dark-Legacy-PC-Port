#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"
#include "engine/math/Math.h"
#include "engine/world/WorldScene.h"

namespace gdl {
class NativeStateDigest;

/**
 * A level's keyframed objects played the way the original moves them: thirty frames a
 * second whatever the frame rate, each object posed by its track and set into the scene,
 * looping at its last frame unless its flags play it once, forwards or backwards.
 */
class WorldAnimator {
public:
    static constexpr f32 kFramesPerSecond = 30.0f;
    /** The most frames a seam may add past the last key before a cursor is corrupt. */
    static constexpr f32 kMostSeamFrames = 8.0f;

    /** Takes the layout's animations, each starting where its record says. */
    void bind(const WorldLayout& layout);
    void clear();
    usize size() const { return m_tracks.size(); }
    f32 frame(usize index) const { return m_tracks[index].frame; }
    s32 object(usize index) const { return m_tracks[index].object; }
    bool finished(usize index) const { return m_tracks[index].finished; }

    /** The track of an object, when the layout animates it. */
    std::optional<usize> trackOf(s32 object) const;
    /** Holds an object at its first frame until fired, as a trigger's target waits. */
    void hold(s32 object);
    /** Plays an object's animation once from where it stands: forwards to open, backwards
     * to close; `atOnce` jumps straight to the end. */
    void fire(s32 object, bool open, bool atOnce = false);
    /** Loop while requested, then finish the current forward cycle on release. */
    void cycle(s32 object, bool active);
    bool held(usize index) const { return m_tracks[index].held; }

    /** Mutable cursor only; authored keys remain in the bound layout. frame is
     * the next pose to sample, since step poses before advancing its clock; a continuous
     * turn runs on past its last key through the closing seam. */
    struct Cursor {
        s32 object = -1;
        s32 frames = 0;
        /** A continuous turn whose keys stop short of closing it runs this many frames past
         * its last key, at the authored rate, before the cycle closes; none otherwise. */
        f32 seam = 0;
        f32 frame = 0;
        bool reverse = false;
        bool once = false;
        bool finished = false;
        bool held = false;
        bool presentationCut = true;
        bool valid() const;
        bool operator==(const Cursor&) const = default;
        template <class Digest> friend void visitNativeState(Digest& digest, const Cursor& value) {
            digest.template fields<9>(value);
        }
    };
    std::vector<Cursor> checkpoint() const;
    /** Restores every cursor atomically against the bound track roster. Does not
     * pose geometry, fire triggers or repeat prior cycle events. The caller keeps
     * the corresponding scene geometry until the next step samples these cursors. */
    bool restore(std::span<const Cursor> cursors);

    /** Poses every object at its current frame. */
    void apply(WorldScene& scene) const;
    /** Poses every object, then moves `seconds` on. */
    void step(f32 seconds, WorldScene& scene, bool pauseLoops = false);
    struct CycleEvent {
        s32 object = -1;
        bool wrapped = false; ///< loop endpoint, otherwise the first frames of a new pass
        bool operator==(const CycleEvent&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const CycleEvent& value) {
            digest.template fields<2>(value);
        }
    };
    std::span<const CycleEvent> cycleEvents() const { return m_cycleEvents; }
    class LocalSnapshot {
        friend class WorldAnimator;
        std::vector<Cursor> m_cursors;
        std::vector<CycleEvent> m_cycles;
        const WorldAnimator* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    const CanonicalDigestSink::Digest& trackDigest() const;
    struct Track : Cursor {
        TrackInfo track;
        Vec3 origin{0.0f}; ///< immutable rest offset from the parent
        /** A continuous turn's rotation channel and the signed radians a frame it turns at
         * through its seam; no channel for any other track. */
        s32 turnChannel = -1;
        f32 turnRate = 0.0f;
    };

    static void pose(const Track& track, WorldScene& scene);

    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    std::vector<Track> m_tracks;
    // Authored keys/origins change only on bind/clear. Live cursors and pending
    // cycle events are still visited directly on every hash and rewind.
    mutable std::optional<CanonicalDigestSink::Digest> m_trackDigest;
    std::vector<CycleEvent> m_cycleEvents;
};

} // namespace gdl
