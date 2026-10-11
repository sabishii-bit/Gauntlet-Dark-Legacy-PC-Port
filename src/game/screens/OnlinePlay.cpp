#include "game/screens/OnlinePlay.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"

#include "game/screens/MatchInputs.h"

namespace gdl::game {
struct OnlinePlay::Host {
    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;
    LevelWorld world;
    PlayScene scene;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    PlayReplication driver;
    ~Host() { scene.close(); }
};
OnlinePlay::OnlinePlay() = default;
OnlinePlay::~OnlinePlay() {
    close();
}

std::optional<u32> OnlinePlay::sceneId(const LevelCatalog& catalog, const LevelRef& level) {
    if (level.isTower()) {
        return level.name == "L1" && (level == LevelRef::tower() || catalog.byName("L1") == level)
                   ? std::optional<u32>{14 * 256 + 1}
                   : std::nullopt;
    }
    if (level.realmId < 0 || level.realmId >= 255 || level.index < 0 || level.index >= 255 ||
        catalog.byName(level.name) != level) {
        return std::nullopt;
    }
    return static_cast<u32>((level.realmId + 1) * 256 + level.index + 1);
}
std::optional<LevelRef> OnlinePlay::levelOf(const LevelCatalog& catalog, u32 scene) {
    if (scene == 14 * 256 + 1) {
        return LevelRef::tower();
    }
    if (scene == 0 || scene > 65535 || scene % 256 == 0) {
        return std::nullopt;
    }
    const auto realmId = static_cast<s32>(scene / 256) - 1;
    const auto index = static_cast<usize>(scene % 256 - 1);
    for (const auto& realm : catalog.realms()) {
        if (realm.id != realmId || realm.id == LevelRef::kTowerRealm ||
            index >= realm.levels.size()) {
            continue;
        }
        const auto level = catalog.byName(realm.levels[index]);
        if (level && sceneId(catalog, *level) == scene) {
            return level;
        }
    }
    return std::nullopt;
}
bool OnlinePlay::open(RenderDevice& device, const GameContext& context, OnlineSession& session,
                      const OnlineParty& selection, const LevelRef& initial,
                      const PlayOptions& options) {
    const auto& match = session.match();
    const bool initialLoad = !session.host() && match.phase() == MatchSession::Phase::Loading &&
                             match.context().transition == MatchTransition::Start;
    if (m_phase != Phase::Closed || session.phase() != OnlineSession::Phase::Active ||
        (match.phase() != MatchSession::Phase::Lobby && !initialLoad) ||
        context.levels == nullptr || !context.levels->loaded() ||
        (context.config != nullptr && context.config->timing.tickRate != 60) ||
        !selection.members(session)) {
        return false;
    }
    const auto scene = sceneId(*context.levels, initial);
    if (session.host() && (!scene || !session.match().prepare(*scene))) {
        return false;
    }
    m_device = &device;
    m_context = context;
    m_session = &session;
    m_selection = &selection;
    m_options = options;
    // Remote progression has no disk authority. The owning application will
    // apply acknowledged local checkpoints when post-level persistence is wired.
    m_context.unlockClasses = {};
    m_context.saveSettings = {};
    m_context.tower = nullptr;
    // PlayScene addresses room seats, while local feedback addresses devices.
    if (context.vibrate) {
        m_context.vibrate = [&session, &selection, callback = context.vibrate](
                                s32 seat, s32 frames, ControlFeedback feedback) {
            if (seat >= 0 && seat < static_cast<s32>(InputCommand::kSeats)) {
                if (const auto local = selection.device(session, static_cast<u8>(seat))) {
                    callback(*local, frames, feedback);
                }
            }
        };
    }
    m_phase = Phase::Loading;
    return true;
}
void OnlinePlay::quiet() {
    if (m_host) {
        m_host->scene.pauseGameplaySounds();
    }
    if (m_context.stopVibration) {
        m_context.stopVibration();
    }
}
void OnlinePlay::close() {
    quiet();
    m_entry.close();
    m_entryEpoch = 0;
    m_host.reset();
    m_replica.clear();
    m_clock.clear();
    m_device = nullptr;
    m_context = {};
    m_session = nullptr;
    m_selection = nullptr;
    m_options = {};
    m_travelMembers.clear();
    m_phase = Phase::Closed;
    m_failure = Failure::None;
    m_outcome = PlayReplication::Result::Held;
    m_epoch = 0;
    m_pausedDevices = 0;
}
OnlinePlay::Phase OnlinePlay::fail(Failure reason) {
    quiet();
    m_entry.close();
    m_failure = reason;
    m_phase = Phase::Failed;
    if (m_session != nullptr) {
        m_session->leave();
    }
    return m_phase;
}
bool OnlinePlay::load(const SessionInputs::Frame& devices) {
    auto& match = m_session->match();
    const auto& context = match.context();
    const auto level = levelOf(*m_context.levels, context.scene);
    if (!level) {
        fail(Failure::Scene);
        return false;
    }
    const bool travelling = context.transition == MatchTransition::Travel;
    const auto* profiles = travelling ? match.travelParty() : m_session->party();
    if (travelling && profiles == nullptr) {
        return false; // Reliable profiles can arrive in a later receive batch.
    }
    if (travelling) {
        const auto* admitted = m_session->party();
        if (admitted == nullptr || profiles == nullptr) {
            fail(Failure::Party);
            return false;
        }
        for (usize seat = 0; seat < profiles->size(); ++seat) {
            const auto& next = (*profiles)[seat];
            const auto& original = (*admitted)[seat];
            if (next.has_value() != original.has_value() ||
                (next && (next->name != original->name || next->character != original->character ||
                          next->color != original->color))) {
                fail(Failure::Party);
                return false;
            }
        }
    }
    try {
        if (context.transition != MatchTransition::Resume) {
            if ((m_epoch != 0) != travelling || profiles == nullptr) {
                fail(Failure::Scene);
                return false;
            }
            if (m_entryEpoch != context.epoch) {
                // Dispose the old scene before entry audio starts, but preserve its
                // party checkpoint. The guest never constructs a gameplay scene.
                m_host.reset();
                m_replica.clear();
                m_clock.clear();
                std::vector<PartyMember> preview;
                for (usize seat = 0; seat < profiles->size(); ++seat) {
                    if (const auto& profile = (*profiles)[seat]) {
                        PartyMember member;
                        member.player = static_cast<s32>(seat);
                        member.save = profile->gameplayCopy();
                        member.fallen = profile->progress.health <= 0;
                        preview.push_back(std::move(member));
                    }
                }
                m_entry.open(*m_device, m_context, *level, preview);
                m_entryEpoch = context.epoch;
            } else {
                const auto local = m_selection->inputs(*m_session, devices);
                if (!local) {
                    fail(Failure::Input);
                    return false;
                }
                if (m_entry.phase() == OnlineLevelEntry::Phase::Movie) {
                    const auto seats = m_session->seats();
                    for (usize index = 0; index < seats.size(); ++index) {
                        if ((*local)[index].movieSkipPressed &&
                            !match.requestMovieSkip(seats[index])) {
                            fail(Failure::Input);
                            return false;
                        }
                    }
                }
                m_entry.update(*m_device, m_context, match.movieSkipReady());
            }
            if (m_entry.phase() == OnlineLevelEntry::Phase::Failed) {
                fail(Failure::Assets);
                return false;
            }
            if (m_entry.phase() != OnlineLevelEntry::Phase::Ready) {
                return false;
            }
        }
        if (context.transition == MatchTransition::Resume) {
            if (m_epoch == 0 ||
                (match.host() ? !m_host || !m_host->driver.bind(m_host->scene, match,
                                                                m_host->pickups, m_host->fixtures)
                              : !m_replica.resume(context) || !m_clock.begin(match))) {
                fail(Failure::Scene);
                return false;
            }
        } else if ((m_epoch != 0) != travelling) {
            fail(Failure::Scene);
            return false;
        } else if (match.host()) {
            const auto members =
                travelling ? std::optional{m_travelMembers} : m_selection->members(*m_session);
            auto candidate = std::make_unique<Host>();
            if (!members || !candidate->world.load(*m_device, m_context.unpackedRoot, *level) ||
                !candidate->scene.open(*m_device, m_context, candidate->world, *members,
                                       m_options) ||
                !candidate->scene.bindReplicationResources(
                    candidate->projectiles, candidate->pickups, candidate->fixtures) ||
                !candidate->driver.bind(candidate->scene, match, candidate->pickups,
                                        candidate->fixtures)) {
                fail(Failure::Assets);
                return false;
            }
            m_host = std::move(candidate);
        } else {
            if (profiles == nullptr ||
                !m_replica.open(*m_device, m_context.unpackedRoot, *level, context, *profiles,
                                m_context.strings) ||
                !m_clock.begin(match)) {
                fail(Failure::Assets);
                return false;
            }
        }
    } catch (const std::exception& error) {
        log::error("Online stage load failed: {}", error.what());
        fail(Failure::Assets);
        return false;
    }
    m_epoch = context.epoch;
    m_travelMembers.clear();
    m_outcome = PlayReplication::Result::Held;
    if (!match.loaded()) {
        fail(Failure::Session);
        return false;
    }
    log::info("Online stage loaded: {} (epoch {}, {})", level->name, m_epoch,
              match.host() ? "host" : "guest");
    return true;
}
bool OnlinePlay::travel() {
    if (!m_host || m_outcome != PlayReplication::Result::Travel) {
        return false;
    }
    const auto& scene = m_host->scene;
    const auto& destination = scene.destination();
    // Results/shop and secret-level return need their own synchronized owners.
    // Never discard a suspended parent stage or skip the completion screens.
    if (scene.secretTravel() || destination.isTower()) {
        return false;
    }
    const auto id = sceneId(*m_context.levels, destination);
    auto members = scene.party();
    const auto& world = m_host->world;
    if (!world.isTower()) {
        const auto* info = world.level();
        recordLevelBeaten(members, world.ref().realmId, world.ref().index,
                          info != nullptr ? info->rune : 0, info != nullptr ? info->legend : 0);
    }
    MatchParty profiles;
    for (const auto& member : members) {
        if (member.player < 0 || member.player >= static_cast<s32>(profiles.size()) ||
            profiles[static_cast<usize>(member.player)]) {
            fail(Failure::Party);
            return false;
        }
        auto& profile = profiles[static_cast<usize>(member.player)];
        profile = CharacterProfile::capture(member.save);
        if (!profile) {
            fail(Failure::Party);
            return false;
        }
    }
    if (!id || !m_session->match().prepare(*id, MatchTransition::Travel, &profiles)) {
        fail(Failure::Session);
        return false;
    }
    m_travelMembers = std::move(members);
    m_options = {};
    m_options.welcome = false;
    m_options.arriving = true;
    m_options.arrivalWorld = static_cast<u32>(std::max(world.ref().realmId, 0));
    quiet();
    m_phase = Phase::Loading;
    return true;
}
OnlinePlay::Phase OnlinePlay::update(const SessionInputs::Frame& devices) {
    if (m_session == nullptr) {
        return m_phase;
    }
    auto gameplay = devices;
    for (usize device = 0; device < gameplay.size(); ++device) {
        if ((m_pausedDevices & (1U << device)) != 0) {
            gameplay[device] = {};
        }
    }
    m_session->update(1.0 / 60);
    if (m_phase == Phase::Failed) {
        return m_phase;
    }
    auto& match = m_session->match();
    if (m_session->phase() != OnlineSession::Phase::Active ||
        match.phase() == MatchSession::Phase::Stopped) {
        return fail(Failure::Session);
    }
    if (match.phase() == MatchSession::Phase::Loading && match.context().epoch != m_epoch) {
        quiet();
        m_phase = Phase::Loading;
        if (!load(gameplay)) {
            return m_phase;
        }
    }
    if (match.phase() == MatchSession::Phase::Paused) {
        if (m_phase != Phase::Paused) {
            quiet();
        }
        m_phase = Phase::Paused;
        return m_phase;
    }
    if (match.phase() != MatchSession::Phase::Running || m_phase == Phase::Finished) {
        return m_phase;
    }
    if (m_epoch == 0 || m_epoch != match.context().epoch) {
        return fail(Failure::Scene);
    }
    const auto local = m_selection->inputs(*m_session, gameplay);
    if (!local) {
        return fail(Failure::Input);
    }
    m_phase = Phase::Playing;
    if (!match.host()) {
        return MatchInputs::sample(match, *local) ? phase() : fail(Failure::Input);
    }
    if (m_entry.phase() != OnlineLevelEntry::Phase::Closed) {
        m_entry.close();
    }
    m_outcome = m_host->driver.advance(match, *local, m_host->projectiles);
    if (m_outcome == PlayReplication::Result::Failed) {
        return fail(Failure::Capture);
    }
    if (m_outcome != PlayReplication::Result::Advanced &&
        m_outcome != PlayReplication::Result::Held) {
        if (travel() || m_phase == Phase::Failed) {
            return m_phase;
        }
        quiet();
        m_phase = Phase::Finished;
    }
    return phase();
}
bool OnlinePlay::pause(s32 device) {
    if (m_session == nullptr || m_phase != Phase::Playing || device < -1 ||
        device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    u32 mask = 0;
    for (const auto seat : m_session->seats()) {
        if (const auto local = m_selection->device(*m_session, seat);
            local && (device == -1 || *local == device)) {
            mask |= 1U << static_cast<u32>(*local);
        }
    }
    m_pausedDevices |= mask;
    return mask != 0;
}
bool OnlinePlay::resume(s32 device) {
    if (device < -1 || device >= static_cast<s32>(InputCommand::kSeats)) {
        return false;
    }
    const u32 mask = device == -1 ? m_pausedDevices : (1U << static_cast<u32>(device));
    const bool captured = (m_pausedDevices & mask) != 0;
    m_pausedDevices &= ~mask;
    return captured;
}
void OnlinePlay::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height,
                        f64 seconds, f32 frameBlend) {
    if (m_phase == Phase::Closed || m_phase == Phase::Failed) {
        return;
    }
    if (m_phase == Phase::Loading && m_entry.visible()) {
        m_entry.render(device, projection, width, height);
        return;
    }
    if (m_host) {
        m_host->scene.render(device, projection, width, height, m_phase == Phase::Paused,
                             m_phase == Phase::Playing ? frameBlend : 1);
        return;
    }
    if (m_session == nullptr) {
        return;
    }
    if (const auto sample = m_clock.sample(m_session->match(), seconds)) {
        if (!m_replica.show(*sample)) {
            if (const auto* view = m_replica.view()) {
                log::error("Online presentation rejected: {}", view->rejection(*sample));
                for (usize seat = 0; seat < sample->players.size(); ++seat) {
                    if (sample->players[seat]) {
                        const auto& animation = sample->players[seat]->animation;
                        const auto* figure = view->actors().playerFigure(static_cast<u8>(seat));
                        log::error(
                            "Pose seat {} action {} sequence {} frame {} generation {} frames {}",
                            seat, animation.action, animation.sequence, animation.frame,
                            animation.generation,
                            figure != nullptr &&
                                    animation.sequence < figure->actionTree()->sequences.size()
                                ? figure->actionTree()->sequences[animation.sequence].frames
                                : -1);
                    }
                }
            }
            fail(Failure::Presentation);
            return;
        }
        if (m_entry.phase() != OnlineLevelEntry::Phase::Closed) {
            m_entry.close();
        }
    }
    if (m_entry.visible()) {
        m_entry.render(device, projection, width, height);
        return; // Keep the preview until the first complete new-epoch checkpoint.
    }
    // Native texture sequences use game frames, not monitor frames.
    const f32 textureFrame =
        static_cast<f32>(m_clock.tick()) / 2 + static_cast<f32>(m_clock.fraction()) / 2;
    static const GameConfig kDefaultVideo;
    m_replica.draw(device, projection, width, height, textureFrame,
                   m_context.config != nullptr ? *m_context.config : kDefaultVideo);
}
const PlayScene* OnlinePlay::hostScene() const {
    return m_host ? &m_host->scene : nullptr;
}
const CombatSnapshot* OnlinePlay::shown() const {
    if (m_host) {
        return m_host->driver.latest();
    }
    const auto* view = m_replica.view();
    return view != nullptr ? view->shown() : nullptr;
}
} // namespace gdl::game
