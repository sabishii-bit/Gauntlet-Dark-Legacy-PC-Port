#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>

#include "engine/io/NativeStateDigest.h"

#include "game/scenes/PlayScene.h"

namespace gdl::game {

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

std::optional<NativeStateDigest::Digest>
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
