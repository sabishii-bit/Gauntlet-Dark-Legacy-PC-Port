#include "engine/world/WorldAnimator.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/TreePose.h"

namespace gdl {
namespace {
constexpr f32 kFrameSlack = 0.00001f;

/** A continuous turn: a spinning decoration whose keys turn one way the whole way round. */
struct ContinuousTurn {
    s32 channel = -1; ///< the rotation channel it turns, 0 to 2
    f32 rate = 0.0f;  ///< signed radians a frame, at the authored keys' pace
    f32 seam = 0.0f;  ///< the frames from the last key back to the first at that rate
};

/** A spinning decoration crosses its loop seam, unlike a cart returning to its start.
 * Check only at wraps: authored keys and native timing remain untouched. */
std::optional<ContinuousTurn> continuousTurn(const TrackInfo& track, s32 frames) {
    const u32 channels = track.flags & 0x1FFU;
    if ((channels != 1 && channels != 2 && channels != 4) || track.frames.size() < 3 ||
        track.values.size() != track.frames.size() || track.frames.front() != 0 ||
        track.frames.back() != frames - 1) {
        return std::nullopt;
    }
    f32 turn = 0;
    f32 fastest = 0;
    for (usize key = 1; key < track.frames.size(); ++key) {
        const f32 interval = static_cast<f32>(track.frames[key] - track.frames[key - 1]);
        const f32 step = TreePose::wrapAngle(track.values[key] - track.values[key - 1]);
        if (interval <= 0 || std::abs(step) >= TreePose::kHoldAngle || step == 0 ||
            (turn != 0 && step * turn < 0)) {
            return std::nullopt;
        }
        turn += step;
        fastest = std::max(fastest, std::abs(step) / interval);
    }
    const f32 seam = TreePose::wrapAngle(track.values.front() - track.values.back());
    // K1's exported full turns omit a closing sample: their final-to-first seam
    // can span two authored increments. A partial turn or a reversing reset is
    // not a continuous spin, even if its endpoints happen to be close together.
    if (std::abs(turn) <= kPi || seam * turn < -kFrameSlack ||
        std::abs(seam) > fastest * 2.5f + kFrameSlack) {
        return std::nullopt;
    }
    ContinuousTurn result;
    result.channel = channels == 1 ? 0 : channels == 2 ? 1 : 2;
    result.rate = turn < 0 ? -fastest : fastest;
    // The keys that stop short of the turn leave the rest of it to the seam, which the
    // loop runs through at the same pace so the spin never jolts; a closed turn has none.
    result.seam = std::abs(seam) > kFrameSlack ? std::abs(seam) / fastest : 0.0f;
    return result;
}

/** The whole cycle of a looping track: its keys, and a continuous turn's seam after them. */
f32 cycleOf(const WorldAnimator::Cursor& cursor, f32 seam) {
    return static_cast<f32>(cursor.frames - 1) + seam;
}
} // namespace

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
        if (const auto turn = continuousTurn(track.track, track.frames)) {
            track.seam = turn->seam;
            track.turnChannel = turn->channel;
            track.turnRate = turn->rate;
        }
        m_tracks.push_back(std::move(track));
    }
}

const CanonicalDigestSink::Digest& WorldAnimator::trackDigest() const {
    if (!m_trackDigest) {
        NativeStateDigest tracks;
        tracks.string("WorldAnimator.tracks/1");
        tracks.wide(m_tracks.size());
        for (const auto& track : m_tracks) {
            tracks.fields<4>(track.track);
            tracks.value(track.origin);
        }
        m_trackDigest = tracks.finish();
    }
    return *m_trackDigest;
}

bool WorldAnimator::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("WorldAnimator/2");
    digest.bytes(trackDigest());
    digest.wide(m_tracks.size());
    for (usize i = 0; i < m_tracks.size(); ++i) {
        const auto& track = m_tracks[i];
        digest.value(saved ? saved->m_cursors[i] : static_cast<const Cursor&>(track));
    }
    digest.value(saved ? saved->m_cycles : m_cycleEvents);
    return digest.good();
}

bool WorldAnimator::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool WorldAnimator::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [cursors, cycles, owner, binding] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

void WorldAnimator::saveLocal(LocalSnapshot& saved) const {
    saved.m_cursors.resize(m_tracks.size());
    for (usize i = 0; i < m_tracks.size(); ++i) {
        saved.m_cursors[i] = static_cast<const Cursor&>(m_tracks[i]);
    }
    saved.m_cycles = m_cycleEvents;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
}

bool WorldAnimator::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_localBinding && saved.m_binding.lock() == m_localBinding &&
           saved.m_cursors.size() == m_tracks.size();
}

bool WorldAnimator::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_cursors == right.m_cursors &&
           left.m_cycles == right.m_cycles;
}

bool WorldAnimator::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < m_tracks.size(); ++i) {
        static_cast<Cursor&>(m_tracks[i]) = saved.m_cursors[i];
    }
    m_cycleEvents = saved.m_cycles;
    return true;
}

void WorldAnimator::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_trackDigest.reset();
    m_tracks.clear();
    m_cycleEvents.clear();
}

bool WorldAnimator::Cursor::valid() const {
    if (object < 0 || frames <= 0 || !std::isfinite(seam) || seam < 0 || seam > kMostSeamFrames ||
        !std::isfinite(frame) || frame < 0 || frame > static_cast<f32>(frames - 1) + seam) {
        return false;
    }
    if (held) {
        return once && reverse && finished && frame == 0;
    }
    if (finished) {
        return reverse ? frame == 0 : once && frame == static_cast<f32>(frames - 1);
    }
    return true;
}

std::vector<WorldAnimator::Cursor> WorldAnimator::checkpoint() const {
    std::vector<Cursor> result;
    result.reserve(m_tracks.size());
    for (const auto& track : m_tracks) {
        result.push_back({track.object, track.frames, track.seam, track.frame, track.reverse,
                          track.once, track.finished, track.held, track.presentationCut});
    }
    return result;
}

bool WorldAnimator::restore(std::span<const Cursor> cursors) {
    if (cursors.size() != m_tracks.size()) {
        return false;
    }
    for (usize i = 0; i < cursors.size(); ++i) {
        const auto& cursor = cursors[i];
        const auto& track = m_tracks[i];
        if (!cursor.valid() || cursor.object != track.object || cursor.frames != track.frames ||
            cursor.seam != track.seam) {
            return false;
        }
    }
    for (usize i = 0; i < cursors.size(); ++i) {
        const auto& cursor = cursors[i];
        auto& track = m_tracks[i];
        track.frame = cursor.frame;
        track.reverse = cursor.reverse;
        track.once = cursor.once;
        track.finished = cursor.finished;
        track.held = cursor.held;
        track.presentationCut = cursor.presentationCut;
    }
    m_cycleEvents.clear();
    return true;
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
    track.presentationCut = true;
}

void WorldAnimator::fire(s32 object, bool open, Finish finish) {
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
    if (finish == Finish::AtOnce) {
        track.frame = open ? last : 0.0f;
        track.finished = true;
        track.presentationCut = true;
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
    const auto last = static_cast<f32>(track.frames - 1);
    NodePose pose = TreePose::sample(track.track, std::min(track.frame, last));
    if (track.seam > 0.0f && track.frame > last && track.turnChannel >= 0) {
        // Through the seam the turn runs on from the last key at the authored rate, so
        // the first key is reached as the cycle closes.
        pose.rotation[track.turnChannel] =
            TreePose::wrapAngle(pose.rotation[track.turnChannel] +
                                track.turnRate * std::min(track.frame - last, track.seam));
    }
    scene.setObjectTransform(static_cast<usize>(track.object),
                             TreePose::localMatrix(pose, track.origin), track.presentationCut);
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
        track.presentationCut = false;
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
        } else if (const f32 cycle = cycleOf(track, track.seam);
                   track.frame + kFrameSlack >= cycle) {
            // A looping world track's final key closes the cycle (a continuous turn's seam
            // after it); retain time past that point rather than lengthening every loop by a
            // caller-dependent amount.
            track.frame =
                cycle > 0.0f && track.frame >= cycle ? std::fmod(track.frame, cycle) : 0.0f;
            track.presentationCut = track.turnChannel < 0;
            m_cycleEvents.push_back({track.object, true});
        } else if (!restarted && std::floor(track.frame) <= 1) {
            m_cycleEvents.push_back({track.object, false});
        }
    }
}

} // namespace gdl
