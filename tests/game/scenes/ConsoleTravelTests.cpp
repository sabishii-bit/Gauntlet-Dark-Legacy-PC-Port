#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/level/LevelCatalog.h"
#include "game/scenes/ConsoleTravel.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr s32 kCastleRealm = 1;
constexpr s32 kTownRealm = 7;
constexpr s32 kWalkedRealm = -1; ///< a Legends map walked, which never indexes the campaign

/** `name` of the realm `realmId`, as the catalogue gives a level. */
LevelRef levelOf(s32 realmId, std::string_view name) {
    LevelRef level;
    level.realmId = realmId;
    level.name = std::string{name};
    level.directory = "LEVELS/LEVEL" + level.name;
    return level;
}

/** One console journey: from where, to where, the level waiting behind a challenge, and what
 * the journey does with it. */
struct Row {
    std::string_view what;
    LevelRef from;
    LevelRef to;
    std::optional<LevelRef> parent;
    ConsoleTravelKind kind = ConsoleTravelKind::Plain;
};

std::array<Row, 14> rows() {
    const LevelRef fields = levelOf(kTownRealm, "G1");
    const LevelRef courtyard = levelOf(kCastleRealm, "A1");
    const LevelRef lab = levelOf(LevelRef::kSecretRealm, "S1");
    const LevelRef cloud = levelOf(LevelRef::kSecretRealm, "S9");
    const LevelRef tower = LevelRef::tower();
    const LevelRef walked = levelOf(kWalkedRealm, "castle1");
    using enum ConsoleTravelKind;
    return {{
        {"level to level", fields, courtyard, std::nullopt, Plain},
        {"level to itself", fields, fields, std::nullopt, Plain},
        {"level to a challenge", fields, lab, std::nullopt, EnterChallenge},
        {"tower to a challenge", tower, lab, std::nullopt, EnterChallenge},
        {"challenge to its parent", lab, fields, fields, ReturnToParent},
        {"challenge to the tower, its parent", lab, tower, tower, ReturnToParent},
        {"challenge to another challenge", lab, cloud, fields, ChallengeToChallenge},
        {"challenge to another level", lab, courtyard, fields, Plain},
        {"challenge to the tower", lab, tower, fields, Plain},
        {"challenge with no parent to a level", lab, fields, std::nullopt, Plain},
        {"level to the level behind no challenge", courtyard, fields, fields, Plain},
        {"Legends map to a level", walked, fields, std::nullopt, Plain},
        {"Legends map to the tower", walked, tower, std::nullopt, Plain},
        {"Legends map to a challenge", walked, lab, std::nullopt, Plain},
    }};
}

} // namespace

TEST_CASE("console travel treats a challenge's parent as the online game does", "[native-travel]") {
    for (const Row& row : rows()) {
        CAPTURE(row.what);
        CHECK(consoleTravelKind(row.from, row.to, row.parent) == row.kind);
    }
}

TEST_CASE("a console journey keeps the request's place and opens without an arrival",
          "[native-travel]") {
    const Vec3 at{4.0f, 5.0f, 6.0f};
    constexpr f32 kYaw = 1.5f;
    for (const Row& row : rows()) {
        CAPTURE(row.what);
        for (const PlayScene::ConsoleTravelRequest& request :
             {PlayScene::ConsoleTravelRequest{row.to, at, kYaw},
              PlayScene::ConsoleTravelRequest{row.to, std::nullopt, std::nullopt}}) {
            CAPTURE(request.position.has_value());
            const ConsoleJourney journey = consoleJourney(request, row.from, row.parent);
            CHECK(journey.kind == row.kind);
            CHECK(journey.secret == (row.kind == ConsoleTravelKind::EnterChallenge ||
                                     row.kind == ConsoleTravelKind::ChallengeToChallenge));
            CHECK(journey.options.position == request.position);
            CHECK(journey.options.yaw == request.yaw);
            CHECK(journey.options.welcome == std::optional{false});
            CHECK_FALSE(journey.options.arriving);
            CHECK(journey.options.arrivalWorld == 0);
            CHECK(journey.options.items.empty());
        }
    }
}
