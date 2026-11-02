#include <array>
#include <concepts>
#include <memory>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f64 kStep = 1.0 / 60;

template <class T> auto nativeHash(const T& value) {
    NativeStateDigest digest;
    digest.value(value);
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

// Defaulted whole-value comparisons intentionally make a new noncomparable
// member a compile error rather than silently excluding it from a manual list.
static_assert(std::equality_comparable<BossCamera>);
static_assert(std::equality_comparable<GameOver>);
static_assert(std::equality_comparable<SecretChallenge>);
static_assert(std::equality_comparable<RuneMeter>);
static_assert(std::equality_comparable<SwitchCutscene>);
static_assert(std::equality_comparable<PlayerHealth>);
static_assert(std::equality_comparable<PartyPickups>);
static_assert(std::equality_comparable<PickupVoices>);
static_assert(std::equality_comparable<AmbientDimmer>);
static_assert(std::equality_comparable<LevelWatch>);
static_assert(std::equality_comparable<LevelRef>);
static_assert(std::equality_comparable<TowerAccess>);
static_assert(std::equality_comparable<CameraShake>);
static_assert(std::equality_comparable<TowerWelcome>);

TEST_CASE("root control equality distinguishes secret coin identities from equal counts",
          "[play-scene-control-equality]") {
    SecretChallenge first;
    SecretChallenge second;
    SecretChallenge reordered;
    REQUIRE(first.begin(3, 120, std::array<usize, 3>{4, 7, 9}));
    REQUIRE(second.begin(3, 120, std::array<usize, 3>{4, 7, 10}));
    REQUIRE(reordered.begin(3, 120, std::array<usize, 3>{9, 4, 7}));
    CHECK(first.state() == second.state());
    CHECK(first.remaining() == second.remaining());
    CHECK(first.duration() == second.duration());
    CHECK(first.coinsLeft() == second.coinsLeft());
    CHECK(first.totalCoins() == second.totalCoins());
    CHECK(first.rewardMask() == second.rewardMask());
    CHECK_FALSE(first == second);
    CHECK(nativeHash(first) != nativeHash(second));
    CHECK(first == reordered); // hash-container insertion order is not game state
    CHECK(nativeHash(first) == nativeHash(reordered));
    CHECK_FALSE(first.collect(9));
    CHECK_FALSE(second.collect(9));
    CHECK(first.coinsLeft() + 1 == second.coinsLeft());
}

TEST_CASE("root camera equality includes hidden marker and acceleration history",
          "[play-scene-control-equality]") {
    BossCameraInfo record;
    record.flags = 3;
    record.maxYaw = std::numbers::pi_v<f32>;
    record.minDistance = record.minPlayerDistance = 25;
    record.maxDistance = 75;
    record.maxPlayerDistance = 30;
    record.minPitch = 0.3f;
    record.maxPitch = 0.45f;
    const BossCameraSubject boss;
    const std::array party{CameraSubject{Vec3{0, 0, 30}, Vec3{0, 3, 30}}};
    std::array<WorldLocator, 2> markers;
    markers[0].position = Vec3{0, 0, 30};
    markers[0].rotation = Vec3{0.4f, 0.5f, 0};
    markers[1] = markers[0];
    markers[1].position.x = 100;
    BossCamera first;
    first.reset(boss, party, record, {}, markers);
    const auto reversed = std::array{markers[1], markers[0]};
    BossCamera second;
    second.reset(boss, party, record, {}, reversed);
    REQUIRE(first.camera() == second.camera());
    CHECK(first.attention() == second.attention());
    CHECK(first.distance() == second.distance());
    CHECK(first.margin() == second.margin());
    CHECK_FALSE(first == second); // same view, different retained marker index
    CHECK(nativeHash(first) != nativeHash(second));

    second = first;
    auto changed = markers;
    changed[0].rotation.y += 0.4f;
    // A sub-ULP view change still changes its retained turn velocity. Both
    // branches receive the same elapsed time and recompute the same margin.
    first.update(boss, party, record, {}, 1.0e-8f, markers);
    second.update(boss, party, record, {}, 1.0e-8f, changed);
    REQUIRE(first.camera() == second.camera());
    CHECK(first.attention() == second.attention());
    CHECK(first.distance() == second.distance());
    CHECK(first.margin() == second.margin());
    CHECK_FALSE(first == second);
    const BossCamera copied = second;
    CHECK(copied == second);
    CHECK(nativeHash(first) != nativeHash(second));
    CHECK(nativeHash(copied) == nativeHash(second));
}

TEST_CASE("root control equality includes fractional clocks priorities and voice randomness",
          "[play-scene-control-equality]") {
    SECTION("fractional lighting clock before any visible darkening") {
        AmbientDimmer first;
        AmbientDimmer second;
        first.update(AmbientDimmer::kFrameSeconds / 4);
        CHECK(first.offset() == second.offset());
        CHECK(first.active() == second.active());
        CHECK_FALSE(first == second);
        CHECK(nativeHash(first) != nativeHash(second));
        second = first;
        CHECK(first == second);
        CHECK(nativeHash(first) == nativeHash(second));
    }
    SECTION("identical defeat captions before their hidden voice trigger") {
        GameOver first;
        GameOver second;
        first.begin("GAME OVER");
        second.begin("GAME OVER");
        first.step(1);
        second.step(2);
        CHECK(first.active() == second.active());
        CHECK(first.finished() == second.finished());
        CHECK(first.letters() == second.letters());
        CHECK_FALSE(first == second);
        CHECK(nativeHash(first) != nativeHash(second));
    }
    SECTION("same current shake but a different future arbitration priority") {
        CameraShake first;
        CameraShake second;
        first.start(CameraShake::Target::Both, 0, 90, 0.1f, 1);
        second.start(CameraShake::Target::Both, 0, 90, 0.1f, 2);
        CHECK(first.offset() == second.offset());
        CHECK(first.active() == second.active());
        CHECK_FALSE(first == second);
        CHECK(nativeHash(first) != nativeHash(second));
        first.start(CameraShake::Target::Eye, 0, 90, 0.3f, 1);
        second.start(CameraShake::Target::Eye, 0, 90, 0.3f, 1);
        CHECK(first.offset() != second.offset());
    }
    SECTION("food response stream advances even when only native state is retained") {
        PickupVoices first;
        PickupVoices second;
        REQUIRE(first == second);
        first.food(0, "APPLE", false, false);
        CHECK_FALSE(first == second);
        CHECK(nativeHash(first) != nativeHash(second));
        second.food(0, "APPLE", false, false);
        CHECK(first == second);
        CHECK(nativeHash(first) == nativeHash(second));
    }
    SECTION("pain RNG includes its exact cursor and all native words") {
        PlayerHealth first;
        PlayerHealth second;
        Mt19937 random{0x5A17u};
        REQUIRE(first == second);
        random();
        REQUIRE(second.restore(random.checkpoint()));
        CHECK_FALSE(first == second);
        CHECK(nativeHash(first) != nativeHash(second));
        first = second;
        CHECK(first == second);
        CHECK(nativeHash(first) == nativeHash(second));
    }
    SECTION("runestone cue latches differ even when the displayed fill matches") {
        std::array<PartyMember, 1> party;
        party[0].save.progress().relics.addShard(9);
        RuneMeter first;
        RuneMeter second;
        first.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, party);
        second = first;
        REQUIRE(first.update(Vec3{0}, true) == RuneMeter::Cue::Closer);
        REQUIRE(first.update(Vec3{100, 0, 0}, true) == RuneMeter::Cue::None);
        CHECK(first.fill() == second.fill());
        CHECK(first.visible() == second.visible());
        CHECK(nativeHash(first) != nativeHash(second));
    }
    SECTION("level notices retain the player associated with each mark") {
        LevelWatch first;
        LevelWatch second;
        first.observe(0, 10);
        second.observe(1, 10);
        CHECK(nativeHash(first) != nativeHash(second));
        second = first;
        CHECK(nativeHash(first) == nativeHash(second));
    }
    SECTION("unopened tower gates still retain partial crystal progress") {
        std::array<CharacterSave, 1> party;
        const TowerAccess first{std::span<const CharacterSave>{party}};
        party[0].progress().crystals[0] = 1;
        const TowerAccess second{std::span<const CharacterSave>{party}};
        CHECK(nativeHash(first) != nativeHash(second));
    }
}

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog levels;
    LevelWorld world;
    GameContext context;
    std::array<PartyMember, 1> party;
    PlayScene scene;

    Fixture() {
        REQUIRE(levels.load(root));
        const auto level = levels.byName("G1");
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        config.timing.tickRate = 60;
        context.config = &config;
        context.levels = &levels;
        context.unpackedRoot = root;
        CharacterSave save;
        save.name = "CONTROL";
        save.progress().health = 5000;
        save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
        party[0] = {0, save};
        open();
        for (u64 tick = 1; tick <= 480 && entering(); ++tick) {
            REQUIRE(scene.recordStep(1, tick, kStep, {}).outcome == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
    }
    bool entering() const {
        return scene.spawning() || (scene.animator(0) != nullptr && scene.animator(0)->entering());
    }
    void open() {
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{18.8f, 0.2f, 0.7f};
        REQUIRE(scene.open(device, context, world, party, options));
    }
};

TEST_CASE("native root control equality survives one to eight corrected scene steps",
          "[play-scene-control-equality][assets]") {
    auto fixture = std::make_unique<Fixture>();
    PlayScene::LocalSnapshot before;
    REQUIRE(fixture->scene.saveLocal(before));
    REQUIRE(fixture->scene.matchesLocalControl(before));
    std::array<PlayScene::LocalSnapshot, 8> expected;
    PlayScene::Inputs input;
    input[0].move = {Vec2{1, 0}, 0.5f};
    input[0].attack = true;
    for (usize tick = 0; tick < expected.size(); ++tick) {
        input[0].attackPressed = tick == 0;
        REQUIRE(fixture->scene.recordStep(1, tick + 1, kStep, input).outcome ==
                PlayOutcome::Running);
        REQUIRE(fixture->scene.saveLocal(expected[tick]));
    }
    for (usize distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(fixture->scene.restoreLocal(before));
        REQUIRE(fixture->scene.matchesLocalControl(before));
        fixture->scene.startGameOver();
        REQUIRE_FALSE(fixture->scene.matchesLocalControl(before));
        for (usize tick = 0; tick < distance; ++tick) {
            REQUIRE(fixture->scene.recordStep(1, tick + 1, kStep, {}).outcome ==
                    PlayOutcome::Running);
        }
        REQUIRE_FALSE(fixture->scene.matchesLocalControl(expected[distance - 1]));
        REQUIRE(fixture->scene.restoreLocal(before));
        REQUIRE(fixture->scene.matchesLocalControl(before));
        for (usize tick = 0; tick < distance; ++tick) {
            input[0].attackPressed = tick == 0;
            REQUIRE(fixture->scene.recordStep(1, tick + 1, kStep, input).outcome ==
                    PlayOutcome::Running);
            CHECK(fixture->scene.matchesLocalControl(expected[tick]));
        }
    }
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    const auto frames = fixture->device.frames;
    CHECK(fixture->scene.matchesLocalControl(expected.back()));
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
    CHECK(fixture->device.frames == frames);
}

TEST_CASE("native root control comparisons reject empty foreign and reopened owners",
          "[play-scene-control-equality][assets]") {
    auto fixture = std::make_unique<Fixture>();
    auto foreign = std::make_unique<Fixture>();
    PlayScene::LocalSnapshot saved;
    PlayScene::LocalSnapshot empty;
    CHECK_FALSE(fixture->scene.matchesLocalControl(empty));
    REQUIRE(fixture->scene.saveLocal(saved));
    CHECK(fixture->scene.matchesLocalControl(saved));
    CHECK_FALSE(foreign->scene.matchesLocalControl(saved));
    // This gate deliberately excludes participant and rendering state. A save
    // slot change is not root simulation control, and must not suggest otherwise.
    fixture->scene.setSaveSlot(0, 9);
    CHECK(fixture->scene.matchesLocalControl(saved));
    fixture->scene.close();
    CHECK_FALSE(fixture->scene.matchesLocalControl(saved));
    fixture->open();
    CHECK_FALSE(fixture->scene.matchesLocalControl(saved));
    REQUIRE(fixture->scene.saveLocal(empty));
    CHECK(fixture->scene.matchesLocalControl(empty));
}
} // namespace
