#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f64 kStep = 1.0 / 60.0;
constexpr s32 kSeats = 2;

/** A level open for two seats (the tower for "L1"), stepped a tick at a time as every peer
 * steps it, with one seat's console line in a tick of its own. */
struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    PlayScene scene;
    GameContext context;
    u64 tick = 0;

    explicit Fixture(std::string_view levelName) {
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", levelName));
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level =
            levelName == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(levelName);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        context.stopTimeTotal = &stopTimeTotal;
        std::vector<PartyMember> party;
        for (s32 seat = 0; seat < kSeats; ++seat) {
            CharacterSave save;
            save.name = "TESTER";
            save.character = seat;
            save.color = seat;
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }

    /** One tick carrying `line` for `seat`; what the scene answers it. */
    PlayScene::RecordedStep say(std::string line, u8 seat) {
        const std::array actions{ConsoleAction{seat, std::move(line)}};
        auto step = scene.recordStep(1, ++tick, kStep, {}, actions);
        REQUIRE(step.consoleReplies.size() == 1);
        return step;
    }
};

} // namespace

TEST_CASE("console travel names the place, whether it is a challenge, and the way back",
          "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));

    // Into a challenge, the way back is where the one who asked stood.
    const auto s1 = fixture->catalog.byName("S1");
    REQUIRE(s1);
    const Vec3 asked = scene.actor(1)->position();
    const Vec3 beside = scene.actor(0)->position();
    REQUIRE((asked.x != beside.x || asked.z != beside.z));
    const auto challenge = fixture->say("goto S1", 1);
    CHECK(challenge.outcome == PlayOutcome::Travel);
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{*s1, std::nullopt, std::nullopt});
    CHECK(scene.destination() == *s1);
    CHECK(scene.secretTravel());
    scene.resumeFromChallenge(scene.party());
    CHECK(scene.actor(0)->position().x == Catch::Approx(asked.x));
    CHECK(scene.actor(0)->position().z == Catch::Approx(asked.z));

    // A level and a point there: no challenge.
    REQUIRE(scene.restoreLocal(opened));
    const auto g1 = fixture->catalog.byName("G1");
    REQUIRE(g1);
    const auto level = fixture->say("goto G1 4 5 6", 1);
    CHECK(level.outcome == PlayOutcome::Travel);
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{*g1, Vec3{4, 5, 6}, std::nullopt});
    CHECK(scene.destination() == *g1);
    CHECK_FALSE(scene.secretTravel());

    // unlock_tower in the tower reopens it where the one who asked stands, facing their way.
    fixture.reset();
    auto tower = std::make_unique<Fixture>("L1");
    const PlayerActor* owner = tower->scene.actor(1);
    REQUIRE(owner != nullptr);
    const Vec3 at = owner->position();
    const f32 yaw = owner->yaw();
    const auto reopened = tower->say("unlock_tower", 1);
    CHECK(reopened.outcome == PlayOutcome::Travel);
    CHECK(tower->scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{LevelRef::tower(), at, yaw});
    CHECK(tower->scene.destination() == LevelRef::tower());
    CHECK_FALSE(tower->scene.secretTravel());
}
