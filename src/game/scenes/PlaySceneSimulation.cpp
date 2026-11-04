#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>

#include "engine/io/NativeStateDigest.h"

#include "game/party/PlayerPowerups.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {
namespace {
/** Explicit, fixed-width local schema. Never hash native padding, addresses,
 * process-local sound handles, profiling counters, or object representation. */
class ControlDigest : public NativeStateDigest {
public:
    using NativeStateDigest::NativeStateDigest;
    void speech(const SpeechTiming& value) {
        wide(std::bit_cast<u64>(value.remainingSeconds));
        flag(value.looping);
    }
    void speech(std::span<const SpeechTiming> values) {
        wide(values.size());
        for (const auto& value : values) {
            speech(value);
        }
    }
    void camera(const WorldCamera& value) {
        vector(value.position);
        real(value.pitch);
        real(value.yaw);
        real(value.roll);
    }
    void camera(const std::optional<WorldCamera>& value) {
        flag(value.has_value());
        if (value) {
            camera(*value);
        }
    }
    void level(const LevelRef& value) {
        string(value.realm);
        integer(value.realmId);
        string(value.name);
        string(value.title);
        string(value.directory);
        string(value.items);
        string(value.ownItems);
        integer(value.index);
    }
};

} // namespace

SimulationObservation::Digest PlayScene::opponentFollowupDigest() const {
    ControlDigest digest;
    digest.string("LevelOpponents native coordinator values");
    digest.word(2);
    m_opponents.appendNativeFollowupDigest(digest);
    return digest.finish(); // This coordinator contains no borrowed resources.
}

SimulationObservation::Digest PlayScene::simulationControlDigest() const {
    ControlDigest control;
    control.string("PlayScene native control plus partial presentation observation");
    control.word(3); // Local field schema, not a protocol version.
    control.flag(m_open);
    control.flag(m_world != nullptr);
    if (m_world != nullptr) {
        control.level(m_world->ref());
    }
    control.word(simulationTickRate());
    // These are the actual members copied into local history, including
    // hidden camera velocities, RNG, rune cue latches and arbitration clocks.
    control.value(m_sim);
    m_camera.appendNativeDigest(control);
    m_seats.appendNativeDigest(control); // the seats' own cameras, solo or not
    control.flag(PlayerPowerups::timeStopped(m_players));
    control.flag(m_links.stopTimeTotal != nullptr);
    if (m_links.stopTimeTotal != nullptr) {
        control.real(*m_links.stopTimeTotal);
    }
    control.flag(m_arrival.active());
    control.real(m_arrival.titleScale());
    control.wide(m_arrival.effectCount());
    const auto& entrance = m_arrival.camera();
    control.word(static_cast<u32>(entrance.phase()));
    control.word(static_cast<u32>(entrance.mode()));
    control.integer(entrance.ticksLeft());
    control.camera(entrance.camera());
    control.vector(entrance.attention());
    control.flag(m_sumnerVisit.active());
    control.integer(m_sumnerVisit.owner());
    control.word(m_sumnerVisit.menu().revision());
    control.flag(m_sumnerVisit.menu().reading());
    control.flag(m_sumnerVisit.menu().burning());
    control.flag(m_ceremonies.promotion().active());
    control.camera(m_ceremonies.promotion().camera());
    const TowerRelics& relics = m_ceremonies.relics();
    control.flag(relics.active());
    control.word(static_cast<u32>(relics.phase()));
    control.word(relics.displayedRunes());
    control.word(relics.displayedShards());
    control.word(relics.pendingCeremonies());
    control.camera(relics.camera());
    control.flag(m_messages.active());
    const auto message = m_messages.look();
    control.flag(message.has_value());
    if (message) {
        control.word(message->message);
        control.word(message->page);
        control.integer(message->burnFrame);
        control.word(message->promptAlpha);
    }
    control.word(static_cast<u32>(m_transition.phase()));
    control.real(m_transition.opacity());
    control.flag(m_departure.started());
    control.flag(m_departure.finished());
    control.real(m_departure.phase());
    control.word(static_cast<u32>(m_bossSequence.victory().state().stage()));
    control.speech(m_ceremonies.promotionVoice().timing);
    control.speech(m_ceremonies.relicVoice().timing);
    control.speech(m_bossSequence.speechTiming());
    control.speech(m_audio.narrationTiming());
    control.flag(m_audio.narrationHeld());
    control.speech(m_audio.barkTiming());
    return control.finish();
}

bool PlayScene::nativePresentationReady() const {
    if (!m_open || m_world == nullptr || !m_messages.nativeReady() ||
        (m_world->isTower() && !m_sumnerVisit.nativeReady(m_messages.text()))) {
        return false;
    }
    // Offline loading permits missing art, but a native peer must have the
    // action clock which drives releases, reactions and death transitions.
    // Pojo and other intentional forms still retain this base animator.
    return std::ranges::all_of(m_players, [](const auto& player) {
        return player.departed || (player.figure != nullptr && player.figure->animator().bound());
    });
}

bool PlayScene::registerNativeDigestResources() {
    auto& resources = m_nativeDigestResources;
    if (!resources.good() || m_world == nullptr || m_device == nullptr ||
        !resources.registerBuiltinTexture(m_device->whiteTexture(), "opaque-white")) {
        return false;
    }
    const auto archive = [&](const ItemArchive& source, std::string_view key) {
        // Missing optional art has no borrowed native pointers to register.
        return !source.loaded() || resources.registerArchive(source, key);
    };
    const auto stage = std::format("stage/{}", m_world->ref().name);
    usize slot = 0;
    for (const auto* source : m_world->itemScope().archives()) {
        if (!archive(*source, std::format("{}/items/{}", stage, slot++))) {
            return false;
        }
    }
    if (!archive(m_world->powerups(), "powerups") || !archive(m_weapons, "weapons") ||
        !resources.registerTextures(m_staticTextures, "static") ||
        (!m_world->realmItems().loaded() &&
         !resources.registerTextures(m_world->realmItems().textures, stage + "/empty-realm")) ||
        !resources.registerTextures(m_world->textures(), stage + "/textures") ||
        !resources.registerCollision(m_world->collision(), stage) ||
        !resources.registerLayout(m_world->layout(), stage) ||
        !resources.registerHazards(m_world->hazards(), stage)) {
        return false;
    }
    if (!m_opponents.enemies().registerNativeResources(resources, stage + "/swarm")) {
        return false;
    }
    for (auto& [name, items] : m_borrowedItems) {
        usize lender = 0;
        for (const auto* source : items->scope().archives()) {
            if (!archive(*source, std::format("console/{}/items/{}", name, lender++))) {
                return false;
            }
        }
    }
    for (const auto& player : m_players) {
        if (player.figure && !player.figure->registerNativeResources(resources)) {
            return false;
        }
        if (player.mikeyFigure && !player.mikeyFigure->registerNativeResources(resources)) {
            return false;
        }
    }
    if (!m_ceremonies.registerRetainedFigures(resources)) {
        return false;
    }
    return m_opponents.critters().registerNativeResources(resources, stage + "/critters") &&
           m_opponents.bosses().registerNativeResources(resources, stage + "/bosses") &&
           m_opponents.generators().registerNativeResources(resources) &&
           m_opponents.statues().registerNativeResources(resources) &&
           m_world->registerNativeResources(resources) &&
           m_fixtures.registerNativeResources(resources) &&
           m_arsenal.registerNativeResources(resources) &&
           m_portals.registerNativeResources(resources) &&
           m_ceremonies.relics().registerNativeResources(resources) &&
           m_effects.registerNativeResources(resources) && resources.good();
}

std::optional<SimulationObservation::Digest>
PlayScene::nativeAgreementDigest(NativeDigestProfile* profile) {
    if (profile) {
        profile->samples.clear();
    }
    if (!m_open || m_world == nullptr) {
        return std::nullopt;
    }
    bool registered = false;
    if (profile) {
        const auto began = std::chrono::steady_clock::now();
        registered = registerNativeDigestResources();
        profile->samples.push_back(
            {"resources",
             std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - began)
                 .count(),
             std::nullopt});
    } else {
        registered = registerNativeDigestResources();
    }
    if (!registered) {
        return std::nullopt;
    }
    NativeStateDigest digest{m_nativeDigestResources.resources()};
    if (profile) {
        // Nothing is hashed before the scene's owners: this is the empty stream's digest.
        profile->samples.back().runningDigest = digest.checkedFinish();
    }
    if (!appendSceneDigest(digest, nullptr, profile)) {
        return std::nullopt;
    }
    return digest.checkedFinish();
}

} // namespace gdl::game
