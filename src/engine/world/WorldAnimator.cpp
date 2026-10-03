#include "engine/world/WorldAnimator.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/world/TreePose.h"

namespace gdl {
namespace {
constexpr f32 kFrameSlack = 0.00001f;
}

void WorldAnimator::bind(const WorldLayout& layout) {
    clear();
    const std::vector<WorldObject>& objects = layout.objects();
    for (const WorldAnimation& animation : layout.animations()) {
        if (animation.object < 0 || static_cast<usize>(animation.object) >= objects.size() ||
            animation.frames <= 0) {
            continue;
        }
        const WorldObject& object = objects[static_cast<usize>(animation.object)];
        const bool reverse = (object.flags & WorldObject::kReverse) != 0;
        const bool once = (object.flags & WorldObject::kOnce) != 0;
        // Both flags together switch the animation off.
        if (reverse && once) {
            continue;
        }
        Track track;
        track.object = animation.object;
        track.frames = animation.frames;
        track.track = animation.track;
        track.origin = object.position;
        track.reverse = reverse;
        track.once = once;
        const auto last = static_cast<f32>(animation.frames - 1);
        track.frame = reverse ? last : std::clamp(animation.start, 0.0f, last);
        m_tracks.push_back(std::move(track));
    }
}

void WorldAnimator::clear() {
    m_tracks.clear();
    m_cycleEvents.clear();
}

std::optional<usize> WorldAnimator::trackOf(s32 object) const {
    for (usize i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].object == object) {
            return i;
        }
    }
    return std::nullopt;
}

void WorldAnimator::hold(s32 object) {
    const auto index = trackOf(object);
    if (!index.has_value()) {
        return;
    }
    Track& track = m_tracks[*index];
    track.held = true;
    track.frame = 0.0f;
    track.once = true;
    track.reverse = true;
    track.finished = true;
}

void WorldAnimator::fire(s32 object, bool open, bool atOnce) {
    const auto index = trackOf(object);
    if (!index.has_value()) {
        return;
    }
    Track& track = m_tracks[*index];
    const auto last = static_cast<f32>(track.frames - 1);
    track.held = false;
    track.once = true;
    track.reverse = !open;
    track.finished = false;
    if (atOnce) {
        track.frame = open ? last : 0.0f;
        track.finished = true;
    }
}

void WorldAnimator::cycle(s32 object, bool active) {
    const auto index = trackOf(object);
    if (!index.has_value()) {
        return;
    }
    Track& track = m_tracks[*index];
    track.held = false;
    track.once = !active;
    track.reverse = false;
    track.finished = false;
}

void WorldAnimator::pose(const Track& track, WorldScene& scene) {
    const NodePose pose = TreePose::sample(track.track, track.frame);
    scene.setObjectTransform(static_cast<usize>(track.object),
                             TreePose::localMatrix(pose, track.origin));
}

void WorldAnimator::apply(WorldScene& scene) const {
    for (const Track& track : m_tracks) {
        pose(track, scene);
    }
}

void WorldAnimator::step(f32 seconds, WorldScene& scene, bool pauseLoops) {
    m_cycleEvents.clear();
    const f32 advance = seconds * kFramesPerSecond;
    for (Track& track : m_tracks) {
        pose(track, scene);
        if (seconds <= 0 || track.finished || (pauseLoops && !track.once && !track.reverse)) {
            continue;
        }
        // Retail reveals a spent cart in its first two 30-Hz frames. Emit the
        // restart before advancing as well, so a slow rendered frame cannot jump
        // over that window and leave the cart hidden for every later cycle.
        const bool restarted = !track.reverse && track.frame == 0.0f;
        if (restarted) {
            m_cycleEvents.push_back({track.object, false});
        }
        const auto last = static_cast<f32>(track.frames - 1);
        if (track.reverse) {
            track.frame -= advance;
            if (track.frame <= 0.0f) {
                track.frame = 0.0f;
                track.finished = true;
            } else if (std::floor(track.frame) >= last - 1) {
                m_cycleEvents.push_back({track.object, false});
            }
            continue;
        }
        track.frame += advance;
        if (track.once) {
            if (track.frame >= last) {
                track.frame = last;
                track.finished = true;
            } else if (!restarted && std::floor(track.frame) <= 1) {
                m_cycleEvents.push_back({track.object, false});
            }
        } else if (track.frame + kFrameSlack >= last) {
            // A looping world track's final key closes the cycle; retain time past
            // that key rather than lengthening every loop by a caller-dependent amount.
            track.frame = last > 0.0f && track.frame >= last ? std::fmod(track.frame, last) : 0.0f;
            m_cycleEvents.push_back({track.object, true});
        } else if (!restarted && std::floor(track.frame) <= 1) {
            m_cycleEvents.push_back({track.object, false});
        }
    }
}

} // namespace gdl
