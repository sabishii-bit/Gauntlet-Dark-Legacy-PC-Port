#include <array>
#include <bit>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/party/PlayerRuntime.h"
#include "game/scenes/TowerCeremonies.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a character owns its costume, none, or one the ceremonies retired",
          "[scenes][tower][ceremonies]") {
    const TowerCeremonies ceremonies;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    // Nobody here wears a loaded costume; null is everyone's own, strangers are not.
    CHECK(ceremonies.ownsFigure(players, 0, nullptr));
    CHECK(ceremonies.ownsFigure(players, 1, players[1].figure.get()));
    CHECK_FALSE(ceremonies.ownsFigure(players, 2, nullptr));
    const auto* stranger = std::bit_cast<const PlayerFigure*>(&players);
    CHECK_FALSE(ceremonies.ownsFigure(players, 0, stranger));
}

TEST_CASE("retiring a costume keeps it owned and releasing lets it go",
          "[scenes][tower][ceremonies]") {
    TowerCeremonies ceremonies;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    // Retiring the current (empty) costume parks it with the ceremonies; activating the
    // same figure again is a no-op either way.
    ceremonies.activateFigure(players, 0, nullptr);
    CHECK(players[0].figure == nullptr);
    CHECK(ceremonies.ownsFigure(players, 0, nullptr));
    ceremonies.activateFigure(players, 0, nullptr);
    CHECK(players[0].figure == nullptr);
    ceremonies.releaseFigures();
    CHECK(ceremonies.ownsFigure(players, 0, nullptr));
}

TEST_CASE("the ceremonies' snapshot round-trips their values and voices",
          "[scenes][tower][ceremonies]") {
    TowerCeremonies ceremonies;
    TowerPromotion::LocalSnapshot promotion;
    TowerRelics::LocalSnapshot relics;
    SpeechTiming relicSpeech;
    SpeechTiming promotionSpeech;
    SoundCueToken relicCue;
    SoundCueToken promotionCue;
    ceremonies.saveLocal(promotion, relics, relicSpeech, promotionSpeech, relicCue, promotionCue);
    REQUIRE(ceremonies.canRestoreLocal(promotion, relics));
    // The voices' clocks advance each frame and come back with the snapshot.
    ceremonies.advanceSpeech(1.0f);
    ceremonies.restoreLocal(promotion, relics, relicSpeech, promotionSpeech, relicCue,
                            promotionCue);
    CHECK(ceremonies.relicVoice().timing == relicSpeech);
    CHECK(ceremonies.promotionVoice().timing == promotionSpeech);
    CHECK_FALSE(ceremonies.promotion().active());
    CHECK_FALSE(ceremonies.relics().active());
    ceremonies.clearCeremonies();
    CHECK_FALSE(ceremonies.relics().active());
}
