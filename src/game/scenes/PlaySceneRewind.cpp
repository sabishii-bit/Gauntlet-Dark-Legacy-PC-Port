#include <chrono>
#include <utility>

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

std::optional<SimulationObservation::Digest>
PlayScene::nativeLocalDigest(const LocalSnapshot& saved) {
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
        profile->samples.push_back(
            {name, std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - began)
                       .count()});
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
                                      challengeHud, camera, transition, relicSpeech,
                                      promotionSpeech, relicCue, promotionCue, previousCamera,
                                      seatCameras, previousSeatView, previousBossCamera,
                                      namesHeld] = *data;
    }
    measured("control", [&] {
        digest.string("PlayScene.native.8");
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
    const auto child = [&](std::string_view name, const auto& owner, auto member) {
        const bool valid = measured(name, [&] {
            return data ? owner.appendLocalDigest(digest, data->*member)
                        : owner.appendNativeDigest(digest);
        });
        if (!valid) {
            digest.invalidate();
        }
    };
    using Data = LocalSnapshot::Data;
    child("world", *m_world, &Data::world);
    child("fixtures", m_fixtures, &Data::fixtures);
    child("console-objects", m_spawned, &Data::spawned);
    child("opponents", m_opponents, &Data::opponents);
    child("attacks", m_attacks, &Data::attacks);
    child("arsenal", m_arsenal, &Data::arsenal);
    child("effects", m_effects, &Data::effects);
    child("portals", m_portals, &Data::portals);
    child("transporters", m_transporters, &Data::transporters);
    child("departure", m_departure, &Data::departure);
    child("arrival", m_arrival, &Data::arrival);
    child("sumner", m_sumner, &Data::sumner);
    child("hud", m_hud, &Data::hud);
    child("messages", m_messages, &Data::messages);
    child("sumner-visit", m_sumnerVisit, &Data::sumnerVisit);
    child("promotion", m_ceremonies.promotion(), &Data::promotion);
    child("relics", m_ceremonies.relics(), &Data::relics);
    child("boss-sequence", m_bossSequence, &Data::bossSequence);
    child("audio", m_audio, &Data::audio);
    child("challenge-hud", m_challengeHud, &Data::challengeHud);
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
    m_world->saveLocal(data.world);
    m_fixtures.saveLocal(data.fixtures);
    m_spawned.saveLocal(data.spawned);
    if (!m_opponents.saveLocal(data.opponents)) {
        data.owner = nullptr;
        return false;
    }
    m_attacks.saveLocal(data.attacks);
    m_arsenal.saveLocal(data.arsenal);
    m_effects.saveLocal(data.effects);
    m_portals.saveLocal(data.portals);
    m_transporters.saveLocal(data.transporters);
    m_departure.saveLocal(data.departure);
    m_arrival.saveLocal(data.arrival);
    m_sumner.saveLocal(data.sumner);
    m_hud.saveLocal(data.hud);
    m_messages.saveLocal(data.messages);
    m_sumnerVisit.saveLocal(data.sumnerVisit);
    m_ceremonies.saveLocal(data.promotion, data.relics, data.relicSpeech, data.promotionSpeech,
                           data.relicCue, data.promotionCue);
    m_bossSequence.saveLocal(data.bossSequence);
    m_audio.saveLocal(data.audio);
    m_challengeHud.saveLocal(data.challengeHud);
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
    return m_world->canRestoreLocal(data.world) && m_fixtures.canRestoreLocal(data.fixtures) &&
           m_spawned.canRestoreLocal(data.spawned) && m_opponents.canRestoreLocal(data.opponents) &&
           m_attacks.canRestoreLocal(data.attacks) && m_arsenal.canRestoreLocal(data.arsenal) &&
           m_effects.canRestoreLocal(data.effects) && m_portals.canRestoreLocal(data.portals) &&
           m_transporters.canRestoreLocal(data.transporters) &&
           m_departure.canRestoreLocal(data.departure) && m_arrival.canRestoreLocal(data.arrival) &&
           m_sumner.canRestoreLocal(data.sumner) && m_hud.canRestoreLocal(data.hud) &&
           m_messages.canRestoreLocal(data.messages) &&
           m_sumnerVisit.canRestoreLocal(data.sumnerVisit) &&
           m_ceremonies.canRestoreLocal(data.promotion, data.relics) &&
           m_bossSequence.canRestoreLocal(data.bossSequence) &&
           m_audio.canRestoreLocal(data.audio) && m_challengeHud.canRestoreLocal(data.challengeHud);
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
    m_world->restoreLocal(data.world);
    m_fixtures.restoreLocal(data.fixtures);
    m_spawned.restoreLocal(data.spawned);
    m_opponents.restoreLocal(data.opponents);
    m_attacks.restoreLocal(data.attacks);
    m_arsenal.restoreLocal(data.arsenal);
    m_effects.restoreLocal(data.effects);
    m_portals.restoreLocal(data.portals);
    m_transporters.restoreLocal(data.transporters);
    m_departure.restoreLocal(data.departure);
    m_arrival.restoreLocal(data.arrival);
    m_sumner.restoreLocal(data.sumner);
    m_hud.restoreLocal(data.hud);
    m_messages.restoreLocal(data.messages);
    m_sumnerVisit.restoreLocal(data.sumnerVisit);
    m_ceremonies.restoreLocal(data.promotion, data.relics, data.relicSpeech, data.promotionSpeech,
                              data.relicCue, data.promotionCue);
    m_bossSequence.restoreLocal(data.bossSequence);
    m_audio.restoreLocal(data.audio);
    m_challengeHud.restoreLocal(data.challengeHud);
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
