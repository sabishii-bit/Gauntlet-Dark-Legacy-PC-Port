#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/math/Math.h"
#include "engine/world/WorldScene.h"

namespace gdl {

/**
 * A level's keyframed objects played the way the original moves them: thirty frames a
 * second whatever the frame rate, each object posed by its track and set into the scene,
 * looping at its last frame unless its flags play it once, forwards or backwards.
 */
class WorldAnimator {
public:
    static constexpr float kFramesPerSecond = 30.0f;

    /** Takes the layout's animations, each starting where its record says. */
    void bind(const WorldLayout& layout);
    void clear();
    std::size_t size() const { return m_tracks.size(); }
    float frame(std::size_t index) const { return m_tracks[index].frame; }
    int object(std::size_t index) const { return m_tracks[index].object; }
    bool finished(std::size_t index) const { return m_tracks[index].finished; }

    /** The track of an object, when the layout animates it. */
    std::optional<std::size_t> trackOf(int object) const;
    /** Holds an object at its first frame until fired, as a trigger's target waits. */
    void hold(int object);
    /** Plays an object's animation once from where it stands: forwards to open, backwards
     * to close; `atOnce` jumps straight to the end. */
    void fire(int object, bool open, bool atOnce = false);
    bool held(std::size_t index) const { return m_tracks[index].held; }

    /** Poses every object at its current frame. */
    void apply(WorldScene& scene) const;
    /** Poses every object, then moves `seconds` on. */
    void step(float seconds, WorldScene& scene);

private:
    struct Track {
        int object = -1;
        int frames = 0;
        TrackInfo track;
        Vec3 origin{0.0f, 0.0f, 0.0f}; ///< the object's rest offset from its parent
        float frame = 0.0f;
        bool reverse = false; ///< plays backwards once
        bool once = false;    ///< plays forwards once
        bool finished = false;
        bool held = false; ///< waiting at its first frame for a trigger
    };

    static void pose(const Track& track, WorldScene& scene);

    std::vector<Track> m_tracks;
};

} // namespace gdl
