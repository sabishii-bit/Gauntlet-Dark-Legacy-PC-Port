#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"

#include "game/characters/LevelResults.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/LevelWorld.h"
#include "game/menu/PauseMenu.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/AttractScene.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayerSelectScene.h"
#include "game/scenes/TitleScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `Screen::open` takes `Services`, a temporary or not, before the other `Args`. */
template <typename Screen, typename Services, typename... Args>
constexpr bool kOpens =
    requires(Screen& screen, RenderDevice& device, Services&& services, Args&... args) {
        screen.open(device, std::forward<Services>(services), args...);
    };
template <typename Services>
constexpr bool kAttractOpens =
    requires(AttractScene& screen, RenderDevice& device, Services&& services) {
        screen.openNext(device, std::forward<Services>(services));
    };
template <typename Services>
constexpr bool kSelectReopens = requires(PlayerSelectScene& screen, RenderDevice& device,
                                         Services&& services, std::span<const PartyMember> party) {
    screen.openAfterLevel(device, std::forward<Services>(services), party);
};
using Members = std::span<const PartyMember>;
using Results = std::span<const LevelResults>;
using Maxima = const std::array<s32, 3>;

// Every screen keeps the view it is opened with, so none takes a temporary one.
static_assert(kOpens<TitleScene, const GameServices&>);
static_assert(!kOpens<TitleScene, GameServices>);
static_assert(kOpens<PauseMenu, const GameServices&, Members, s32>);
static_assert(!kOpens<PauseMenu, GameServices, Members, s32>);
static_assert(kOpens<PlayerSelectScene, const GameServices&, s32>);
static_assert(!kOpens<PlayerSelectScene, GameServices, s32>);
static_assert(kSelectReopens<const GameServices&>);
static_assert(!kSelectReopens<GameServices>);
static_assert(
    kOpens<AfterLevelScene, const GameServices&, Members, Results, Maxima, std::string_view>);
static_assert(!kOpens<AfterLevelScene, GameServices, Members, Results, Maxima, std::string_view>);
static_assert(kAttractOpens<const GameServices&>);
static_assert(!kAttractOpens<GameServices>);
static_assert(kOpens<PlayScene, const GameServices&, LevelWorld, Members>);
static_assert(!kOpens<PlayScene, GameServices, LevelWorld, Members>);

TEST_CASE("a screen not yet opened reads a view with nothing in it",
          "[game][context][game-services]") {
    const GameServices& none = GameServices::none();
    CHECK(&none == &GameServices::none());
    CHECK(none.config == nullptr);
    CHECK(none.strings == nullptr);
    CHECK(none.controlLabels == nullptr);
    CHECK(none.sounds == nullptr);
    CHECK(none.movieMixer == nullptr);
    CHECK(none.assets == nullptr);
    CHECK(none.levels == nullptr);
    CHECK(none.mechanics == nullptr);
    CHECK(none.unpackedRoot.empty());
}

TEST_CASE("a copied view points elsewhere without moving the one it came from",
          "[game][context][game-services]") {
    const GameConfig config;
    const GameConfig online;
    const StringTable strings;
    const GameServices game{.config = &config, .strings = &strings, .unpackedRoot = "unpacked"};
    GameServices copy = game;
    copy.config = &online;
    CHECK(game.config == &config);
    CHECK(copy.config == &online);
    CHECK(copy.strings == &strings);
    CHECK(copy.unpackedRoot == game.unpackedRoot);
}
} // namespace
