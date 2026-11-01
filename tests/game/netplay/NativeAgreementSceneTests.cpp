#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Log.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/enemies/OpponentResources.h"
#include "game/netplay/CharacterProfile.h"
#include "game/rollback/PlacedEnemyCheckpoint.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene scene;

    explicit Fixture(std::string_view name = "G1", s32 players = 1,
                     const std::optional<CharacterSave>& initial = std::nullopt) {
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        GameContext context;
        context.config = &config;
        context.levels = &catalog;
        context.tower = world.isTower() ? &world : nullptr;
        context.unpackedRoot = root;
        CharacterSave save = initial.value_or(CharacterSave{});
        save.name = "WATCH";
        save.progress().health = 2000;
        REQUIRE(CharacterProfile::capture(save));
        std::vector<PartyMember> party;
        party.reserve(static_cast<usize>(players));
        for (s32 player = 0; player < players; ++player) {
            party.push_back({player, save});
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }

    SimulationObservation::Digest digest() {
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        const auto draws = device.draws.size();
        const auto result = scene.nativeAgreementDigest();
        REQUIRE(result);
        CHECK(scene.nativeAgreementDigest() == result);
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.draws.size() == draws);
        return *result;
    }
};
} // namespace

TEST_CASE("native watchdog observes boss generators without a restore-domain certificate",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    const auto before = fixture->digest();
    auto& generators = fixture->scene.generators();
    const auto index = static_cast<s32>(generators.count());
    ItemArchive noArt;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 100;
    info.height = 5;
    info.radius = 2;
    // This is the native boss-effect birth API. Missing artwork legitimately
    // leaves the gameplay generator present; no checkpoint state is injected.
    REQUIRE(generators.placeBoss(fixture->device, info, noArt, fixture->scene.enemies(), kGruntKind,
                                 Mat4{1}, nullptr));
    REQUIRE_FALSE(generators.checkpoint());
    const auto born = fixture->digest();
    CHECK(born != before);
    REQUIRE(generators.strike(index, 1, 0));
    CHECK(fixture->digest() != born);
    REQUIRE_FALSE(generators.checkpoint()); // Restore admission is unchanged.
}

TEST_CASE("native watchdog covers stored player cooldowns and inactive saved classes",
          "[native-agreement-scene][native-state-digest][assets]") {
    auto fixture = std::make_unique<Fixture>();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& player = const_cast<PlayerRuntime&>(fixture->scene.participants().front());
    const PlayerSimulation saved = player.simulation();
    const auto before = fixture->digest();
    player.surfaceGap = 0.25f;
    CHECK(fixture->digest() != before);
    player.simulation() = saved;
    CHECK(fixture->digest() == before);
    player.entrySave.progress().health += 1;
    CHECK(fixture->digest() != before);
    player.simulation() = saved;
    CHECK(fixture->digest() == before);
}

TEST_CASE("native watchdog rejects unknown missile resources instead of omitting them",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& missiles = const_cast<EnemyMissiles&>(fixture->scene.enemyMissiles());
    const TreeModel firstArt;
    const auto before = fixture->digest();
    missiles.launch(EnemyMissileKind::arrow(), {0, 10, 0}, {0, 10, 30}, 1, &firstArt, 0);
    REQUIRE(missiles.count() == 1);
    REQUIRE_FALSE(missiles.checkpoint(OpponentResources::missiles(fixture->scene.enemies())));
    CHECK_FALSE(fixture->scene.nativeAgreementDigest());
    // A legitimate art-free native flight is still hashable. Unregistered
    // non-null resources cannot be silently collapsed into that state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<EnemyMissile&>(missiles.missile(0)).model = nullptr;
    const auto launched = fixture->digest();
    CHECK(launched != before);
    missiles.update(1.0f / 60, nullptr, {});
    REQUIRE(missiles.count() == 1);
    CHECK(fixture->digest() != launched);
    missiles.burst({0, 0, 0}, 5, 10, 0, 1, -1);
    const auto withBlast = fixture->digest();
    REQUIRE(missiles.burstCount() == 1);
    missiles.clear(); // Clear borrowers before the local artwork owners die.
    CHECK(fixture->digest() != withBlast);
}

TEST_CASE("native watchdog compares full blast parameters with unchanged flight and burst counts",
          "[native-agreement-scene][native-state-digest][assets]") {
    auto fixture = std::make_unique<Fixture>();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& missiles = const_cast<EnemyMissiles&>(fixture->scene.enemyMissiles());
    EnemyMissiles::LocalSnapshot empty;
    missiles.saveLocal(empty);
    missiles.burst({0, 0, 0}, 5, 10, 0, 1, -1);
    const auto first = fixture->digest();
    REQUIRE(missiles.restoreLocal(empty));
    missiles.burst({0, 0, 0}, 8, 10, 0, 1, -1);
    REQUIRE(missiles.burstCount() == 1);
    REQUIRE(missiles.count() == 0);
    CHECK(fixture->digest() != first);
}

TEST_CASE("native watchdog includes trigger inputs waiting for their next native update",
          "[native-agreement-scene][native-state-digest][assets]") {
    auto fixture = std::make_unique<Fixture>();
    const auto before = fixture->digest();
    REQUIRE(fixture->world.triggers().size() > 0);
    // Change only the queued contact bit, not any public geometry/camera result.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& trigger = const_cast<LevelTrigger&>(fixture->world.triggers().trigger(0));
    trigger.shot = !trigger.shot;
    CHECK(fixture->digest() != before);
}

TEST_CASE("native coordinator digest matches its saved values and rewinds pending rewards",
          "[native-agreement-scene][native-state-digest][local-rewind][assets]") {
    auto fixture = std::make_unique<Fixture>();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& opponents = const_cast<LevelOpponents&>(fixture->scene.opponents());
    LevelOpponents::LocalSnapshot saved;
    REQUIRE(opponents.saveLocal(saved));
    NativeStateDigest initial;
    REQUIRE(opponents.appendNativeFollowupDigest(initial));
    NativeStateDigest history;
    REQUIRE(opponents.appendLocalFollowupDigest(history, saved));
    CHECK(initial.checkedFinish() == history.checkedFinish());
    REQUIRE(opponents.generators().count() > 0);
    opponents.strikeGenerator(0, 1, 0, fixture->scene.participants());
    NativeStateDigest changed;
    REQUIRE(opponents.appendNativeFollowupDigest(changed));
    CHECK(initial.checkedFinish() != changed.checkedFinish());
    REQUIRE(opponents.restoreLocal(saved));
    NativeStateDigest restored;
    REQUIRE(opponents.appendNativeFollowupDigest(restored));
    CHECK(initial.checkedFinish() == restored.checkedFinish());
    auto other = std::make_unique<Fixture>();
    NativeStateDigest foreign;
    CHECK_FALSE(other->scene.opponents().appendLocalFollowupDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
}

TEST_CASE("native watchdog walks the entire pending placement roster beyond wire limits",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& placements = const_cast<PlacedEnemies&>(fixture->scene.placedEnemies());
    PlacedEnemies::Placement placement;
    placement.kind = kGruntKind;
    placement.spawn.kind = kGruntKind;
    placement.spawn.placed = true;
    std::vector<PlacedEnemies::Placement> roster(PlacedEnemyCheckpoint::kMaxPlacements + 1,
                                                 placement);
    placements.bind(roster);
    const auto local = PlacedEnemiesCheckpoint::capture(placements);
    REQUIRE(local);
    REQUIRE_FALSE(PlacedEnemyCheckpoint::canonicalDigest(*local));
    const auto complete = fixture->digest();
    roster.back().spawn.position.x = 0.25f;
    placements.bind(roster);
    const auto changedLast = fixture->digest();
    CHECK(changedLast != complete); // No truncation to the wire's 512 records.
    usize dispatched = 0;
    placements.activate([&](const PlacedEnemies::Placement&) { ++dispatched; });
    REQUIRE(dispatched == roster.size());
    REQUIRE(placements.pending().empty());
    CHECK(fixture->digest() != changedLast);
}

TEST_CASE("native watchdog includes a real boss encounter without claiming full boss state",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>("G5");
    REQUIRE(fixture->scene.bosses().present());
    REQUIRE_FALSE(fixture->scene.bosses().view().awake);
    const auto sleeping = fixture->digest();
    fixture->scene.bosses().wake();
    REQUIRE(fixture->scene.bosses().view().awake);
    CHECK(fixture->digest() != sleeping);
    const auto& fighter = fixture->scene.bosses().fighter();
    const auto position = fighter.position();
    CHECK(fixture->scene.bosses().position() != nullptr);
    CHECK(*fixture->scene.bosses().position() == position);
}

TEST_CASE("first native watchdog after promotion registers the original retained figure",
          "[native-agreement-scene][native-promotion-resources][local-rewind][assets]") {
    test::assetOrSkip("PLAYERS/JES/SFXYEL/ANIM.PS2");
    CharacterSave save;
    save.character = 7;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    auto fixture = std::make_unique<Fixture>("L1", 1, save);
    auto& scene = fixture->scene;
    REQUIRE(scene.promotion().active());
    auto* original = scene.participants().front().figure.get();
    REQUIRE(original);
    auto* originalEffects = original->effects();
    REQUIRE(originalEffects);

    // Keep a genuine class effect alive across the native costume replacement.
    // The first figure is retained by ownership, but has no promotion-cache entry.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& effects = const_cast<EffectTrees&>(scene.effects());
    EffectTrees::Setting setting;
    setting.persistent = true;
    const auto effect = effects.startSet(fixture->device, *originalEffects, "JES_PWRA2",
                                         scene.actor(0)->position(), setting);
    REQUIRE(effect != 0);

    PlayScene::LocalSnapshot beforePromotion;
    for (usize tick = 0; tick < 1500 && scene.runtime(0)->figure.get() == original; ++tick) {
        // Keep the exact pre-award tick, without constructing a digest/registry.
        REQUIRE(scene.saveLocal(beforePromotion));
        REQUIRE(scene.update(1.0 / 60.0, {}) == PlayOutcome::Running);
    }
    const auto* promoted = scene.runtime(0)->figure.get();
    REQUIRE(promoted);
    REQUIRE(promoted != original);
    REQUIRE(scene.actor(0)->save().progress().appearanceLevel() == 30);
    const auto retainedEffect = [&] {
        for (usize index = 0; index < effects.count(); ++index) {
            const auto& entry = effects.effect(index);
            if (entry.id == effect) {
                return entry.archive == originalEffects;
            }
        }
        return false;
    };
    REQUIRE(retainedEffect());

    // Deliberately the first hash: prior registration must not hide the missing
    // old owner. The helper also verifies no uploads, texture creation or draws.
    const auto afterPromotion = fixture->digest();
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    REQUIRE(scene.restoreLocal(beforePromotion));
    REQUIRE(scene.runtime(0)->figure.get() == original);
    REQUIRE(retainedEffect());
    REQUIRE(scene.update(1.0 / 60.0, {}) == PlayOutcome::Running);
    REQUIRE(scene.runtime(0)->figure.get() == promoted);
    REQUIRE(retainedEffect());
    CHECK(fixture->digest() == afterPromotion);
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
}

TEST_CASE("native watchdog profiling retains the exact unprofiled state digest",
          "[native-agreement-scene][native-digest-profile][assets]") {
    auto fixture = std::make_unique<Fixture>();
    const auto expected = fixture->scene.nativeAgreementDigest();
    REQUIRE(expected);
    PlayScene::NativeDigestProfile profile;
    CHECK(fixture->scene.nativeAgreementDigest(&profile) == expected);
    // The owners and their order are what peers agree on: a change bumps PlayScene.native and
    // updates tests/scripts/test_scene_digest_roster.py with this list.
    const std::vector<std::string_view> roster{
        "resources", "control", "players",       "world",   "fixtures",      "console-objects",
        "opponents", "attacks", "arsenal",       "effects", "portals",       "transporters",
        "departure", "arrival", "sumner",        "hud",     "messages",      "sumner-visit",
        "promotion", "relics",  "boss-sequence", "audio",   "challenge-hud", "camera-speech"};
    std::vector<std::string_view> owners;
    owners.reserve(profile.samples.size());
    for (const auto& sample : profile.samples) {
        owners.push_back(sample.owner);
    }
    CHECK(owners == roster);
    for (const auto& sample : profile.samples) {
        CAPTURE(sample.owner);
        CHECK(sample.microseconds >= 0);
    }
    CHECK(fixture->scene.nativeAgreementDigest() == expected);
}

TEST_CASE("native watchdog measures warm registry and actual state hashing in a loaded stage",
          "[.][native-digest-benchmark][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", 4);
    for (usize tick = 0; tick < 648; ++tick) {
        REQUIRE(fixture->scene.update(1.0 / 60.0, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(fixture->scene.spawning());
    REQUIRE_FALSE(fixture->scene.startCamera().active());
    REQUIRE_FALSE(fixture->scene.enemies().targets().empty());
    REQUIRE(fixture->scene.nativeAgreementDigest()); // Exclude initial binding/index costs.
    std::vector<double> samples;
    std::map<std::string_view, std::vector<double>> owners;
    PlayScene::NativeDigestProfile profile;
    constexpr usize kCount = 128;
    samples.reserve(kCount);
    for (usize index = 0; index < kCount; ++index) {
        fixture->scene.update(1.0 / 60.0, {});
        const auto began = std::chrono::steady_clock::now();
        const auto result = fixture->scene.nativeAgreementDigest();
        const auto ended = std::chrono::steady_clock::now();
        REQUIRE(result);
        samples.push_back(std::chrono::duration<double, std::micro>(ended - began).count());
        REQUIRE(fixture->scene.nativeAgreementDigest(&profile) == result);
        for (const auto& sample : profile.samples) {
            owners[sample.owner].push_back(sample.microseconds);
        }
    }
    std::ranges::sort(samples);
    log::info("native-watchdog-us level=G2 players=4 enemies={} generators={} samples={} "
              "p50={} p95={} max={}",
              fixture->scene.enemies().targets().size(), fixture->scene.generators().count(),
              kCount, samples[kCount / 2], samples[kCount * 95 / 100], samples.back());
    for (auto& [name, timings] : owners) {
        REQUIRE(timings.size() == kCount);
        std::ranges::sort(timings);
        log::info("native-watchdog-owner-us owner={} samples={} p50={} p95={} max={}", name, kCount,
                  timings[kCount / 2], timings[kCount * 95 / 100], timings.back());
    }
}
