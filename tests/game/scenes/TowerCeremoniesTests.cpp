#include <array>
#include <bit>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/MessageTable.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/math/Math.h"

#include "game/audio/SceneSoundEvents.h"
#include "game/audio/SpeechTiming.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerRuntime.h"
#include "game/scenes/TowerCeremonies.h"
#include "game/tower/TowerPromotion.h"
#include "game/tower/TowerRelics.h"

using namespace gdl;
using namespace gdl::game;

namespace {

/** Where the scene's snapshot keeps the ceremonies' two owners. */
struct CeremonySlots {
    TowerPromotion::LocalSnapshot promotion;
    TowerRelics::LocalSnapshot relics;
};

/** The ceremonies' two parts of the scene's roster, each reached through its accessor as the
 * scene reaches it. */
constexpr auto kCeremonyParts = local::parts(
    local::sample("promotion",
                  local::child([](auto& ceremonies) -> auto& { return ceremonies.promotion(); },
                               &CeremonySlots::promotion)),
    local::sample("relics",
                  local::child([](auto& ceremonies) -> auto& { return ceremonies.relics(); },
                               &CeremonySlots::relics)));

} // namespace

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

TEST_CASE("the ceremonies' voices come back with the scene's snapshot",
          "[scenes][tower][ceremonies]") {
    TowerCeremonies ceremonies;
    const SpeechTiming relicSpeech{.remainingSeconds = 3.0};
    const SpeechTiming promotionSpeech{.remainingSeconds = 2.0};
    SoundCueToken relicCue;
    relicCue.event.tick = 7;
    SoundCueToken promotionCue;
    promotionCue.event.tick = 9;
    ceremonies.restoreVoices(relicSpeech, promotionSpeech, relicCue, promotionCue);
    SpeechTiming savedRelicSpeech;
    SpeechTiming savedPromotionSpeech;
    SoundCueToken savedRelicCue;
    SoundCueToken savedPromotionCue;
    ceremonies.saveVoices(savedRelicSpeech, savedPromotionSpeech, savedRelicCue, savedPromotionCue);
    CHECK(savedRelicSpeech == relicSpeech);
    CHECK(savedPromotionSpeech == promotionSpeech);
    CHECK(savedRelicCue == relicCue);
    CHECK(savedPromotionCue == promotionCue);
    // The voices' clocks advance each frame and come back with the snapshot.
    ceremonies.advanceSpeech(1.0f);
    CHECK(ceremonies.relicVoice().timing != relicSpeech);
    CHECK(ceremonies.promotionVoice().timing != promotionSpeech);
    ceremonies.restoreVoices(savedRelicSpeech, savedPromotionSpeech, savedRelicCue,
                             savedPromotionCue);
    CHECK(ceremonies.relicVoice().timing == relicSpeech);
    CHECK(ceremonies.promotionVoice().timing == promotionSpeech);
    CHECK(ceremonies.relicVoice().cue == relicCue);
    CHECK(ceremonies.promotionVoice().cue == promotionCue);
}

TEST_CASE("the ceremonies' promotion and relics come back through a roster like the scene's",
          "[scenes][tower][ceremonies]") {
    std::array<PlayerRuntime, 1> players;
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    players[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    TowerCeremonies ceremonies;
    // A promotion due, so that its clock has somewhere to go.
    ceremonies.promotion().begin(players, MessageTable{});
    REQUIRE(ceremonies.promotion().active());
    CeremonySlots saved;
    REQUIRE(local::save(ceremonies, kCeremonyParts, saved, local::noValues()));
    CHECK(local::admits(ceremonies, kCeremonyParts, saved, local::noValues()));
    const TowerCeremonies foreign;
    CHECK_FALSE(local::admits(foreign, kCeremonyParts, saved, local::noValues()));
    const usize shown = ceremonies.promotion().shown();
    ceremonies.promotion().update(240, false);
    REQUIRE(ceremonies.promotion().shown() != shown);
    local::restore(ceremonies, kCeremonyParts, saved, local::noValues());
    CHECK(ceremonies.promotion().shown() == shown);
    CHECK(ceremonies.promotion().active());
    CHECK_FALSE(ceremonies.relics().active());
    // Forgetting the ceremonies binds both owners anew, so what was saved before goes.
    ceremonies.clearCeremonies();
    CHECK_FALSE(ceremonies.promotion().active());
    CHECK_FALSE(local::admits(ceremonies, kCeremonyParts, saved, local::noValues()));
}
