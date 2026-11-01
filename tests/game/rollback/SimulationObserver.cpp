// Extended local diagnostics only. Production sessions use the direct live
// watchdog and never capture or transfer these represented checkpoint states.
#include "game/rollback/SimulationObserver.h"

#include <memory>
#include <sstream>
#include <utility>

#include "game/enemies/EnemyMissilesCheckpoint.h"
#include "game/enemies/OpponentResources.h"
#include "game/rollback/CanonicalDigestSink.h"
#include "game/rollback/EnemyPoolCheckpoint.h"
#include "game/rollback/GeneratorCheckpoint.h"
#include "game/rollback/PlacedEnemyCheckpoint.h"

namespace gdl::game {
namespace {
using Subsystem = SimulationObservation::Subsystem;

Subsystem unavailable(std::string name, std::string reason) {
    return {std::move(name), std::nullopt, std::move(reason)};
}
Subsystem observed(std::string name, std::optional<SimulationObservation::Digest> digest,
                   std::string reason) {
    if (!digest) {
        return unavailable(std::move(name), std::move(reason));
    }
    return {std::move(name), digest, {}};
}
template <class Codec, class State>
Subsystem captured(std::string name, const std::optional<State>& state) {
    if (!state) {
        return unavailable(std::move(name), "native checkpoint refused current state or binding");
    }
    return observed(std::move(name), Codec::canonicalDigest(*state),
                    "canonical field walker refused its record or byte bounds");
}

} // namespace

class SimulationCheckpointObserver {
public:
    static std::optional<OpponentCombatCheckpoint::State>
    adapterValues(const OpponentCombatCheckpoint& adapter,
                  const EffectTrees::PoolCheckpoint& effects) {
        return adapter.captureValues(effects, true);
    }
};
SimulationObservation SimulationObserver::observe() {
    if (m_generation != m_scene.m_openGeneration) {
        clear();
    }
    SimulationObservation result;
    result.unobserved = {
        "PartyHud: selectors, help/message queues, pickup cards, hourglass and relic HUD clocks",
        "LevelArrivalPresentation/StartCamera: hidden hold/ride/title clocks and spawn particles",
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): one entry wrapped across two lines
        "SumnerVisit/SumnerHints, SumnerFigure, TowerPromotion and TowerRelics: "
        "hidden clocks, queues, figures and ceremony effects",
        "LevelMessages/ScrollBox: hidden clocks and pending cues",
        "Bosses, BossSequence/LegendRite/BossMeters and SkorneArena owners",
        "LevelOpponents Death/yell playback handles and their external audio state",
        "LevelSoundscape music/ambience, TriggerCues and external sound/mixer playback, "
        "callbacks and output queues (logical speech timing is observed)",
        "Render-history inputs (presented clip/previous camera) and retained promotion figures",
        "Application travel/shop/secret-parent ownership, persistence/unlock callbacks, config "
        "and input-source state outside the sampled scene scalars",
    };
    result.subsystems.reserve(8);

    const auto combat = [&]() -> Subsystem {
        if (!m_scene.m_open) {
            return unavailable("party_combat", "scene is closed");
        }
        if (!m_combatResources) {
            return unavailable("party_combat", "load-time combat resource catalog is not bound");
        }
        if (!m_combatResources->matches(m_scene.m_players)) {
            return unavailable("party_combat", "combat resource catalog no longer matches party");
        }
        std::vector<std::unique_ptr<PlayerRuntimeCheckpoint>> owners;
        std::vector<PlayerRuntimeCheckpoint*> players;
        owners.reserve(m_scene.m_players.size());
        players.reserve(m_scene.m_players.size());
        for (auto& player : m_scene.m_players) {
            owners.push_back(std::make_unique<PlayerRuntimeCheckpoint>(
                player, m_scene.m_classes.stats(player.actor.save().character)));
            players.push_back(owners.back().get());
        }
        const CombatRollbackCheckpoint capture{players,
                                               m_scene.m_sim.health,
                                               m_scene.m_sim.levels,
                                               m_scene.m_attacks,
                                               m_scene.m_arsenal,
                                               m_scene.m_effects,
                                               m_combatResources->resources()};
        auto state = capture.checkpoint();
        if (!state) {
            return unavailable("party_combat", "party/combat checkpoint refused current native "
                                               "state, resources or cross-owner references");
        }
        const OpponentCombatCheckpoint adapter{m_scene.m_opponents,
                                               m_combatResources->resources().sharedEffects};
        auto adapterState = SimulationCheckpointObserver::adapterValues(adapter, state->effects);
        if (!adapterState) {
            // Distinguish an ordinary-only diagnostic contract from a broken
            // native actor. These probes are read-only and do not broaden the
            // old adapter's prepare/restore domain.
            auto& opponents = m_scene.m_opponents;
            const CrittersCheckpoint critters{opponents.m_critters};
            const auto population = critters.checkpoint();
            const CombatantProjectilesCheckpoint missiles{
                opponents.m_combatantProjectiles, m_scene.m_effects, critters.resources(),
                m_combatResources->resources().sharedEffects};
            const auto flights = missiles.checkpoint(state->effects);
            const auto statues = opponents.m_statues.checkpoint();
            std::ostringstream reason;
            reason << "represented opponent adapter refused: critters=" << bool(population)
                   << ", projectiles=" << bool(flights) << ", statues=" << bool(statues);
            for (usize index = 0; index < opponents.m_sim.critterEffects.size(); ++index) {
                const auto& effect = opponents.m_sim.critterEffects[index];
                if (effect.ofBoss) {
                    reason << "; critterEffects[" << index
                           << "].ofBoss=true (critter=" << effect.critter
                           << ", effect=" << effect.effect << ") outside ordinary adapter domain";
                    break;
                }
            }
            for (usize index = 0; index < opponents.m_sim.moveEffects.size(); ++index) {
                const auto& effect = opponents.m_sim.moveEffects[index];
                if (effect.ofBoss) {
                    reason << "; moveEffects[" << index
                           << "].ofBoss=true (critter=" << effect.critter
                           << ", effect=" << effect.effect << ") outside ordinary adapter domain";
                    break;
                }
            }
            return unavailable("party_combat", reason.str());
        }
        const CombatStatePacket::State values{std::move(*state), std::move(*adapterState)};
        // Each trusted read-only capture validated its own leaves. Reapplying
        // the old packet's ordinary-only admission would reject live boss
        // effects/projectiles; keep all cross-owner checks without authorizing
        // any wider encode/decode or restore domain.
        if (!values.references()) {
            return unavailable("party_combat",
                               "captured combat/adapter cross-owner validation failed");
        }
        const auto body = CombatStatePacket::canonicalDigest(values.combat, values.adapter, true);
        if (!body) {
            return unavailable("party_combat",
                               "canonical combat field walker refused its record or byte bounds");
        }
        // Local composite schema 3: the opponents' native follow-up values are their own
        // subsystem below, so either can change without hiding the other.
        CanonicalDigestSink digest;
        digest.push_back(3);
        digest.append(*body);
        return observed("party_combat", digest.finish(), {});
    };
    result.subsystems.push_back(combat());
    // The native cloud/Death follow-up values; not part of the replay adapter's restore contract.
    result.subsystems.push_back(
        m_scene.m_open ? observed("opponent_followup", m_scene.opponentFollowupDigest(), {})
                       : unavailable("opponent_followup", "scene is closed"));

    if (m_scene.m_open) {
        result.subsystems.push_back(captured<GeneratorCheckpoint>(
            "opponent_generators", m_scene.m_opponents.generators().checkpoint()));
        result.subsystems.push_back(captured<EnemyPoolCheckpoint>(
            "opponent_swarm", m_scene.m_opponents.enemies().checkpoint()));
        const auto resources = OpponentResources::missiles(m_scene.m_opponents.enemies());
        result.subsystems.push_back(captured<EnemyMissilesCheckpoint>(
            "opponent_missiles", m_scene.m_opponents.missiles().checkpoint(resources)));
        result.subsystems.push_back(captured<PlacedEnemyCheckpoint>(
            "opponent_placements",
            PlacedEnemiesCheckpoint::capture(m_scene.m_opponents.placements())));
    } else {
        for (const auto* const name : {"opponent_generators", "opponent_swarm", "opponent_missiles",
                                       "opponent_placements"}) {
            result.subsystems.push_back(unavailable(name, "scene is closed"));
        }
    }

    const auto stage = [&]() -> Subsystem {
        if (!m_scene.m_open || m_scene.m_world == nullptr) {
            return unavailable("stage", "scene is closed or has no world");
        }
        if (!m_combatWorld || !m_combatFixtures || !m_combatCamera || !m_combatPortals ||
            !m_combatTransporters || !m_combatStageCatalog.valid()) {
            return unavailable("stage", "load-time world/fixture/camera/portal/transporter "
                                        "checkpoint owners or trusted stage catalog are not bound");
        }
        if (!m_combatWorld->bindsObservation(*m_scene.m_world) ||
            !m_combatFixtures->binds(m_scene.m_fixtures) ||
            !m_combatCamera->binds(m_scene.m_camera, *m_scene.m_world) ||
            !m_combatPortals->binds(m_scene.m_portals) ||
            !m_combatTransporters->binds(m_scene.m_transporters)) {
            return unavailable("stage", "stage checkpoint owners no longer match live bindings");
        }
        auto world = m_combatWorld->observe();
        auto fixtures = m_combatFixtures->checkpoint();
        auto camera = m_combatCamera->checkpoint();
        auto portals = m_combatPortals->checkpoint();
        auto transporters = m_combatTransporters->checkpoint();
        std::string failures;
        const auto failed = [&](bool available, std::string_view name) {
            if (!available) {
                if (!failures.empty()) {
                    failures += "; ";
                }
                failures += name;
                failures += " checkpoint refused current native state or resources";
            }
        };
        failed(world.has_value(), "world");
        failed(fixtures.has_value(), "fixtures");
        failed(camera.has_value(), "camera");
        failed(portals.has_value(), "portals");
        failed(transporters.has_value(), "transporters");
        if (!failures.empty()) {
            return unavailable("stage", std::move(failures));
        }
        StageStatePacket::State values{std::move(*world), std::move(*fixtures), std::move(camera),
                                       std::move(portals), std::move(transporters)};
        if (!values.world.validObservation() || !values.fixtures.valid() ||
            !values.camera->valid() || values.camera->level != values.world.level ||
            !values.portals->valid() || !values.transporters->valid()) {
            return unavailable("stage", "captured stage validation failed");
        }
        const auto body = StageStatePacket::canonicalDigest(values, m_combatStageCatalog, true);
        if (!body) {
            return unavailable("stage", "canonical stage field walker refused catalog identity, "
                                        "record or decoded-byte bounds");
        }
        // Geometry is catalog-referenced in the canonical body. Its trusted
        // fingerprint is part of this diagnostic, never merely an external label.
        CanonicalDigestSink digest;
        digest.append(m_combatStageCatalog.fingerprint());
        digest.append(*body);
        return observed("stage", digest.finish(), {});
    };
    result.subsystems.push_back(stage());

    result.subsystems.push_back(observed("scene_control", m_scene.simulationControlDigest(), {}));
    return result;
}

} // namespace gdl::game
