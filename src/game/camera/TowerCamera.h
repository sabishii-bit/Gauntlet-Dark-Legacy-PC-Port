#pragma once

#include <array>
#include <memory>
#include <span>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** A character the camera keeps in view: where it stands and the point it follows. */
struct CameraSubject {
    Vec3 feet{0.0f, 0.0f, 0.0f};
    Vec3 follow{0.0f, 0.0f, 0.0f}; ///< the body's centre, part way up
    f32 viewRadius = 0.0f;         ///< half the authored character height, for boss-camera fitting
};

/** The level's camera data: how far the camera stays and how flat it may look. */
struct CameraRange {
    f32 radiusMin = 24.0f;
    f32 radiusMax = 32.0f;
    f32 minPitch = 0.6981317f;        ///< a party is looked down on by at least this angle
    Vec3 boundsMin{0.0f, 0.0f, 0.0f}; ///< the look-at point stays inside this box when it
    Vec3 boundsMax{0.0f, 0.0f, 0.0f}; ///< has any size
};

/** The projection the camera must fit the party into. */
struct CameraView {
    f32 horizontalFov = 1.0471976f; ///< radians
    f32 aspect = 640.0f / 448.0f;
};

/**
 * The tower's follow camera, as the original's standard camera works: it looks at the middle
 * of the party from the angle of the nearest camera marker, turning to a new marker's angle
 * over a short while, keeps the look-at point and its distance smoothed over the last few
 * frames, and backs away just far enough to keep every player on screen. A party is held
 * inside heldView() as it moves, which caps how far it can draw the camera out.
 */
class TowerCamera {
public:
    TowerCamera() = default;
    /** Native camera values remain copyable, but a copied or replaced camera
     * never inherits another owner's binding identity. */
    TowerCamera(const TowerCamera& other);
    TowerCamera& operator=(const TowerCamera& other);
    TowerCamera(TowerCamera&& other) noexcept;
    TowerCamera& operator=(TowerCamera&& other) noexcept;
    ~TowerCamera() = default;

    static constexpr s32 kRing = 9;         ///< frames the look-at point and distance average over
    static constexpr s32 kTurnSteps = 50;   ///< steps a turn to a new marker takes
    static constexpr f32 kStepRate = 30.0f; ///< steps per second
    static constexpr f32 kSwitchRatio =
        4.0f / 9.0f; ///< a marker takes over when this much nearer (squared)
    static constexpr f32 kAloneDistance = 0.8f;    ///< of the shortest range with one player
    static constexpr f32 kPartyDistance = 0.9375f; ///< of the longest range with more
    static constexpr f32 kNearGap = 10.0f;   ///< slack kept when the fit lets the camera come in
    static constexpr f32 kFarGap = 4.0f;     ///< slack added when it must go out
    static constexpr s32 kSettleLimit = 100; ///< updates a fresh start may take to settle

    /** Complete native follow-camera history. This is gameplay state: it affects
     * party bounds and visibility-gated spawning, not the local video projection. */
    struct State {
        WorldCamera camera;
        Vec3 attention{0};
        f32 distance = 0;
        s32 marker = -1;
        s32 aimedMarker = -1;
        f32 yawRate = 0;
        f32 pitchRate = 0;
        f32 turnStep = static_cast<f32>(kTurnSteps);
        std::array<Vec3, kRing> ringPositions{};
        std::array<f32, kRing> ringDistances{};
        s32 ringIndex = 0;
        bool operator==(const State&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const State& state) {
            const auto& [camera, attention, distance, marker, aimedMarker, yawRate, pitchRate,
                         turnStep, ringPositions, ringDistances, ringIndex] = state;
            digest.fields<4>(camera);
            digest.values(attention, distance, marker, aimedMarker, yawRate, pitchRate, turnStep,
                          ringPositions, ringDistances, ringIndex);
        }
    };
    const State& localState() const { return m_state; }
    bool appendNativeDigest(NativeStateDigest& digest) const;
    /** State is a plain value, intentionally independent of camera ownership.
     * The scene's history guard supplies the enclosing stage/owner boundary. */
    static bool appendLocalDigest(NativeStateDigest& digest, const State& state);
    static bool equalLocal(const State& left, const State& right) { return left == right; }
    /** Trusted in-process state, copied without allocating a new binding. */
    void restoreLocal(const State& state) noexcept { m_state = state; }

    /** Stands the camera up for `subjects` and lets it settle, like a level start. */
    void reset(std::span<const CameraSubject> subjects, std::span<const WorldLocator> markers,
               const CameraRange& range, const CameraView& view);

    /** Follows `subjects` for `seconds`; true while anything is still moving. */
    bool update(std::span<const CameraSubject> subjects, std::span<const WorldLocator> markers,
                const CameraRange& range, const CameraView& view, f32 seconds);
    /** End a trigger-camera hold at the current party target, without re-aiming the view. */
    void snapAttention(std::span<const CameraSubject> subjects, const CameraRange& range);

    /** How far behind the look-at point the camera keeps while the party fits easily
     * (fn_8006DF34's +0xDC): the range's shortest when it has no span, 0.8 of that for one
     * player, else the nearest marker's own distance or 0.9375 of the range's longest. */
    f32 keptDistance(usize count, std::span<const WorldLocator> markers,
                     const CameraRange& range) const;
    /** The view a party of `count` is held inside as it moves (fn_8006DC64): this camera's
     * angles from keptDistance() behind the look-at point, however far the fit has backed the
     * camera off to show someone carried out of it. */
    WorldCamera heldView(usize count, std::span<const WorldLocator> markers,
                         const CameraRange& range) const;

    const WorldCamera& camera() const { return m_state.camera; }
    f32 yaw() const { return m_state.camera.yaw; }
    f32 pitch() const { return m_state.camera.pitch; }
    f32 distance() const { return m_state.distance; }
    const Vec3& attention() const { return m_state.attention; }
    s32 marker() const { return m_state.marker; }
    bool turning() const { return m_state.turnStep < static_cast<f32>(kTurnSteps); }

    /** The angles a marker asks for: the camera looks the way the marker faces. */
    static f32 markerYaw(const WorldLocator& marker) { return marker.rotation.y; }
    static f32 markerPitch(const WorldLocator& marker) { return marker.rotation.x; }

private:
    void copyValues(const TowerCamera& other) noexcept;
    static Vec3 middleOf(std::span<const CameraSubject> subjects, const CameraRange& range);
    Vec3 projectedMiddleOf(std::span<const CameraSubject> subjects, const CameraRange& range) const;
    void chooseMarker(std::span<const WorldLocator> markers, const Vec3& near);
    void aim(std::span<const WorldLocator> markers, usize count, const CameraRange& range,
             bool jump);
    bool turn(f32 steps);
    bool follow();
    f32 fitDistance(const Vec3& point, std::span<const CameraSubject> subjects,
                    const CameraView& view, const CameraRange& range, f32 current) const;
    void place();

    State m_state;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
