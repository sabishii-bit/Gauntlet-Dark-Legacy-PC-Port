#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/rollback/SimulationObserver.h"

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
    SimulationObserver observer{scene};

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
        REQUIRE(observer.bind());
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
    auto fixture = std::make_unique<Fixture>(name);
    PlayScene::LocalSnapshot before;
    std::vector<SimulationObservation> uninterrupted;
    for (u32 tick = 1; tick <= boundary + 8; ++tick) {
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick)) == PlayOutcome::Running);
        if (tick < boundary - 1) {
            continue;
        }
        const auto observation = fixture->observer.observe();
        CHECK_FALSE(observation.complete);
        for (const auto& subsystem : observation.subsystems) {
            CAPTURE(name, tick, subsystem.name, subsystem.unavailable);
            CHECK(subsystem.digest);
        }
        CHECK(fixture->observer.observe() == observation);
        if (tick == boundary - 1) {
            REQUIRE(fixture->scene.saveLocal(before));
        } else {
            uninterrupted.push_back(observation);
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
        CHECK(fixture->observer.observe() == uninterrupted[tick - boundary]);
    }
}

TEST_CASE("boss observations do not expand ordinary combat packet admission",
          "[simulation-observation-boss][combat-state-packet]") {
    CombatStatePacket::State state;
    state.combat.health = Mt19937{0x12345678}.checkpoint();
    REQUIRE(state.valid());
    state.combat.effects.nextId = 2;
    const bool following = GENERATE(false, true);
    if (following) {
        state.adapter.critterEffects.push_back({1, 0, true});
    } else {
        state.adapter.moveEffects.push_back({1, 0, true});
    }
    // The reference itself names an issued effect and no missing player. Boss
    // ownership is nevertheless not representable by this ordinary wire state.
    CHECK(state.references());
    CHECK_FALSE(state.adapter.valid());
    CHECK_FALSE(state.valid());
    CHECK_FALSE(CombatStatePacket::encode(state));
}

TEST_CASE("campaign stages bind complete represented observation catalogs",
          "[simulation-observation-binding][assets]") {
    const auto name =
        GENERATE(std::string_view{"E2"}, std::string_view{"H3"}, std::string_view{"K1"});
    CAPTURE(name);
    auto fixture = std::make_unique<Fixture>(name);
    const auto observation = fixture->observer.observe();
    CHECK_FALSE(observation.complete);
    for (const auto& subsystem : observation.subsystems) {
        CAPTURE(subsystem.name, subsystem.unavailable);
        CHECK(subsystem.digest);
    }

    // These native stages are outside the old transferable-state domain: E2
    // owns a Skorne arena, while H3/K1 have 399/303 authored particle emitters.
    // Observation keeps every represented value, without admitting restoration
    // through the ordinary checkpoint or relaxing the old wire emitter budget.
    LevelWorldCheckpoint ordinary(fixture->world);
    CHECK_FALSE(ordinary.bindResources(fixture->device));
    CHECK_FALSE(ordinary.checkpoint());
    LevelWorldCheckpoint values(fixture->world);
    REQUIRE(values.bindObservation(fixture->device));
    REQUIRE(values.bindsObservation(fixture->world));
    const auto native = values.observe();
    REQUIRE(native);
    REQUIRE(native->validObservation());
    CHECK_FALSE(values.checkpoint());
    CHECK_FALSE(values.prepareRestore(*native));

    StageStatePacket::State represented;
    represented.world = *native;
    const auto catalog = StageStatePacket::Catalog::buildObservation(represented);
    REQUIRE(catalog);
    CHECK(StageStatePacket::canonicalDigest(represented, *catalog));
    if (name != "E2") {
        REQUIRE(native->particles.entries.size() == (name == "H3" ? 399 : 303));
        CHECK_FALSE(native->valid());
        CHECK_FALSE(StageStatePacket::Catalog::build(represented));
        CHECK_FALSE(StageStatePacket::encode(represented, *catalog));
        CHECK_FALSE(StageStatePacket::decodedSize(represented, *catalog));

        auto malformed = represented;
        malformed.world.particles.entries.back().emitter.node[3].y =
            std::numeric_limits<f32>::quiet_NaN();
        CHECK_FALSE(malformed.world.validObservation());
        CHECK_FALSE(StageStatePacket::Catalog::buildObservation(malformed));
        CHECK_FALSE(StageStatePacket::encode(malformed, *catalog));
        malformed = represented;
        malformed.world.particleSources.back() = malformed.world.particleSources.front();
        CHECK_FALSE(malformed.world.validObservation());
        CHECK_FALSE(StageStatePacket::Catalog::buildObservation(malformed));
    }

    PlayScene::LocalSnapshot saved;
    REQUIRE(fixture->scene.saveLocal(saved));
    std::array<SimulationObservation, 8> uninterrupted;
    for (u32 tick = 0; tick < uninterrupted.size(); ++tick) {
        REQUIRE(fixture->scene.update(1.0 / 60, combatJournal(tick + 1)) == PlayOutcome::Running);
        uninterrupted[tick] = fixture->observer.observe();
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
        CHECK(fixture->observer.observe() == uninterrupted[tick]);
    }
}
