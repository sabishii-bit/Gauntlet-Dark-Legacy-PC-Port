#pragma once
#include <array>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

namespace gdl::game {
/**
 * Where the node a move strikes with went over the last native frame: the original's
 * moveMatrix and moveOrigin, the node's world origin a 30 Hz frame ago and now
 * (CritterMoveSetup, 0x8003B1CC), along which a blow pushes a player as well as away from the
 * body (CritterNodePlayerCollide, 0x80036424). Updates may come at any rate, so the trail keeps
 * its last few samples and measures from the newest one a native frame old: a blow carries the
 * swing of the frame it lands in, whichever tick it lands on.
 */
class MoveNodeTrail {
public:
    static constexpr usize kSamples = 4; ///< a native frame of updates a quarter of one apart
    static constexpr f32 kFrameSeconds = 1.0f / 30.0f; ///< the original's frame
    static constexpr f32 kFrameSlack = 1e-6f;          ///< what summed ticks may fall short by
    static constexpr f32 kPushScale = 2.0f;            ///< 0x80346478
    static constexpr f32 kLift = 1.0f; ///< the height the way from the body is given (0x803464A8)

    bool operator==(const MoveNodeTrail&) const = default;

    /** Notes the node `seconds` on: its world origin, its origin in the model's space, the
     * model's placement and whether the fighter is another's part. A body's travel is the
     * world difference, its own step and turn with it; a part's is its pose's change alone,
     * turned by the body as it now stands. An update of no time takes the newest sample again
     * and leaves the travel as it was. */
    void step(f32 seconds, const Vec3& world, const Vec3& local, const Mat4& model, bool part);
    /** Where the node went over the last native frame, or over as much of one as the trail
     * holds when updates come faster than its samples span; nought until it has one to
     * measure from. */
    const Vec3& travel() const { return m_travel; }
    /** A blow's push on a player whose feet stand at `player`, from a body whose origin is
     * `body`: twice the sum of the node's travel and the way from the body to the player, that
     * given a height of a unit, each scaled by the reciprocal of its fast length (NormalVector),
     * so a still node pushes straight away, lifted. */
    Vec3 blowPush(const Vec3& body, const Vec3& player) const;

private:
    struct Sample {
        Vec3 world{0.0f};
        Vec3 local{0.0f};
        f32 age = 0.0f; ///< seconds since it was taken
        bool operator==(const Sample&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const Sample& state) {
            digest.fields<3>(state);
        }
    };
    friend void visitNativeState(NativeStateDigest& digest, const MoveNodeTrail& state) {
        const auto& [samples, count, travel] = state;
        digest.values(samples, count, travel);
    }
    std::array<Sample, kSamples> m_samples{}; ///< oldest first; those past the count stay default
    usize m_count = 0;
    Vec3 m_travel{0.0f};
};
} // namespace gdl::game
