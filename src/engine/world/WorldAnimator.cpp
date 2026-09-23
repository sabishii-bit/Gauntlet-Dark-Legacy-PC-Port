#include "engine/world/WorldAnimator.h"

#include <algorithm>
#include <cstddef>

#include "engine/world/TreePose.h"

namespace gdl {

void WorldAnimator::bind(const WorldLayout& layout) {
    clear();
    const std::vector<WorldObject>& objects = layout.objects();
    for (const WorldAnimation& animation : layout.animations()) {
        if (animation.object < 0 || static_cast<std::size_t>(animation.object) >= objects.size() ||
            animation.frames <= 0) {
            continue;
        }
        const WorldObject& object = objects[static_cast<std::size_t>(animation.object)];
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
        const auto last = static_cast<float>(animation.frames - 1);
        track.frame = reverse ? last : std::clamp(animation.start, 0.0f, last);
        m_tracks.push_back(std::move(track));
    }
}

void WorldAnimator::clear() {
    m_tracks.clear();
}

std::optional<std::size_t> WorldAnimator::trackOf(int object) const {
    for (std::size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].object == object) {
            return i;
        }
    }
    return std::nullopt;
}

void WorldAnimator::hold(int object) {
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

void WorldAnimator::fire(int object, bool open, bool atOnce) {
    const auto index = trackOf(object);
    if (!index.has_value()) {
        return;
    }
    Track& track = m_tracks[*index];
    const auto last = static_cast<float>(track.frames - 1);
    track.held = false;
    track.once = true;
    track.reverse = !open;
    track.finished = false;
    if (atOnce) {
        track.frame = open ? last : 0.0f;
        track.finished = true;
    }
}

void WorldAnimator::pose(const Track& track, WorldScene& scene) {
    const NodePose pose = TreePose::sample(track.track, track.frame);
    scene.setObjectTransform(static_cast<std::size_t>(track.object),
                             TreePose::localMatrix(pose, track.origin));
}

void WorldAnimator::apply(WorldScene& scene) const {
    for (const Track& track : m_tracks) {
        pose(track, scene);
    }
}

void WorldAnimator::step(float seconds, WorldScene& scene) {
    const float advance = seconds * kFramesPerSecond;
    for (Track& track : m_tracks) {
        pose(track, scene);
        if (track.finished) {
            continue;
        }
        const auto last = static_cast<float>(track.frames - 1);
        if (track.reverse) {
            track.frame -= advance;
            if (track.frame <= 0.0f) {
                track.frame = 0.0f;
                track.finished = true;
            }
            continue;
        }
        track.frame += advance;
        if (track.once) {
            if (track.frame >= last) {
                track.frame = last;
                track.finished = true;
            }
        } else if (std::floor(track.frame) >= last) {
            track.frame = 0.0f;
        }
    }
}

} // namespace gdl
