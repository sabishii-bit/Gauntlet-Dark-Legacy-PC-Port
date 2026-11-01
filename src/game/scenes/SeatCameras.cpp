#include "game/scenes/SeatCameras.h"

#include <algorithm>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl::game {

namespace {

bool standing(const PlayerRuntime& player) {
    return player.life == PlayerLife::Standing;
}

bool inSeats(s32 seat) {
    return seat >= 0 && seat < static_cast<s32>(SeatCameras::kSeats);
}

} // namespace

template <class Apply>
void SeatCameras::eachSeat(std::span<const PlayerRuntime> players,
                           std::span<const CameraSubject> subjects, Apply apply) {
    std::vector<CameraSubject> party;
    for (usize i = 0; i < subjects.size() && i < players.size(); ++i) {
        if (standing(players[i])) {
            party.push_back(subjects[i]);
        }
    }
    for (usize i = 0; i < players.size() && i < subjects.size(); ++i) {
        const s32 seat = players[i].actor.player();
        if (!inSeats(seat)) {
            continue;
        }
        const std::span<const CameraSubject> own = standing(players[i]) ? subjects.subspan(i, 1)
                                                   : party.empty()
                                                       ? subjects
                                                       : std::span<const CameraSubject>{party};
        apply(m_cameras[static_cast<usize>(seat)], own);
    }
}

void SeatCameras::reset(std::span<const PlayerRuntime> players,
                        std::span<const CameraSubject> subjects,
                        std::span<const WorldLocator> markers, const CameraRange& range,
                        const CameraView& view) {
    eachSeat(players, subjects, [&](TowerCamera& camera, std::span<const CameraSubject> own) {
        camera.reset(own, markers, range, view);
    });
}

void SeatCameras::follow(std::span<const PlayerRuntime> players,
                         std::span<const CameraSubject> subjects,
                         std::span<const WorldLocator> markers, const CameraRange& range,
                         const CameraView& view, f32 seconds) {
    eachSeat(players, subjects, [&](TowerCamera& camera, std::span<const CameraSubject> own) {
        camera.update(own, markers, range, view, seconds);
    });
}

void SeatCameras::snap(std::span<const PlayerRuntime> players,
                       std::span<const CameraSubject> subjects, const CameraRange& range) {
    eachSeat(players, subjects, [&](TowerCamera& camera, std::span<const CameraSubject> own) {
        camera.snapAttention(own, range);
    });
}

const TowerCamera& SeatCameras::camera(s32 seat) const {
    return m_cameras[static_cast<usize>(std::clamp(seat, 0, static_cast<s32>(kSeats) - 1))];
}

s32 SeatCameras::presented(std::span<const PlayerRuntime> players) const {
    s32 lowest = -1;
    for (const PlayerRuntime& player : players) {
        const s32 seat = player.actor.player();
        if (!inSeats(seat)) {
            continue;
        }
        if (seat == m_viewSeat) {
            return seat;
        }
        if (standing(player) && (lowest < 0 || seat < lowest)) {
            lowest = seat;
        }
    }
    return lowest;
}

SeatCameras::States SeatCameras::states() const {
    States result;
    for (usize i = 0; i < result.size(); ++i) {
        result[i] = m_cameras[i].localState();
    }
    return result;
}

void SeatCameras::restore(const States& states) {
    for (usize i = 0; i < m_cameras.size(); ++i) {
        m_cameras[i].restoreLocal(states[i]);
    }
}

bool SeatCameras::appendNativeDigest(NativeStateDigest& digest) const {
    bool good = true;
    for (const TowerCamera& camera : m_cameras) {
        good = camera.appendNativeDigest(digest) && good;
    }
    return good;
}

} // namespace gdl::game
