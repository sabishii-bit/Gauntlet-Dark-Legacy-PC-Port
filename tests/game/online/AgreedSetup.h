#pragma once

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "game/config/GameConfig.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/netplay/OnlineSession.h"
#include "game/online/NativePlaySetup.h"
#include "game/scenes/PlayScene.h"

namespace gdl::test {
/** The native setup this machine builds, from its own config and options, for the contract its
 * session agreed; the case fails when the setup is refused. The session must hold the contract,
 * its room and its party. */
inline std::unique_ptr<game::NativePlaySetup>
agreedSetup(const game::OnlineSession& session, const game::GameConfig& config,
            const game::PlayOptions& options, const game::MechanicCatalog* mechanics = nullptr) {
    const auto& agreed = *session.nativeContract();
    const game::NativePlaySetup::Contract contract{
        .scene = agreed.rules.scene,
        .autoMelee = agreed.rules.autoMelee,
        .autoActivateItems = agreed.rules.autoActivateItems,
        .expandedShop = agreed.rules.expandedShop,
        .autoAim = agreed.rules.autoAim,
        .build = agreed.identity.build,
        .content = agreed.identity.content,
        .data = agreed.identity.data,
        .mechanics = mechanics,
    };
    auto setup = game::NativePlaySetup::create(config, session.room()->settings, *session.party(),
                                               options, contract);
    REQUIRE(setup);
    return setup;
}

/** Builds the agreed setup and confirms its digest to the session; the setup is the caller's to
 * keep or drop. */
inline std::unique_ptr<game::NativePlaySetup>
confirmAgreedSetup(game::OnlineSession& session, const game::GameConfig& config,
                   const game::PlayOptions& options,
                   const game::MechanicCatalog* mechanics = nullptr) {
    auto setup = agreedSetup(session, config, options, mechanics);
    REQUIRE(session.confirmNativeSetup(setup->digest()));
    return setup;
}
} // namespace gdl::test
