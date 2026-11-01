#include <algorithm>

#include "engine/core/Types.h"

#include "game/players/CursorAim.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {
std::optional<Vec3> PlayScene::cursorAim(Vec2 cursor, const Vec3& position) const {
    return m_world != nullptr && m_presentedClip
               ? cursorAimDirection(cursor, *m_presentedClip, position)
               : std::nullopt;
}

bool PlayScene::scriptedCamera() const {
    return m_sim.switchCutscene.showing() || m_arrival.camera().active() ||
           m_sim.welcome.camera().has_value() || m_ceremonies.promotion().active() ||
           relicCeremonyOn();
}

bool PlayScene::cameraContinuous() const {
    return !m_viewOverride && previousViewCamera().has_value() && !scriptedCamera() &&
           m_previousBossCamera == bossCameraOn();
}

const std::optional<WorldCamera>& PlayScene::previousViewCamera() const {
    return soloViewActive() && !scriptedCamera() ? m_previousSeatView : m_previousCamera;
}

WorldCamera PlayScene::viewCamera() const {
    return m_viewOverride.value_or(presentedCamera());
}

/** Solo cameras hold while nothing else frames the party: not a boss fight's camera nor an
 * imported level's own. The scripted cameras (cuts, arrivals, ceremonies) show for all. */
bool PlayScene::soloViewActive() const {
    return m_sim.soloCameras && m_world != nullptr && m_world->imported() == nullptr &&
           !bossCameraOn();
}

const TowerCamera& PlayScene::seatCamera(s32 seat) const {
    return m_seats.camera(seat);
}

WorldCamera PlayScene::cameraOfSeat(s32 seat) const {
    const TowerCamera& own = seatCamera(seat);
    return m_sim.shake.apply(own.camera(), own.attention());
}

WorldCamera PlayScene::presentedCamera() const {
    if (soloViewActive() && !scriptedCamera()) {
        if (const s32 seat = m_seats.presented(m_players); seat >= 0) {
            return cameraOfSeat(seat);
        }
    }
    return simulationCamera();
}

WorldCamera PlayScene::cameraFor(const PlayerActor& actor) const {
    const s32 seat = actor.player();
    if (soloViewActive() && !scriptedCamera() && seat >= 0 && seat < kPlayerCount) {
        return cameraOfSeat(seat);
    }
    return simulationCamera();
}

std::vector<f32> PlayScene::movementYaws(f32 shared) const {
    std::vector<f32> yaws(m_players.size(), shared);
    if (!soloViewActive()) {
        return yaws;
    }
    for (usize i = 0; i < m_players.size(); ++i) {
        const s32 seat = m_players[i].actor.player();
        if (seat >= 0 && seat < kPlayerCount) {
            yaws[i] = seatCamera(seat).yaw();
        }
    }
    return yaws;
}

u8 PlayScene::arrowOwnSeats() const {
    if (soloViewActive() && !scriptedCamera()) {
        if (const s32 seat = m_seats.presented(m_players); seat >= 0) {
            return static_cast<u8>(1U << static_cast<u32>(seat));
        }
    }
    return m_localSeats;
}

PlayScene::SeatCameraStates PlayScene::seatCameraStates() const {
    return m_seats.states();
}

std::vector<CameraSubject> PlayScene::partySubjects(const Vec3& displacement) const {
    std::vector<CameraSubject> subjects;
    subjects.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        const auto& actor = runtime.actor;
        subjects.push_back({actor.position() + displacement, actor.followPoint() + displacement,
                            actor.height() * 0.5f});
    }
    return subjects;
}

void PlayScene::resetSeatCameras(std::span<const CameraSubject> subjects,
                                 std::span<const WorldLocator> markers, const CameraRange& range) {
    m_seats.reset(m_players, subjects, markers, range, cameraView());
}

void PlayScene::followSeatCameras(std::span<const CameraSubject> subjects, f32 seconds) {
    m_seats.follow(m_players, subjects, m_world->cameraMarkers(), m_world->cameraRange(),
                   cameraView(), seconds);
}

void PlayScene::snapSeatCameras(std::span<const CameraSubject> subjects) {
    m_seats.snap(m_players, subjects, m_world->cameraRange());
}

WorldCamera PlayScene::simulationCamera() const {
    if (const auto& camera = m_sim.switchCutscene.camera();
        m_sim.switchCutscene.showing() && camera.has_value()) {
        return *camera;
    }
    if (m_arrival.camera().active()) {
        return m_arrival.camera().camera();
    }
    if (const auto cut = m_sim.welcome.camera()) {
        return *cut;
    }
    if (const auto& promotion = m_ceremonies.promotion().camera();
        promotion.has_value() && !spawning() && m_ceremonies.promotion().active()) {
        return *promotion;
    }
    if (const auto& ceremony = m_ceremonies.relics().camera();
        ceremony.has_value() && relicCeremonyOn()) {
        return *ceremony;
    }
    if (m_world != nullptr) {
        if (const auto* imported = m_world->imported()) {
            if (const auto camera = imported->camera()) {
                const f32 distance =
                    std::max(1.0f, glm::distance(camera->position, m_camera.attention()));
                return m_sim.shake.apply(*camera, camera->position + camera->forward() * distance);
            }
        }
    }
    return bossCameraOn()
               ? m_sim.shake.apply(m_sim.bossCamera.camera(), m_sim.bossCamera.attention())
               : m_sim.shake.apply(m_camera.camera(), m_camera.attention());
}

/** A boss level with a boss camera record frames the fight with it while the boss stands. */
bool PlayScene::bossCameraOn() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    return level != nullptr && level->bossCamera.has_value() &&
           (m_opponents.bosses().present() || m_bossSequence.fallen());
}

/** The boss as the camera sees it; once it has fallen, the wizard in its place. */
BossCameraSubject PlayScene::bossSubject() const {
    BossCameraSubject subject;
    if (m_bossSequence.victory().state().running()) {
        return m_bossSequence.victorySubject();
    }
    if (!m_opponents.bosses().present() && m_bossSequence.fallen()) {
        return m_bossSequence.restSubject();
    }
    if (const Vec3* at = m_opponents.bosses().position(); at != nullptr) {
        subject.position = *at;
    }
    subject.facing = m_opponents.bosses().facing();
    subject.radius = m_opponents.bosses().radius();
    subject.height = m_opponents.bosses().height();
    subject.attentionOffset = m_opponents.bosses().cameraOffset();
    subject.baseAttention = m_opponents.bosses().cameraBase();
    subject.awake = m_opponents.bosses().view().awake;
    return subject;
}

/** Canonical gameplay projection for camera fitting, visibility, party limits and
 * transporter selection. Local FOV/aspect belong only to render() and its
 * presented cursor projection; they must not change the next native update. */
CameraView PlayScene::cameraView() {
    return {};
}

} // namespace gdl::game
