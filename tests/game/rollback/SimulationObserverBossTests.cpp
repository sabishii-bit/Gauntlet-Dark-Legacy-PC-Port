#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/scenes/NativeDigestProfileSupport.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlayScene::Inputs combatJournal(u32 tick) {
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    PlayScene::Inputs inputs;
    for (u32 seat = 0; seat < 4; ++seat) {
        auto& input = inputs[seat];
        if (tick > 360) {
            const u32 elapsed = tick - 361;
            const u32 phase = (elapsed + seat * 30) % 240;
            const Vec2 direction = kDirections[(elapsed / 120 + seat) % kDirections.size()];
            input.move = {direction, 1};
            input.aimDirection = Vec3{direction.x, 0, -direction.y};
            input.attack = phase < 90;
            input.attackPressed = phase == 0;
            input.strongAttack = phase >= 120 && phase < 150;
            input.turbo = phase >= 180 && phase < 190;
            input.defendPressed = phase == 180;
            input.usePotion = elapsed % 600 == 300;
            input.menu.select = elapsed % 120 == 0;
            input.menu.buttonPressed = input.menu.select;
        }
    }
    return inputs;
}

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog levels;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;

    explicit Fixture(std::string_view name) {
        device.recordDraws = false;
        config.timing.tickRate = 60;
        REQUIRE(levels.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level = levels.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &levels;
        context.strings = &strings;
        context.unpackedRoot = root;
        std::vector<PartyMember> party;
        for (s32 seat = 0; seat < 4; ++seat) {
            CharacterSave character;
            character.name = "PROBE";
            character.character = seat;
            character.color = seat;
            character.progress().health = 2000;
            character.progress().inventory.addKeys(9);
            character.progress().inventory.addPotions(seat + 1, 9);
            party.push_back({seat, character});
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(
            scene.open(device, context, world, party, options, {.stopTimeTotal = &stopTimeTotal}));
    }
};
} // namespace

TEST_CASE("native boss combat keeps the represented diagnostic observations available",
          "[simulation-observation-boss][assets]") {
    const auto name =
        GENERATE(std::string_view{"A5"}, std::string_view{"B6"}, std::string_view{"F2"},
                 std::string_view{"G5"}, std::string_view{"H4"}, std::string_view{"J5"},
                 std::string_view{"K5"});
    struct Boundary {
        std::string_view name;
        u32 tick;
    };
    constexpr std::array kBoundaries{Boundary{"A5", 738}, Boundary{"B6", 604}, Boundary{"F2", 376},
                                     Boundary{"G5", 627}, Boundary{"H4", 354}, Boundary{"J5", 609},
                                     Boundary{"K5", 439}};
    const u32 boundary = std::ranges::find(kBoundaries, name, &Boundary::name)->tick;
    // The journal never brings A5's four into the Chimera's arena together, and only the whole
    // party there lowers the lift that arms it: they are put down in the arena before the
    // journal starts, so that its window holds the Chimera awake.
    const bool arena = name == "A5";
    constexpr u32 kArenaTick = 300;
    constexpr Vec3 kArena{-3, 0, -11};
    auto fixture = std::make_unique<Fixture>(name);
    PlayScene::LocalSnapshot before;
    std::vector<PlayScene::NativeDigestProfile> uninterrupted;
    for (u32 tick = 1; tick <= boundary + 8; ++tick) {
        if (arena && tick == kArenaTick) {
            REQUIRE_FALSE(fixture->scene.spawning());
            REQUIRE(fixture->scene.relocate(kArena));
        }
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick)) == PlayOutcome::Running);
        if (tick < boundary - 1) {
            continue;
        }
        if (arena) {
            CHECK(fixture->scene.bosses().view().awake);
        }
        PlayScene::NativeDigestProfile profile;
        const auto digest = fixture->scene.nativeAgreementDigest(&profile);
        for (const auto& sample : profile.samples) {
            CAPTURE(name, tick, sample.owner);
            CHECK(sample.runningDigest);
        }
        CHECK(fixture->scene.nativeAgreementDigest() == digest);
        if (tick == boundary - 1) {
            REQUIRE(fixture->scene.saveLocal(before));
        } else {
            uninterrupted.push_back(std::move(profile));
        }
    }
    REQUIRE(fixture->scene.restoreLocal(before));
    for (u32 tick = boundary; tick < boundary + 8; ++tick) {
        auto wrong = combatJournal(tick);
        for (auto& input : wrong) {
            input.move.direction = -input.move.direction;
            input.attack = !input.attack;
            input.attackPressed = true;
            input.strongAttack = true;
        }
        REQUIRE(fixture->scene.update(1.0 / 60, wrong) == PlayOutcome::Running);
    }
    REQUIRE(fixture->scene.restoreLocal(before));
    for (u32 tick = boundary; tick <= boundary + 8; ++tick) {
        CAPTURE(name, tick);
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick)) == PlayOutcome::Running);
        PlayScene::NativeDigestProfile replayed;
        CHECK(fixture->scene.nativeAgreementDigest(&replayed));
        CHECK(test::firstDivergence(replayed, uninterrupted[tick - boundary]).empty());
    }
}

TEST_CASE("campaign stages bind complete represented observation catalogs",
          "[simulation-observation-binding][assets]") {
    const auto name =
        GENERATE(std::string_view{"E2"}, std::string_view{"H3"}, std::string_view{"K1"});
    CAPTURE(name);
    auto fixture = std::make_unique<Fixture>(name);
    PlayScene::NativeDigestProfile profile;
    CHECK(fixture->scene.nativeAgreementDigest(&profile));
    for (const auto& sample : profile.samples) {
        CAPTURE(sample.owner);
        CHECK(sample.runningDigest);
    }

    // E2 owns a Skorne arena, while H3 and K1 author 399 and 303 particle emitters; the
    // native digest keeps every one of their values.
    if (name != "E2") {
        REQUIRE(fixture->world.particles().size() == (name == "H3" ? 399 : 303));
    }

    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    std::array<PlayScene::NativeDigestProfile, 8> uninterrupted;
    for (u32 tick = 0; tick < uninterrupted.size(); ++tick) {
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick + 1)) == PlayOutcome::Running);
        CHECK(fixture->scene.nativeAgreementDigest(&uninterrupted[tick]));
    }
    REQUIRE(fixture->scene.restoreLocal(saved));
    for (u32 tick = 0; tick < uninterrupted.size(); ++tick) {
        auto wrong = combatJournal(tick + 361);
        wrong[0].usePotion = true;
        REQUIRE(fixture->scene.update(1.0 / 60, wrong) == PlayOutcome::Running);
    }
    REQUIRE(fixture->scene.restoreLocal(saved));
    for (u32 tick = 0; tick < uninterrupted.size(); ++tick) {
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick + 1)) == PlayOutcome::Running);
        PlayScene::NativeDigestProfile replayed;
        CHECK(fixture->scene.nativeAgreementDigest(&replayed));
        CHECK(test::firstDivergence(replayed, uninterrupted[tick]).empty());
    }
}
