#include "game/screens/PlayReplication.h"

#include <cmath>
#include <limits>

#include "game/screens/CombatCapture.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/ReplicaHud.h"

namespace gdl::game {
bool PlayReplication::rosterMatches(const MatchContext& context) const {
    if (m_scene == nullptr || !m_scene->isOpen() ||
        m_scene->simulationTickRate() != context.tickRate) {
        return false;
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& runtime : m_scene->participants()) {
        const auto seat = runtime.actor.player();
        if (seat < 0 || seat >= static_cast<s32>(seen.size()) || seen[static_cast<usize>(seat)] ||
            runtime.departed || context.owners[static_cast<usize>(seat)] == 0) {
            return false;
        }
        seen[static_cast<usize>(seat)] = true;
    }
    for (usize seat = 0; seat < seen.size(); ++seat) {
        if (seen[seat] != (context.owners[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool PlayReplication::bind(PlayScene& scene, const MatchSession& match,
                           const PickupResources& pickups, const FixtureResources& fixtures) {
    if (!match.host() || match.phase() != MatchSession::Phase::Loading || match.tick() != 0 ||
        !match.context().valid() || match.context().epoch <= m_context.epoch) {
        return false;
    }
    PlayReplication candidate;
    candidate.m_scene = &scene;
    candidate.m_pickups = &pickups;
    candidate.m_fixtures = &fixtures;
    candidate.m_context = match.context();
    if (!candidate.rosterMatches(candidate.m_context) ||
        !candidate.m_fighters.bind(scene.critters(), scene.bosses())) {
        return false;
    }
    candidate.m_continuity.fill(1);
    if (match.context().transition == MatchTransition::Resume && m_scene == &scene) {
        candidate.m_arrivalSkipVotes = m_arrivalSkipVotes;
    }
    *this = std::move(candidate);
    return true;
}
void PlayReplication::clear() {
    *this = PlayReplication{};
}
PlayReplication::Result PlayReplication::fail() {
    m_failed = true;
    if (m_scene != nullptr && !m_quiet) {
        m_scene->pauseGameplaySounds();
        m_quiet = true;
    }
    return Result::Failed;
}

std::optional<CombatSnapshot> PlayReplication::capture(u64 tick,
                                                       const ProjectileResources& resources) {
    MotionSnapshot motion;
    motion.epoch = m_context.epoch;
    motion.tick = tick;
    const auto increment = [](u32& continuity) {
        if (continuity == std::numeric_limits<u32>::max()) {
            return false;
        }
        ++continuity;
        return true;
    };
    if (m_latest && !m_scene->cameraContinuous() && !increment(m_cameraContinuity)) {
        return std::nullopt;
    }
    motion.cameraContinuity = m_cameraContinuity;
    motion.camera = m_scene->viewCamera();
    motion.camera.pitch = std::remainder(motion.camera.pitch, kTwoPi);
    motion.camera.yaw = std::remainder(motion.camera.yaw, kTwoPi);
    motion.camera.roll = std::remainder(motion.camera.roll, kTwoPi);
    const auto view = m_scene->cameraView();
    motion.horizontalFov = view.horizontalFov;
    motion.aspect = view.aspect;
    for (const auto& runtime : m_scene->participants()) {
        const auto seat = static_cast<usize>(runtime.actor.player());
        if (m_latest && !PartyFigures::presentationContinuous(runtime) &&
            !increment(m_continuity[seat])) {
            return std::nullopt;
        }
        motion.players[seat] =
            SeatMotion{m_context.grants[seat], m_continuity[seat], runtime.actor.position(),
                       std::remainder(runtime.actor.yaw(), kTwoPi)};
    }
    auto state = CombatCapture::capture(motion, m_scene->participants(), m_scene->enemies(),
                                        &m_scene->departure());
    if (!state ||
        !ProjectileCapture::append(*state, resources, m_scene->missiles(), m_scene->enemyMissiles(),
                                   &m_scene->effects(), &m_scene->arrival())) {
        return std::nullopt;
    }
    const auto* world = m_scene->world();
    if (world == nullptr) {
        return std::nullopt;
    }
    state->geometry = world->scene().geometry();
    state->hud = HudCapture::capture(m_scene->participants(), m_scene->hud(), m_scene->hudVisible(),
                                     &m_scene->bossMeter());
    if (!state->hud) {
        return std::nullopt;
    }
    state->hud->screen = HudCapture::screen(m_scene->transition(), m_scene->arrival(),
                                            m_scene->gameOver(), m_scene->cinematicBars());
    if (const auto scroll = m_scene->scrollLook()) {
        state->hud->scroll =
            HudScroll{scroll->message, scroll->page, scroll->burnFrame, scroll->promptAlpha};
    }
    if (const auto hourglass = m_scene->hourglassLook()) {
        state->hud->hourglass = HudHourglass{hourglass->elapsed, hourglass->fallingFrame};
    }
    if (m_scene->runeMeter().visible()) {
        state->hud->runeFill = m_scene->runeMeter().fill();
    }
    if (world->level() == nullptr || world->level()->title.empty()) {
        state->hud->screen.titleScale = 0;
    }
    if (!FighterCapture::append(*state, m_fighters, m_scene->critters(), m_scene->bosses())) {
        return std::nullopt;
    }
    if (m_pickups == nullptr || !PickupCapture::append(*state, world->placedItems(), *m_pickups)) {
        return std::nullopt;
    }
    if (m_fixtures == nullptr ||
        !FixtureCapture::append(*state, *m_fixtures,
                                {m_scene->chests(), m_scene->gates(), world->triggers(),
                                 m_scene->generators(), m_scene->barrels(), m_scene->traps(),
                                 m_scene->safeRocks(), m_scene->rubble(), &m_scene->statues(),
                                 &m_scene->portals()})) {
        return std::nullopt;
    }
    return state->valid() ? std::move(state) : std::nullopt;
}
void PlayReplication::filterArrivalSkip(SessionInputs::Frame& inputs) {
    const auto& camera = m_scene->arrival().camera();
    if (!camera.active()) {
        m_arrivalSkipVotes = {};
        return;
    }
    // Only legacy boss-entry holds are skippable. Keep the native eligibility
    // window, but require the whole online party instead of its first button.
    const bool eligible = camera.mode() == StartCamera::Mode::Legacy &&
                          camera.phase() == StartCamera::Phase::Hold &&
                          camera.ticksLeft() <= StartCamera::kSkipBelow;
    bool unanimous = eligible;
    for (usize seat = 0; seat < inputs.size(); ++seat) {
        auto& menu = inputs[seat].menu;
        if (eligible && m_context.owners[seat] != 0) {
            m_arrivalSkipVotes[seat] =
                m_arrivalSkipVotes[seat] || menu.start || menu.select || menu.back;
            unanimous = unanimous && m_arrivalSkipVotes[seat];
        }
        menu.start = false;
        menu.select = false;
        menu.back = false;
    }
    if (unanimous) {
        for (usize seat = 0; seat < inputs.size(); ++seat) {
            inputs[seat].menu.select = m_context.owners[seat] != 0;
        }
    }
}

PlayReplication::Result PlayReplication::advance(MatchSession& match,
                                                 const SessionInputs::Frame& local,
                                                 const ProjectileResources& resources) {
    if (m_failed) {
        return Result::Failed;
    }
    if (m_scene == nullptr || m_finished) {
        return Result::Held;
    }
    if (!match.host() || match.context() != m_context || match.tick() != m_nextTick ||
        !rosterMatches(m_context)) {
        return fail();
    }
    if (match.phase() != MatchSession::Phase::Running) {
        if (!m_quiet) {
            m_scene->pauseGameplaySounds();
            m_quiet = true;
        }
        return Result::Held;
    }
    if (!MatchInputs::sample(match, local)) {
        return fail();
    }
    auto inputs = MatchInputs::advance(match);
    if (!inputs) {
        return fail();
    }
    filterArrivalSkip(*inputs);
    m_quiet = false;
    const auto outcome = m_scene->update(1.0 / m_context.tickRate, *inputs);
    ++m_nextTick;
    if (!rosterMatches(m_context)) {
        // Mid-match departure needs a new admitted roster, not a silent slot edit.
        return fail();
    }
    auto state = capture(match.tick() - 1, resources);
    if (!state || ((state->motion.tick % kSnapshotStride == 0 || outcome != PlayOutcome::Running) &&
                   !match.publish(*state))) {
        return fail();
    }
    m_latest = std::move(state);
    m_finished = outcome != PlayOutcome::Running;
    switch (outcome) {
    case PlayOutcome::Running: return Result::Advanced;
    case PlayOutcome::Travel: return Result::Travel;
    case PlayOutcome::GameOver: return Result::GameOver;
    case PlayOutcome::Leave: return Result::Leave;
    }
    return fail();
}
} // namespace gdl::game
