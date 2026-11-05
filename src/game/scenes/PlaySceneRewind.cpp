#include <chrono>
#include <optional>
#include <string_view>

#include "engine/io/LocalRoster.h"

#include "game/scenes/PlayScene.h"

namespace gdl::game {

struct PlayScene::LocalSnapshot::Data {
    const PlayScene* owner = nullptr;
    const LevelWorld* worldOwner = nullptr;
    u64 generation = 0;
    Simulation state;
    struct Participant {
        PlayerSimulation simulation;
        PlayerRuntime::Presentation previous;
        PlayerFigure* figure = nullptr;
        PlayerFigure::LocalSnapshot figureState;
        std::optional<MikeyFigure> mikey;
        BodyGlow glow;
        WeaponGlow weaponGlow;
        u32 deathHeldEffect = 0;
    };
    std::vector<Participant> players;
    LevelWorld::LocalSnapshot world;
    LevelFixtures::LocalSnapshot fixtures;
    SpawnedObjects::LocalSnapshot spawned;
    LevelOpponents::LocalSnapshot opponents;
    PlayerAttacks::LocalSnapshot attacks;
    PlayerArsenal::LocalSnapshot arsenal;
    EffectTrees::LocalSnapshot effects;
    ExitPortals::LocalSnapshot portals;
    LevelTransporters::LocalSnapshot transporters;
    PortalDeparture::LocalSnapshot departure;
    LevelArrivalPresentation::LocalSnapshot arrival;
    SumnerFigure::LocalSnapshot sumner;
    PartyHud::LocalSnapshot hud;
    LevelMessages::LocalSnapshot messages;
    SumnerVisit::LocalSnapshot sumnerVisit;
    TowerPromotion::LocalSnapshot promotion;
    TowerRelics::LocalSnapshot relics;
    BossSequence::LocalSnapshot bossSequence;
    LevelSoundscape::LocalSnapshot audio;
    ChallengeHud::LocalSnapshot challengeHud;
    MechanicsFacade::LocalSnapshot mechanics;
    TowerCamera::State camera;
    TransitionScreen::State transition;
    SpeechTiming relicSpeech;
    SpeechTiming promotionSpeech;
    SoundCueToken relicCue;
    SoundCueToken promotionCue;
    std::optional<WorldCamera> previousCamera;
    SeatCameraStates seatCameras{};
    std::optional<WorldCamera> previousSeatView; ///< this machine's view alone, never digested
    bool previousBossCamera = false;
    bool namesHeld = false;
};

PlayScene::LocalSnapshot::LocalSnapshot() = default;
PlayScene::LocalSnapshot::~LocalSnapshot() = default;
PlayScene::LocalSnapshot::LocalSnapshot(LocalSnapshot&&) noexcept = default;
PlayScene::LocalSnapshot& PlayScene::LocalSnapshot::operator=(LocalSnapshot&&) noexcept = default;

const auto& PlayScene::sceneOwners() {
    using Data = LocalSnapshot::Data;
    // The world is borrowed, and the ceremonies hold two owners, so those three are reached
    // through calls.
    static constexpr auto kOwners = local::parts(
        local::sample("world", local::child([](auto& scene) -> auto& { return *scene.m_world; },
                                            &Data::world)),
        local::sample("fixtures", local::child(&PlayScene::m_fixtures, &Data::fixtures)),
        local::sample("console-objects", local::child(&PlayScene::m_spawned, &Data::spawned)),
        local::sample("opponents", local::child(&PlayScene::m_opponents, &Data::opponents)),
        local::sample("attacks", local::child(&PlayScene::m_attacks, &Data::attacks)),
        local::sample("arsenal", local::child(&PlayScene::m_arsenal, &Data::arsenal)),
        local::sample("effects", local::child(&PlayScene::m_effects, &Data::effects)),
        local::sample("portals", local::child(&PlayScene::m_portals, &Data::portals)),
        local::sample("transporters",
                      local::child(&PlayScene::m_transporters, &Data::transporters)),
        local::sample("departure", local::child(&PlayScene::m_departure, &Data::departure)),
        local::sample("arrival", local::child(&PlayScene::m_arrival, &Data::arrival)),
        local::sample("sumner", local::child(&PlayScene::m_sumner, &Data::sumner)),
        local::sample("hud", local::child(&PlayScene::m_hud, &Data::hud)),
        local::sample("messages", local::child(&PlayScene::m_messages, &Data::messages)),
        local::sample("sumner-visit", local::child(&PlayScene::m_sumnerVisit, &Data::sumnerVisit)),
        local::sample(
            "promotion",
            local::child([](auto& scene) -> auto& { return scene.m_ceremonies.promotion(); },
                         &Data::promotion)),
        local::sample("relics",
                      local::child([](auto& scene) -> auto& { return scene.m_ceremonies.relics(); },
                                   &Data::relics)),
        local::sample("boss-sequence",
                      local::child(&PlayScene::m_bossSequence, &Data::bossSequence)),
        local::sample("audio", local::child(&PlayScene::m_audio, &Data::audio)),
        local::sample("challenge-hud",
                      local::child(&PlayScene::m_challengeHud, &Data::challengeHud)),
        local::sample("mechanics", local::child(&PlayScene::m_mechanics, &Data::mechanics)));
    return kOwners;
}

std::optional<NativeStateDigest::Digest> PlayScene::nativeLocalDigest(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved) || !registerNativeDigestResources()) {
        return std::nullopt;
    }
    for (const auto& player : saved.m_data->players) {
        if (player.mikey && !player.mikey->registerNativeResources(m_nativeDigestResources)) {
            return std::nullopt;
        }
    }
    NativeStateDigest digest{m_nativeDigestResources.resources()};
    if (!appendSceneDigest(digest, &saved)) {
        return std::nullopt;
    }
    return digest.checkedFinish();
}

bool PlayScene::appendSceneDigest(NativeStateDigest& digest, const LocalSnapshot* saved,
                                  NativeDigestProfile* profile) const {
    const auto measured = [&](std::string_view name, const auto& append) {
        if (!profile) {
            return append();
        }
        const auto began = std::chrono::steady_clock::now();
        const bool valid = append();
        const auto ended = std::chrono::steady_clock::now();
        // Hashed after the clock stops, so the timing holds only the owner's own append.
        profile->samples.push_back({name,
                                    std::chrono::duration<f64, std::micro>(ended - began).count(),
                                    valid ? digest.checkedFinish() : std::nullopt});
        return valid;
    };
    const auto* data = saved != nullptr ? saved->m_data.get() : nullptr;
    if (data) {
        // A new captured owner must enter this schema explicitly; never silently
        // let the rewind roster grow beyond the state checked for agreement.
        [[maybe_unused]] const auto& [owner, worldOwner, generation, state, players, world,
                                      fixtures, spawned, opponents, attacks, arsenal, effects,
                                      portals, transporters, departure, arrival, sumner, hud,
                                      messages, sumnerVisit, promotion, relics, bossSequence, audio,
                                      challengeHud, mechanics, camera, transition, relicSpeech,
                                      promotionSpeech, relicCue, promotionCue, previousCamera,
                                      seatCameras, previousSeatView, previousBossCamera,
                                      namesHeld] = *data;
    }
    measured("control", [&] {
        digest.string("PlayScene.native.10");
        digest.value(m_world->ref());
        digest.value(simulationTickRate());
        digest.value(data ? data->state : m_sim);
        return digest.good();
    });
    if (!measured("players", [&] {
            digest.wide(m_players.size());
            for (usize i = 0; i < m_players.size(); ++i) {
                const auto& live = m_players[i];
                if (data) {
                    const auto& [simulation, previous, figure, figureState, mikey, glow, weaponGlow,
                                 deathHeldEffect] = data->players[i];
                    digest.values(simulation, previous);
                    digest.flag(figure != nullptr);
                    if (figure && !figure->appendLocalDigest(digest, figureState)) {
                        return false;
                    }
                    digest.values(mikey, glow, weaponGlow, deathHeldEffect);
                } else {
                    digest.values(live.simulation(), live.previous);
                    digest.flag(live.figure != nullptr);
                    if (live.figure && !live.figure->appendNativeDigest(digest)) {
                        return false;
                    }
                    digest.flag(live.mikeyFigure != nullptr);
                    if (live.mikeyFigure) {
                        digest.value(*live.mikeyFigure);
                    }
                    digest.values(live.glow, live.weaponGlow, live.deathHeldEffect);
                }
            }
            return digest.good();
        })) {
        return false;
    }
    // Each owner is measured on its own, and one that fails spoils the digest without stopping
    // the owners after it.
    const auto child = [&](std::string_view name, const auto& part) {
        const bool valid = measured(name, [&] {
            return local::append(*this, local::parts(part), digest, data, local::noValues());
        });
        if (!valid) {
            digest.invalidate();
        }
    };
    local::eachSample(sceneOwners(), child);
    measured("camera-speech", [&] {
        digest.values(data ? data->camera : m_camera.localState(),
                      data ? data->transition : m_transition.state(),
                      data ? data->relicSpeech : m_ceremonies.relicVoice().timing,
                      data ? data->promotionSpeech : m_ceremonies.promotionVoice().timing,
                      data ? data->relicCue : m_ceremonies.relicVoice().cue,
                      data ? data->promotionCue : m_ceremonies.promotionVoice().cue,
                      data ? data->previousCamera : m_previousCamera,
                      data ? data->seatCameras : seatCameraStates(),
                      data ? data->previousBossCamera : m_previousBossCamera,
                      data ? data->namesHeld : m_names.held());
        return digest.good();
    });
    return digest.good();
}

bool PlayScene::saveLocal(LocalSnapshot& saved) const {
    if (!m_open || m_world == nullptr) {
        saved.m_data.reset();
        return false;
    }
    if (!saved.m_data) {
        saved.m_data = std::make_unique<LocalSnapshot::Data>();
    }
    auto& data = *saved.m_data;
    data.owner = nullptr;
    data.worldOwner = m_world;
    data.generation = m_openGeneration;
    data.state = m_sim;
    data.players.resize(m_players.size());
    for (usize i = 0; i < m_players.size(); ++i) {
        const auto& live = m_players[i];
        auto& player = data.players[i];
        player.simulation = live.simulation();
        player.previous = live.previous;
        player.figure = live.figure.get();
        if (live.figure != nullptr) {
            live.figure->saveLocal(player.figureState);
        }
        if (live.mikeyFigure != nullptr) {
            player.mikey = *live.mikeyFigure;
        } else {
            player.mikey.reset();
        }
        player.glow = live.glow;
        player.weaponGlow = live.weaponGlow;
        player.deathHeldEffect = live.deathHeldEffect;
    }
    // The opponents' save can fail, and a snapshot they could not fill is admitted by nobody.
    if (!local::save(*this, sceneOwners(), data, local::noValues())) {
        data.owner = nullptr;
        return false;
    }
    m_ceremonies.saveVoices(data.relicSpeech, data.promotionSpeech, data.relicCue,
                            data.promotionCue);
    data.camera = m_camera.localState();
    data.transition = m_transition.state();
    data.previousCamera = m_previousCamera;
    data.seatCameras = seatCameraStates();
    data.previousSeatView = m_previousSeatView;
    data.previousBossCamera = m_previousBossCamera;
    data.namesHeld = m_names.held();
    data.owner = this;
    return true;
}

bool PlayScene::canRestoreLocal(const LocalSnapshot& saved) const {
    if (!m_open || !saved.m_data || saved.m_data->owner != this ||
        saved.m_data->worldOwner != m_world || saved.m_data->generation != m_openGeneration ||
        saved.m_data->players.size() != m_players.size()) {
        return false;
    }
    const auto& data = *saved.m_data;
    for (usize i = 0; i < m_players.size(); ++i) {
        const auto& player = data.players[i];
        // Check retained ownership before dereferencing a borrowed costume.
        if (player.simulation.actor.player() != m_players[i].actor.player() ||
            !m_ceremonies.ownsFigure(m_players, i, player.figure) ||
            (player.figure != nullptr && !player.figure->canRestoreLocal(player.figureState))) {
            return false;
        }
    }
    return local::admits(*this, sceneOwners(), data, local::noValues());
}

bool PlayScene::matchesLocalControl(const LocalSnapshot& saved) const {
    return m_open && m_world != nullptr && saved.m_data && saved.m_data->owner == this &&
           saved.m_data->worldOwner == m_world && saved.m_data->generation == m_openGeneration &&
           m_sim == saved.m_data->state;
}

bool PlayScene::restoreLocal(const LocalSnapshot& saved) {
    // No owner is mutated until every same-stage binding has passed preflight.
    // These are ordinary native values; there is no wire decode, resource lookup,
    // prepare/commit protocol, output callback, or GPU upload in this path.
    if (!canRestoreLocal(saved)) {
        return false;
    }
    const auto& data = *saved.m_data;
    local::restore(*this, sceneOwners(), data, local::noValues());
    m_ceremonies.restoreVoices(data.relicSpeech, data.promotionSpeech, data.relicCue,
                               data.promotionCue);
    m_sim = data.state;
    for (usize i = 0; i < m_players.size(); ++i) {
        const auto& player = data.players[i];
        auto& live = m_players[i];
        m_ceremonies.activateFigure(m_players, i, player.figure);
        live.simulation() = player.simulation;
        live.previous = player.previous;
        if (live.figure != nullptr) {
            live.figure->restoreLocal(player.figureState);
        }
        if (player.mikey) {
            if (!live.mikeyFigure) {
                live.mikeyFigure = std::make_unique<MikeyFigure>(*player.mikey);
            } else {
                *live.mikeyFigure = *player.mikey;
            }
        } else {
            live.mikeyFigure.reset();
        }
        live.glow = player.glow;
        live.weaponGlow = player.weaponGlow;
        live.deathHeldEffect = player.deathHeldEffect;
        // Noclip flight is not in the record; it follows the cheats just restored.
        live.ghost = cheats(live.actor.player()).noclip;
    }
    m_camera.restoreLocal(data.camera);
    m_transition.restoreLocal(data.transition);
    m_previousCamera = data.previousCamera;
    m_seats.restore(data.seatCameras);
    m_previousSeatView = data.previousSeatView;
    m_previousBossCamera = data.previousBossCamera;
    m_names.restoreHeld(data.namesHeld);
    return true;
}

} // namespace gdl::game
