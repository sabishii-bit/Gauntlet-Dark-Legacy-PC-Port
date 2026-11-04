#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/rollback/LocalStateHistory.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/NativeDigestProfileSupport.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f64 kStep = 1.0 / 60.0;

PlayScene::Inputs journal(u32 tick, bool wrong = false) {
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    PlayScene::Inputs inputs;
    for (u8 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        PlayInput input;
        if (tick > 360) {
            const u32 elapsed = tick - 361;
            const u32 phase = (elapsed + seat * 30) % 240;
            const Vec2 direction = kDirections[(elapsed / 120 + seat + (wrong ? 2 : 0)) % 4];
            input.move = {direction, 1};
            input.aimDirection = Vec3{direction.x, 0, -direction.y};
            input.attack = wrong ? phase >= 90 : phase < 90;
            input.attackPressed = wrong || phase == 0;
            input.strongAttack = !wrong && phase >= 120 && phase < 150;
            input.turbo = !wrong && phase >= 180 && phase < 190;
            input.defendPressed = !wrong && phase == 180;
            input.usePotion = wrong ? elapsed % 2 == 0 : elapsed % 600 == 300;
            input.menu.select = elapsed % 120 == 0;
            input.menu.buttonPressed = input.menu.select;
        } else if (wrong) {
            input.menu.select = true;
            input.menu.buttonPressed = true;
        }
        inputs[seat] = SessionInputs::playInput(SessionInputs::command(input, 1, tick, 1, seat));
    }
    return inputs;
}

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    GameServices context;
    PlayScene scene;
    std::vector<PartyMember> initialParty;
    u32 tick = 0;

    explicit Fixture(std::string_view levelName, s32 seats = 2) {
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
        for (s32 seat = 0; seat < seats; ++seat) {
            CharacterSave save;
            save.name = "REWIND";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            save.progress().inventory.addKeys(9);
            save.progress().inventory.addPotions(seat + 1, 9);
            initialParty.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        open();
    }

    void open() {
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, initialParty, options,
                           {.stopTimeTotal = &stopTimeTotal}));
        REQUIRE(scene.nativeAgreementDigest());
        tick = 0;
    }

    void advanceTo(u32 target) {
        while (tick < target) {
            ++tick;
            REQUIRE(scene.update(kStep, journal(tick)) == PlayOutcome::Running);
        }
    }

    void render() {
        device.draws.clear();
        REQUIRE(device.beginFrame());
        scene.render(device, glm::ortho(0.0f, 640.0f, 448.0f, 0.0f), 640, 448, false, 0.5f);
        device.endFrame();
    }
};

struct PlayerView {
    Vec3 position;
    Vec3 facing;
    PlayerLife life;
    f32 healthFraction;
    s32 levelKills;
    s32 nameTicks;
    std::string character;
    bool operator==(const PlayerView&) const = default;
};

struct Observation {
    std::optional<NativeStateDigest::Digest> native;
    std::vector<PlayerView> players;
    WorldCamera camera;
    bool spawning{};
    bool leaving{};
    bool secret{};
    TransitionScreen::Phase transition{};
    f32 stopTimeTotal{};
    std::vector<PlayerSimulation> nativePlayers;
    const LevelOpponents* opponents = nullptr;
    LevelOpponents::LocalSnapshot nativeOpponents;
    const LevelFixtures* fixtures = nullptr;
    LevelFixtures::LocalSnapshot nativeFixtures;
    const LevelWorld* world = nullptr;
    LevelWorld::LocalSnapshot nativeWorld;
};

Observation observe(Fixture& fixture) {
    Observation result{fixture.scene.nativeAgreementDigest(),
                       {},
                       fixture.scene.viewCamera(),
                       fixture.scene.spawning(),
                       fixture.scene.leaving(),
                       fixture.scene.secretTravel(),
                       fixture.scene.transition().phase(),
                       fixture.stopTimeTotal};
    REQUIRE(result.native);
    result.opponents = &fixture.scene.opponents();
    REQUIRE(result.opponents->saveLocal(result.nativeOpponents));
    result.fixtures = &fixture.scene.fixtures();
    result.fixtures->saveLocal(result.nativeFixtures);
    result.world = fixture.scene.world();
    REQUIRE(result.world != nullptr);
    result.world->saveLocal(result.nativeWorld);
    for (const auto& player : fixture.scene.participants()) {
        result.nativePlayers.push_back(player.simulation());
        result.players.push_back({player.actor.position(), player.actor.facing(), player.life,
                                  player.healthFraction, player.levelKills, player.nameTicks,
                                  player.actor.save().toJson()});
    }
    return result;
}

void checkObservation(const Observation& actual, const Observation& expected) {
    REQUIRE(actual.opponents != nullptr);
    REQUIRE(actual.opponents == expected.opponents);
    CHECK(actual.opponents->equalLocal(actual.nativeOpponents, expected.nativeOpponents));
    REQUIRE(actual.fixtures != nullptr);
    REQUIRE(actual.fixtures == expected.fixtures);
    CHECK(actual.fixtures->equalLocal(actual.nativeFixtures, expected.nativeFixtures));
    REQUIRE(actual.world != nullptr);
    REQUIRE(actual.world == expected.world);
    CHECK(actual.world->equalLocal(actual.nativeWorld, expected.nativeWorld));
    CHECK(actual.nativePlayers == expected.nativePlayers);
    CHECK(actual.native == expected.native);
    CHECK(actual.players == expected.players);
    CHECK(actual.camera.position == expected.camera.position);
    CHECK(actual.camera.pitch == expected.camera.pitch);
    CHECK(actual.camera.yaw == expected.camera.yaw);
    CHECK(actual.camera.roll == expected.camera.roll);
    CHECK(actual.spawning == expected.spawning);
    CHECK(actual.leaving == expected.leaving);
    CHECK(actual.secret == expected.secret);
    CHECK(actual.transition == expected.transition);
    CHECK(actual.stopTimeTotal == expected.stopTimeTotal);
}

void restoreWithoutOutput(Fixture& fixture, const PlayScene::LocalSnapshot& saved) {
    const auto textures = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto draws = fixture.device.draws.size();
    const auto frames = fixture.device.frames;
    REQUIRE(fixture.scene.canRestoreLocal(saved));
    REQUIRE(fixture.scene.restoreLocal(saved));
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == updates);
    CHECK(fixture.device.draws.size() == draws);
    CHECK(fixture.device.frames == frames);
}

void checkDraws(std::span<const test::RecordedDraw> actual,
                std::span<const test::RecordedDraw> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i].texture == expected[i].texture);
        const bool sameVertices = actual[i].vertices == expected[i].vertices;
        if (!sameVertices) {
            const auto& got = actual[i];
            const auto& want = expected[i];
            const auto printVertex = [](std::string_view name, const ImmediateVertex& vertex) {
                std::cout << std::format(
                    "{} pos=({:.9g},{:.9g},{:.9g}) rgba=({},{},{},{}) uv=({:.9g},{:.9g}) "
                    "uv2=({:.9g},{:.9g})\n",
                    name, vertex.position.x, vertex.position.y, vertex.position.z, vertex.color.r,
                    vertex.color.g, vertex.color.b, vertex.color.a, vertex.uv.x, vertex.uv.y,
                    vertex.uv2.x, vertex.uv2.y);
            };
            std::cout << "draw mismatch " << i << '/' << actual.size()
                      << " vertices=" << got.vertices.size() << '/' << want.vertices.size()
                      << " texture=" << static_cast<const void*>(got.texture)
                      << " dimensions=" << got.texture->width() << 'x' << got.texture->height()
                      << " blend=" << static_cast<u32>(got.state.blend)
                      << " depth=" << got.state.depthTest << '/' << got.state.depthWrite
                      << " transform column3=" << got.transform[3].x << ',' << got.transform[3].y
                      << ',' << got.transform[3].z << ',' << got.transform[3].w << '\n';
            const usize shared = std::min(got.vertices.size(), want.vertices.size());
            usize different = 0;
            for (usize vertex = 0; vertex < shared; ++vertex) {
                if (got.vertices[vertex] == want.vertices[vertex]) {
                    continue;
                }
                if (different++ == 0) {
                    std::cout << "first differing vertex=" << vertex << '\n';
                    printVertex("actual", got.vertices[vertex]);
                    printVertex("expected", want.vertices[vertex]);
                }
            }
            std::cout << "different vertices=" << different << '\n';
        }
        CHECK(sameVertices);
        CHECK(actual[i].transform == expected[i].transform);
        CHECK(actual[i].state == expected[i].state);
    }
}
} // namespace

TEST_CASE("native console actions target their canonical seat and rewind cheats gifts and flight",
          "[native-console][play-scene-local-rewind][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 2);
    fixture->advanceTo(360);
    auto& scene = fixture->scene;
    const auto flying = [&](s32 seat) {
        return std::ranges::any_of(scene.participants(), [&](const PlayerRuntime& player) {
            return player.actor.player() == seat && player.ghost;
        });
    };
    REQUIRE(scene.beginRecordedAudio(1));
    PlayScene::LocalSnapshot before;
    REQUIRE(scene.saveLocal(before));
    const auto beforeDigest = scene.nativeAgreementDigest();
    REQUIRE(beforeDigest);
    const s32 hostGold = scene.actor(0)->save().gold;
    const s32 guestGold = scene.actor(1)->save().gold;
    const std::array actions{ConsoleAction{1, "god"}, ConsoleAction{1, "notarget"},
                             ConsoleAction{1, "noclip"}, ConsoleAction{1, "get gold 37"}};
    PlayScene::Inputs input;
    input[1].consoleFlight = {0, 1, 0};
    input[1].move = {{1, 0}, 1}; // Ignored while flight owns movement.
    const Vec3 position = scene.actor(1)->position();
    const auto step = scene.recordStep(1, 361, kStep, input, actions);
    REQUIRE(step.outcome == PlayOutcome::Running);
    REQUIRE(step.consoleReplies.size() == actions.size());
    CHECK(scene.cheats(1).god);
    CHECK(scene.cheats(1).noTarget);
    CHECK(scene.cheats(1).noclip);
    CHECK(flying(1));
    CHECK_FALSE(scene.cheats(0).god);
    CHECK(scene.actor(0)->save().gold == hostGold);
    CHECK(scene.actor(1)->save().gold == guestGold + 37);
    CHECK(scene.actor(1)->position().y > position.y);
    CHECK(scene.actor(1)->position().x == position.x);
    const auto afterDigest = scene.nativeAgreementDigest();
    REQUIRE(afterDigest);
    REQUIRE(scene.restoreLocal(before));
    CHECK(scene.nativeAgreementDigest() == beforeDigest);
    CHECK_FALSE(scene.cheats(1).god);
    CHECK_FALSE(scene.cheats(1).noTarget);
    CHECK_FALSE(scene.cheats(1).noclip);
    // Pads, pickups and portals see the restored body again, as they did before the toggle.
    CHECK_FALSE(flying(1));
    REQUIRE(scene.recordStep(1, 361, kStep, input, actions).outcome == PlayOutcome::Running);
    CHECK(flying(1));
    CHECK(scene.nativeAgreementDigest() == afterDigest);
}

TEST_CASE("local console camera changes neither native state nor the next native tick",
          "[native-console][native-console-camera][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 2);
    fixture->advanceTo(360);
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot before;
    REQUIRE(scene.saveLocal(before));
    const auto initial = scene.nativeAgreementDigest();
    REQUIRE(initial);
    REQUIRE(scene.recordStep(1, 361, kStep, {}).outcome == PlayOutcome::Running);
    const auto expected = scene.nativeAgreementDigest();
    REQUIRE(expected);
    REQUIRE(scene.restoreLocal(before));
    WorldCamera free = scene.simulationCamera();
    free.position += Vec3{900, 900, 900};
    free.yaw += 1.0f;
    scene.setViewOverride(free);
    CHECK(scene.nativeAgreementDigest() == initial);
    CHECK(scene.viewCamera().position == free.position);
    CHECK(scene.simulationCamera().position != free.position);
    REQUIRE(scene.recordStep(1, 361, kStep, {}).outcome == PlayOutcome::Running);
    CHECK(scene.nativeAgreementDigest() == expected);
    scene.setViewOverride(std::nullopt);
    CHECK(scene.nativeAgreementDigest() == expected);
}

TEST_CASE("console travel records reversible intent and clears on secret return",
          "[native-console][native-console-travel][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 2);
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot before;
    REQUIRE(scene.saveLocal(before));
    const std::array actions{ConsoleAction{1, "goto S1 10 20 30"}};
    REQUIRE(scene.recordStep(1, 1, kStep, {}, actions).outcome == PlayOutcome::Travel);
    REQUIRE(scene.consoleTravelRequest());
    CHECK(scene.consoleTravelRequest()->destination.name == "S1");
    CHECK(scene.consoleTravelRequest()->position == Vec3{10, 20, 30});
    CHECK(scene.secretTravel());
    CHECK(scene.isOpen());
    const auto planned = scene.nativeAgreementDigest();
    REQUIRE(planned);
    REQUIRE(scene.restoreLocal(before));
    CHECK_FALSE(scene.consoleTravelRequest());
    REQUIRE(scene.recordStep(1, 1, kStep, {}, actions).outcome == PlayOutcome::Travel);
    CHECK(scene.nativeAgreementDigest() == planned);
    scene.resumeFromChallenge(scene.party());
    CHECK_FALSE(scene.consoleTravelRequest());
    CHECK_FALSE(scene.secretTravel());
    CHECK(scene.recordStep(1, 2, kStep, {}).outcome == PlayOutcome::Running);
}

TEST_CASE("shared console rejects invalid values and absent seats without mutation",
          "[native-console][native-console-validation][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 2);
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot before;
    REQUIRE(scene.saveLocal(before));
    REQUIRE(scene.recordStep(1, 1, kStep, {}).outcome == PlayOutcome::Running);
    const auto expected = scene.nativeAgreementDigest();
    REQUIRE(expected);
    for (const auto& action :
         {ConsoleAction{0, "goto nan 0 0"}, ConsoleAction{0, "goto 1e38 0 0"},
          ConsoleAction{3, "god"}, ConsoleAction{0, "god unwanted"}, ConsoleAction{0, "get info"},
          ConsoleAction{0, "get exp 2147483647"}, ConsoleAction{0, "get gold 2147483647"},
          ConsoleAction{0, "get keys 2147483647"}, ConsoleAction{0, "get health 2147483647"},
          ConsoleAction{0, "get levitate 1e38"}, ConsoleAction{0, "get levitate nan"},
          ConsoleAction{0, "goto MISSING"}, ConsoleAction{0, "goto back"},
          ConsoleAction{0, "goto return"}, ConsoleAction{0, "goto n64 castle"},
          ConsoleAction{0, "goto castle preview"}, ConsoleAction{0, "goto lab"}}) {
        CAPTURE(action.line, action.seat);
        REQUIRE(scene.restoreLocal(before));
        const std::array actions{action};
        const auto step = scene.recordStep(1, 1, kStep, {}, actions);
        REQUIRE(step.consoleReplies.size() == 1);
        CHECK_FALSE(step.consoleReplies.front().text.empty());
        CHECK(step.outcome == PlayOutcome::Running);
        CHECK(scene.nativeAgreementDigest() == expected);
    }
}

TEST_CASE("native console spawn categories survive correction with retained resource caches",
          "[native-console][native-console-spawn][assets]") {
    test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2");
    auto fixture = std::make_unique<Fixture>("G2", 2);
    auto& scene = fixture->scene;
    REQUIRE_FALSE(fixture->world.layout().objects().empty());
    REQUIRE(scene.beginRecordedAudio(1));
    REQUIRE(fixture->world.placedItems().size() > 0);
    const std::vector<std::string> commands{
        "spawn enemy gru 2 2 G1",
        "spawn critter golem",
        "spawn critter golem B1",
        "spawn generator rat 2",
        "spawn generator special E1",
        "spawn item " + fixture->world.placedItems().item(0).name,
        "spawn object " + fixture->world.layout().objects().front().name};
    for (const auto& command : commands) {
        CAPTURE(command);
        PlayScene::LocalSnapshot before;
        REQUIRE(scene.saveLocal(before));
        const auto initial = scene.nativeAgreementDigest();
        REQUIRE(initial);
        const std::array actions{ConsoleAction{1, command}};
        const auto step = scene.recordStep(1, 1, kStep, {}, actions);
        REQUIRE(step.consoleReplies.size() == 1);
        CAPTURE(step.consoleReplies.front().text);
        CHECK_FALSE(step.consoleReplies.front().text.starts_with("No "));
        CHECK_FALSE(step.consoleReplies.front().text.starts_with("Spawned 0 "));
        const auto expected = scene.nativeAgreementDigest();
        REQUIRE(expected);
        REQUIRE(scene.restoreLocal(before));
        CHECK(scene.nativeAgreementDigest() == initial);
        REQUIRE(scene.recordStep(1, 1, kStep, {}, actions).outcome == PlayOutcome::Running);
        CHECK(scene.nativeAgreementDigest() == expected);
    }
}

TEST_CASE("console spawn budget is bounded and restored with its active objects",
          "[native-console][native-console-spawn-limit][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 2);
    auto& scene = fixture->scene;
    REQUIRE_FALSE(fixture->world.layout().objects().empty());
    const std::string command = "spawn object " + fixture->world.layout().objects().front().name;
    PlayScene::LocalSnapshot before;
    REQUIRE(scene.saveLocal(before));
    const std::vector<ConsoleAction> crowded(257, ConsoleAction{0, command});
    const auto step = scene.recordStep(1, 1, kStep, {}, crowded);
    REQUIRE(step.consoleReplies.size() == crowded.size());
    CHECK(scene.spawnedObjectCount() == 256);
    CHECK(step.consoleReplies.back().text == "The stage's console spawn limit is reached.");
    REQUIRE(scene.restoreLocal(before));
    CHECK(scene.spawnedObjectCount() == 0);
    const std::array one{ConsoleAction{0, command}};
    REQUIRE(scene.recordStep(1, 1, kStep, {}, one).outcome == PlayOutcome::Running);
    CHECK(scene.spawnedObjectCount() == 1);
}

TEST_CASE("whole native scenes rewind wrong one to eight tick branches and replay their journal",
          "[play-scene-local-rewind][assets]") {
    const auto level = GENERATE(std::string_view{"G2"}, std::string_view{"E1"},
                                std::string_view{"G5"}, std::string_view{"L1"});
    CAPTURE(level);
    auto fixture = std::make_unique<Fixture>(level);
    // One seed during the entrance and one after real movement/combat. The G2
    // journal also reaches native gas/explosion interactions near tick 650.
    for (const u32 checkpoint : {32U, 648U}) {
        fixture->advanceTo(checkpoint);
        const auto before = observe(*fixture);
        PlayScene::LocalSnapshot saved;
        REQUIRE(fixture->scene.saveLocal(saved));
        std::array<Observation, 8> expected;
        for (u32 frame = 0; frame < 8; ++frame) {
            REQUIRE(fixture->scene.update(kStep, journal(checkpoint + frame + 1)) ==
                    PlayOutcome::Running);
            expected[frame] = observe(*fixture);
        }
        restoreWithoutOutput(*fixture, saved);
        checkObservation(observe(*fixture), before);
        for (u32 distance = 1; distance <= 8; ++distance) {
            CAPTURE(checkpoint, distance);
            for (u32 frame = 0; frame < distance; ++frame) {
                REQUIRE(fixture->scene.update(kStep, journal(checkpoint + frame + 1, true)) ==
                        PlayOutcome::Running);
            }
            restoreWithoutOutput(*fixture, saved);
            checkObservation(observe(*fixture), before);
            for (u32 frame = 0; frame < distance; ++frame) {
                REQUIRE(fixture->scene.update(kStep, journal(checkpoint + frame + 1)) ==
                        PlayOutcome::Running);
                checkObservation(observe(*fixture), expected[frame]);
            }
            restoreWithoutOutput(*fixture, saved);
        }
    }
}

TEST_CASE("native scene restoration recovers rendered poses and texture animation without uploads",
          "[play-scene-local-rewind][play-scene-local-render][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    fixture->advanceTo(648);
    fixture->render(); // Warm lazy drawing resources before saving this loaded owner.
    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    fixture->render();
    const auto expected = fixture->device.draws;
    REQUIRE_FALSE(expected.empty());
    const auto before = observe(*fixture);
    for (u32 frame = 1; frame <= 8; ++frame) {
        REQUIRE(fixture->scene.update(kStep, journal(fixture->tick + frame, true)) ==
                PlayOutcome::Running);
    }
    fixture->render();
    restoreWithoutOutput(*fixture, saved);
    checkObservation(observe(*fixture), before);
    fixture->render();
    checkDraws(fixture->device.draws, expected);
}

TEST_CASE("native scene history crosses speculative defeat but rejects foreign and replaced stages",
          "[play-scene-local-rewind][play-scene-local-guards][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto foreign = std::make_unique<Fixture>("L1");
    fixture->advanceTo(32);
    PlayScene::LocalSnapshot empty;
    CHECK_FALSE(fixture->scene.canRestoreLocal(empty));
    CHECK_FALSE(fixture->scene.restoreLocal(empty));
    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    const auto before = observe(*fixture);
    CHECK_FALSE(foreign->scene.canRestoreLocal(saved));
    CHECK_FALSE(foreign->scene.restoreLocal(saved));
    fixture->scene.startGameOver();
    REQUIRE(fixture->scene.gameOver().active());
    for (u32 tick = 0; tick < 8; ++tick) {
        REQUIRE(fixture->scene.update(kStep, {}) == PlayOutcome::Running);
    }
    restoreWithoutOutput(*fixture, saved);
    CHECK_FALSE(fixture->scene.gameOver().active());
    checkObservation(observe(*fixture), before);
    // Save identities are intentionally not gameplay state.
    fixture->scene.setSaveSlot(0, 5);
    restoreWithoutOutput(*fixture, saved);
    REQUIRE(fixture->scene.runtime(0));
    CHECK(fixture->scene.runtime(0)->slot == 5);
    fixture->scene.close();
    CHECK_FALSE(fixture->scene.canRestoreLocal(saved));
    CHECK_FALSE(fixture->scene.restoreLocal(saved));
    fixture->open();
    CHECK_FALSE(fixture->scene.canRestoreLocal(saved));
    CHECK_FALSE(fixture->scene.restoreLocal(saved));
    const PlayScene closed;
    CHECK_FALSE(closed.saveLocal(empty));
}

TEST_CASE("the first running digest that differs names the owner a change touched",
          "[play-scene-local-rewind][native-digest-profile][assets]") {
    auto fixture = std::make_unique<Fixture>("G1", 1);
    auto& scene = fixture->scene;
    PlayScene::NativeDigestProfile before;
    REQUIRE(scene.nativeAgreementDigest(&before));
    PlayScene::NativeDigestProfile unchanged;
    REQUIRE(scene.nativeAgreementDigest(&unchanged));
    CHECK(test::firstDivergence(before, unchanged).empty());

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the fixture owns this live actor.
    auto& actor = const_cast<PlayerActor&>(*scene.actor(0));
    ++actor.save().progress().gold;
    PlayScene::NativeDigestProfile gold;
    REQUIRE(scene.nativeAgreementDigest(&gold));
    CHECK(test::firstDivergence(before, gold) == "players");

    fixture->world.setAmbientOffset(0.125f);
    PlayScene::NativeDigestProfile ambient;
    REQUIRE(scene.nativeAgreementDigest(&ambient));
    CHECK(test::firstDivergence(gold, ambient) == "world");
}

TEST_CASE("native scenes measure a warmed sixteen slot ring with two and four participants",
          "[.][play-scene-local-benchmark][assets]") {
    const auto level = GENERATE(std::string_view{"G2"}, std::string_view{"F2"});
    const auto seats = GENERATE(2, 4);
    CAPTURE(level, seats);
    auto fixture = std::make_unique<Fixture>(level, seats);
    fixture->advanceTo(632);
    LocalStateHistory<PlayScene::LocalSnapshot> history;
    static_assert(decltype(history)::kCapacity == 16);
    for (u32 frame = 0; frame < 16; ++frame) {
        const u32 next = fixture->tick + 1;
        REQUIRE(
            history.capture(next, [&](auto& saved) { return fixture->scene.saveLocal(saved); }));
        fixture->advanceTo(next);
    }
    // Every slot is warm, but each measures a distinct evolving native state.
    // These measurements are not assertions against shared-machine timing and
    // do not imply a shipping rollback budget on every player's hardware.
    constexpr usize kSamples = 128;
    std::array<f64, kSamples> saves{};
    std::array<f64, kSamples> restores{};
    std::array<f64, kSamples> updates{};
    std::array<f64, kSamples> replay8{};
    using Clock = std::chrono::steady_clock;
    const auto micros = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<f64, std::micro>(to - from).count();
    };
    const u32 firstTick = fixture->tick;
    const usize initialEnemies = fixture->scene.enemies().targets().size();
    usize mostEnemies = initialEnemies;
    bool bossAwake = false;
    bool bossAlive = false;
    for (usize sample = 0; sample < kSamples; ++sample) {
        const u32 next = fixture->tick + 1;
        const auto baseline = observe(*fixture);
        auto start = Clock::now();
        const bool captured =
            history.capture(next, [&](auto& saved) { return fixture->scene.saveLocal(saved); });
        auto end = Clock::now();
        REQUIRE(captured);
        saves[sample] = micros(start, end);
        const auto* saved = history.find(next);
        REQUIRE(saved);
        start = Clock::now();
        const auto outcome = fixture->scene.update(kStep, journal(next));
        end = Clock::now();
        REQUIRE(outcome == PlayOutcome::Running);
        updates[sample] = micros(start, end);
        const auto expected = observe(*fixture);
        start = Clock::now();
        const bool restored = fixture->scene.restoreLocal(*saved);
        end = Clock::now();
        REQUIRE(restored);
        restores[sample] = micros(start, end);
        checkObservation(observe(*fixture), baseline);
        start = Clock::now();
        bool running = true;
        for (u32 frame = 1; frame <= 8; ++frame) {
            running &= fixture->scene.update(kStep, journal(fixture->tick + frame)) ==
                       PlayOutcome::Running;
        }
        end = Clock::now();
        REQUIRE(running);
        replay8[sample] = micros(start, end);
        REQUIRE(fixture->scene.restoreLocal(*saved));
        fixture->advanceTo(next);
        checkObservation(observe(*fixture), expected);
        mostEnemies = std::max(mostEnemies, fixture->scene.enemies().targets().size());
        if (const auto boss = fixture->scene.bossView()) {
            bossAwake |= boss->awake;
            bossAlive |= boss->alive;
        }
    }
    const auto report = [](std::string_view name, auto times) {
        std::ranges::sort(times);
        std::cout << name << " us p50=" << times[kSamples / 2]
                  << " p95=" << times[kSamples * 95 / 100] << " max=" << times.back() << '\n';
    };
    std::cout << level << " native whole-scene, ticks=" << firstTick << '-' << fixture->tick
              << ", players=" << fixture->scene.actorCount()
              << ", enemies initial/max=" << initialEnemies << '/' << mostEnemies
              << ", generators=" << fixture->scene.generators().count()
              << ", boss awake/alive=" << bossAwake << '/' << bossAlive
              << ", ring slots=" << decltype(history)::kCapacity << ", samples=" << kSamples
              << '\n';
    report("ring-save", saves);
    report("ring-restore", restores);
    report("update", updates);
    report("replay8", replay8);
}

TEST_CASE("native snapshot profiling separates warmed owners in active Ghost Town",
          "[.][native-copy-profile][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 4);
    fixture->advanceTo(632);
    constexpr usize kSlots = 16;
    constexpr usize kSamples = 128;
    struct History {
        std::array<PlayScene::LocalSnapshot, kSlots> scene;
        std::array<LevelWorld::LocalSnapshot, kSlots> world;
        std::array<ParticleField, kSlots> particles;
        std::array<WorldScene::LocalSnapshot, kSlots> geometry;
        std::array<WorldCollision::LocalSnapshot, kSlots> collision;
        std::array<PlacedItems::LocalSnapshot, kSlots> pickups;
        std::array<LevelOpponents::LocalSnapshot, kSlots> opponents;
        std::array<Chests::LocalSnapshot, kSlots> chests;
        std::array<Breakables::LocalSnapshot, kSlots> barrels;
        std::array<Traps::LocalSnapshot, kSlots> traps;
        std::array<EffectTrees::LocalSnapshot, kSlots> effects;
    };
    auto history = std::make_unique<History>();
    constexpr std::array<std::string_view, 11> kNames{
        "whole-scene",     "world",        "world-particles", "world-geometry",
        "world-collision", "placed-items", "opponents",       "chests",
        "barrels",         "traps",        "effects"};
    std::array<std::array<f64, kSamples>, kNames.size()> timings{};
    using Clock = std::chrono::steady_clock;
    usize maxParticles = 0;
    for (usize sample = 0; sample < kSlots + kSamples; ++sample) {
        const usize slot = sample % kSlots;
        const auto capture = [&](usize component) {
            switch (component) {
            case 0: return fixture->scene.saveLocal(history->scene[slot]);
            case 1: fixture->world.saveLocal(history->world[slot]); break;
            case 2: history->particles[slot] = fixture->world.particles(); break;
            case 3: fixture->world.scene().saveLocal(history->geometry[slot]); break;
            case 4: fixture->world.collision().saveLocal(history->collision[slot]); break;
            case 5: fixture->world.placedItems().saveLocal(history->pickups[slot]); break;
            case 6: return fixture->scene.opponents().saveLocal(history->opponents[slot]);
            case 7: fixture->scene.chests().saveLocal(history->chests[slot]); break;
            case 8: fixture->scene.barrels().saveLocal(history->barrels[slot]); break;
            case 9: fixture->scene.traps().saveLocal(history->traps[slot]); break;
            case 10: fixture->scene.effects().saveLocal(history->effects[slot]); break;
            default: return false;
            }
            return true;
        };
        // Rotate the first owner rather than always giving the whole scene cold
        // caches and its children already-warm caches. Nested owner timings
        // overlap: they locate copy cost, and must not be summed as a tick cost.
        for (usize offset = 0; offset < kNames.size(); ++offset) {
            const usize component = (sample + offset) % kNames.size();
            const auto start = Clock::now();
            const bool captured = capture(component);
            const auto end = Clock::now();
            REQUIRE(captured);
            if (sample >= kSlots) {
                timings[component][sample - kSlots] =
                    std::chrono::duration<f64, std::micro>(end - start).count();
            }
        }
        maxParticles = std::max(maxParticles, fixture->world.particles().particleCount());
        fixture->advanceTo(fixture->tick + 1);
    }
    REQUIRE(fixture->scene.enemies().targets().size() >= 10);
    REQUIRE(maxParticles > 0);
    std::cout << "native snapshot owners level=G2 seats=4 samples=" << kSamples
              << " emitters=" << fixture->world.particles().size()
              << " max-particles=" << maxParticles << " nested-timings=overlap\n";
    for (usize component = 0; component < kNames.size(); ++component) {
        auto& times = timings[component];
        std::ranges::sort(times);
        std::cout << kNames[component] << " copy-us p50=" << times[kSamples / 2]
                  << " p95=" << times[kSamples * 95 / 100] << " max=" << times.back() << '\n';
    }
}
