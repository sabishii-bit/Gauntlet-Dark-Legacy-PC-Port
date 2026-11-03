#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digest = SimulationObservation::Digest;
constexpr f64 kStep = 1.0 / 60.0;

// The same input-only warmup used by the native rewind fixture. These are real
// scene ticks, not a public-observation or checkpoint-codec approximation.
PlayScene::Inputs inputsAt(u32 tick, bool wrong = false) {
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    PlayScene::Inputs inputs{};
    for (u8 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        PlayInput input;
        if (tick > 360) {
            const u32 elapsed = tick - 361;
            const u32 phase = (elapsed + seat * 30) % 240;
            const auto direction = kDirections[(elapsed / 120 + seat + (wrong ? 2 : 0)) % 4];
            input.move = {direction, 1};
            input.aimDirection = {direction.x, 0, -direction.y};
            input.attack = wrong ? phase >= 90 : phase < 90;
            input.attackPressed = wrong || phase == 0;
            input.strongAttack = !wrong && phase >= 120 && phase < 150;
            input.turbo = !wrong && phase >= 180 && phase < 190;
            input.defendPressed = !wrong && phase == 180;
            input.usePotion = wrong ? elapsed % 2 == 0 : elapsed % 600 == 300;
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
    LevelWorld world;
    GameServices context;
    PlayScene scene;
    std::vector<PartyMember> party;
    PlayOptions options;
    u32 tick = 0;

    explicit Fixture(std::string_view stage, s32 seats = 2,
                     std::optional<CharacterSave> first = std::nullopt) {
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", stage));
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level = stage == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(stage);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        for (s32 seat = 0; seat < seats; ++seat) {
            CharacterSave save = seat == 0 && first ? *first : CharacterSave{};
            save.name = "DIGEST";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            save.progress().inventory.addKeys(9);
            save.progress().inventory.addPotions(seat + 1, 9);
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        options.welcome = false;
        if (stage == "L1") {
            options.arrivalWorld = 10;
        }
        reopen();
    }

    void reopen() {
        REQUIRE(scene.open(device, context, world, party, options));
        tick = 0;
    }

    void advanceTo(u32 target) {
        while (tick < target) {
            REQUIRE(scene.update(kStep, inputsAt(++tick)) == PlayOutcome::Running);
        }
    }

    Digest digest(const PlayScene::LocalSnapshot* saved = nullptr) {
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        const auto draws = device.draws.size();
        const auto value = saved ? scene.nativeLocalDigest(*saved) : scene.nativeAgreementDigest();
        REQUIRE(value);
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.draws.size() == draws);
        return *value;
    }

    void render(f32 alpha) {
        device.draws.clear();
        REQUIRE(device.beginFrame());
        scene.render(device, glm::ortho(0.0f, 640.0f, 448.0f, 0.0f), 640, 448, false, alpha);
        device.endFrame();
    }

    void restore(const PlayScene::LocalSnapshot& saved) {
        const auto textures = device.texturesCreated;
        const auto uploads = device.textureUpdates;
        const auto draws = device.draws.size();
        REQUIRE(scene.restoreLocal(saved));
        CHECK(device.texturesCreated == textures);
        CHECK(device.textureUpdates == uploads);
        CHECK(device.draws.size() == draws);
        CHECK(digest() == digest(&saved));
    }

    bool entering() const {
        if (scene.spawning()) {
            return true;
        }
        for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
            if (scene.animator(seat) && scene.animator(seat)->entering()) {
                return true;
            }
        }
        return false;
    }
};

void checkSuffixes(Fixture& fixture, bool still = false) {
    const auto anchor = fixture.tick;
    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture.scene.saveLocal(saved));
    const auto before = fixture.digest();
    CHECK(fixture.digest(&saved) == before);
    std::array<Digest, 8> reference{};
    for (u32 offset = 1; offset <= 8; ++offset) {
        const auto input = still ? PlayScene::Inputs{} : inputsAt(anchor + offset);
        REQUIRE(fixture.scene.update(kStep, input) == PlayOutcome::Running);
        reference[offset - 1] = fixture.digest();
        PlayScene::LocalSnapshot current;
        REQUIRE(fixture.scene.saveLocal(current));
        CHECK(fixture.digest(&current) == reference[offset - 1]);
        CHECK(fixture.digest(&saved) == before);
    }
    fixture.restore(saved);
    for (u32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(anchor, distance);
        for (u32 offset = 1; offset <= distance; ++offset) {
            REQUIRE(fixture.scene.update(kStep, inputsAt(anchor + offset, true)) ==
                    PlayOutcome::Running);
        }
        const auto speculative = fixture.digest();
        fixture.render(((distance % 2) != 0u) ? -1.0f : 0.37f);
        CHECK(fixture.digest() == speculative);
        CHECK(fixture.digest(&saved) == before);
        fixture.restore(saved);
        CHECK(fixture.digest() == before);
        for (u32 offset = 1; offset <= distance; ++offset) {
            const auto input = still ? PlayScene::Inputs{} : inputsAt(anchor + offset);
            REQUIRE(fixture.scene.update(kStep, input) == PlayOutcome::Running);
            CHECK(fixture.digest() == reference[offset - 1]);
        }
        fixture.restore(saved);
    }
}
} // namespace

TEST_CASE("native scene hashes agree across independent loads and different draw cadence",
          "[play-scene-native-digest][native-state-digest][assets]") {
    const s32 seats = GENERATE(2, 4);
    auto first = std::make_unique<Fixture>("G2", seats);
    auto second = std::make_unique<Fixture>("G2", seats);
    for (const u32 anchor : {32U, 648U}) {
        CAPTURE(seats, anchor);
        first->advanceTo(anchor);
        second->advanceTo(anchor);
        CHECK(first->digest() == second->digest());
        PlayScene::LocalSnapshot saved;
        REQUIRE(first->scene.saveLocal(saved));
        const auto before = first->digest(&saved);
        for (u32 offset = 0; offset < 8; ++offset) {
            first->render(((offset % 2) != 0u) ? -1.0f : 0.0f);
            if (offset % 3 == 0) {
                second->render(1.0f);
            }
            CHECK(first->digest() == second->digest());
            CHECK(first->digest(&saved) == before);
            first->advanceTo(first->tick + 1);
            second->advanceTo(second->tick + 1);
            CHECK(first->digest() == second->digest());
        }
    }
}

TEST_CASE("native scene saved hashes survive all one through eight tick wrong branches",
          "[play-scene-native-digest][native-state-digest][local-rewind][assets]") {
    const auto domain = GENERATE(0, 1, 2);
    const std::string_view stage = domain == 2 ? "F2" : "G2";
    const s32 seats = domain == 0 ? 2 : 4;
    CAPTURE(stage, seats);
    auto fixture = std::make_unique<Fixture>(stage, seats);
    fixture->advanceTo(648);
    checkSuffixes(*fixture);
}

TEST_CASE("native scene saved hashes retain tower ceremony and costume replacement state",
          "[play-scene-native-digest][native-state-digest][local-rewind][promotion][assets]") {
    const bool promotion = GENERATE(false, true);
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = promotion ? 29 : 30;
    save.progress().crystals[8] = LevelTriggers::crystalsNeeded(8);
    if (!promotion) {
        save.progress().relics.addShard(8);
    }
    auto fixture = std::make_unique<Fixture>("L1", 2, save);
    REQUIRE(fixture->scene.promotion().active() == promotion);
    REQUIRE(fixture->scene.towerRelics().active() == !promotion);
    for (u32 guard = 0; guard < 500 && fixture->entering(); ++guard) {
        ++fixture->tick;
        REQUIRE(fixture->scene.update(kStep, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(fixture->entering());
    if (promotion) {
        for (u32 step = 0; step < 359; ++step) {
            ++fixture->tick;
            REQUIRE(fixture->scene.update(kStep, {}) == PlayOutcome::Running);
        }
        REQUIRE(fixture->scene.familiarTier(0) == 0);
    }
    const auto* initialFigure = fixture->scene.runtime(0)->figure.get();
    checkSuffixes(*fixture, true);
    CHECK(fixture->scene.runtime(0)->figure.get() == initialFigure);
    if (promotion) {
        REQUIRE(fixture->scene.update(kStep, {}) == PlayOutcome::Running);
        CHECK(fixture->scene.familiarTier(0) == 1);
        CHECK(fixture->scene.runtime(0)->figure.get() != initialFigure);
        PlayScene::LocalSnapshot promoted;
        REQUIRE(fixture->scene.saveLocal(promoted));
        CHECK(fixture->digest() == fixture->digest(&promoted));
    }
}

TEST_CASE("native scene hash detects hidden player projectile world and fixture changes",
          "[play-scene-native-digest][native-state-digest][local-rewind][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    fixture->advanceTo(32);
    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    const auto before = fixture->digest(&saved);
    const auto changed = [&] {
        CHECK(fixture->digest() != before);
        CHECK(fixture->digest(&saved) == before);
        fixture->restore(saved);
        CHECK(fixture->digest() == before);
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& player = const_cast<PlayerRuntime&>(fixture->scene.participants().front());
    player.surfaceGap += 0.25f;
    changed();
    player.entrySave.progress().health += 1;
    changed();
    player.shockGaps.push_back({31, 0.125f});
    changed();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& missiles = const_cast<EnemyMissiles&>(fixture->scene.enemyMissiles());
    missiles.burst({0, 0, 0}, 5, 10, 0, 1, -1);
    changed();
    REQUIRE(fixture->world.triggers().size() > 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& trigger = const_cast<LevelTrigger&>(fixture->world.triggers().trigger(0));
    trigger.shot = !trigger.shot;
    changed();
    fixture->world.setAmbientOffset(-0.125f);
    changed();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& chests = const_cast<LevelFixtures&>(fixture->scene.fixtures()).chests();
    REQUIRE(chests.size() > 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<Chests::Chest&>(chests.chest(0)).refusalLeft += 0.375f;
    changed();
}

TEST_CASE("native scene saved hashes reject empty foreign stale and unknown resource states",
          "[play-scene-native-digest][native-state-digest][local-rewind][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto foreign = std::make_unique<Fixture>("G2");
    const PlayScene::LocalSnapshot empty;
    CHECK_FALSE(fixture->scene.nativeLocalDigest(empty));
    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    CHECK(fixture->digest() == fixture->digest(&saved));
    CHECK_FALSE(foreign->scene.nativeLocalDigest(saved));

    // The snapshot itself does not load or bind resources. An unsupported
    // borrower must refuse a digest, not silently hash a partial scene.
    const TreeModel unknown;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& missiles = const_cast<EnemyMissiles&>(fixture->scene.enemyMissiles());
    missiles.launch(EnemyMissileKind::arrow(), {0, 10, 0}, {0, 10, 30}, 1, &unknown, 0);
    PlayScene::LocalSnapshot unregistered;
    REQUIRE(fixture->scene.saveLocal(unregistered));
    CHECK_FALSE(fixture->scene.nativeAgreementDigest());
    CHECK_FALSE(fixture->scene.nativeLocalDigest(unregistered));
    fixture->restore(saved);
    fixture->scene.close();
    CHECK_FALSE(fixture->scene.nativeAgreementDigest());
    CHECK_FALSE(fixture->scene.nativeLocalDigest(saved));
    fixture->reopen();
    CHECK_FALSE(fixture->scene.nativeLocalDigest(saved));
    PlayScene::LocalSnapshot reopened;
    REQUIRE(fixture->scene.saveLocal(reopened));
    CHECK(fixture->digest() == fixture->digest(&reopened));
}
