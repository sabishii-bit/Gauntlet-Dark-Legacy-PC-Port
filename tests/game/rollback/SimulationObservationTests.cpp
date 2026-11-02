#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterProfile.h"
#include "game/combat/PowerupFlags.h"
#include "game/enemies/OpponentResources.h"
#include "game/party/PlayerPowerups.h"
#include "game/rollback/SessionInputs.h"
#include "game/rollback/SimulationObservation.h"
#include "game/rollback/SimulationObserver.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::array<std::string_view, 8> kNames{
    "party_combat",   "opponent_followup", "opponent_generators",
    "opponent_swarm", "opponent_missiles", "opponent_placements",
    "stage",          "scene_control"};

/** The subsystem of `observation` called `name`. */
const SimulationObservation::Subsystem& named(const SimulationObservation& observation,
                                              std::string_view name) {
    const auto found =
        std::ranges::find(observation.subsystems, name, &SimulationObservation::Subsystem::name);
    REQUIRE(found != observation.subsystems.end());
    return *found;
}

void checkSchema(const SimulationObservation& observation) {
    CHECK_FALSE(observation.complete);
    REQUIRE_FALSE(observation.unobserved.empty());
    REQUIRE(observation.subsystems.size() == kNames.size());
    for (usize index = 0; index < kNames.size(); ++index) {
        CAPTURE(index, observation.subsystems[index].name,
                observation.subsystems[index].unavailable);
        CHECK(observation.subsystems[index].name == kNames[index]);
        CHECK(observation.subsystems[index].digest.has_value() ==
              observation.subsystems[index].unavailable.empty());
    }
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
    PlayScene scene;
    SimulationObserver observer{scene};

    explicit Fixture(std::string_view name, bool stopTime = false, bool probeParty = false) {
        REQUIRE(catalog.load(root));
        const auto level = name == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        GameContext context;
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        std::vector<PartyMember> party;
        if (probeParty) {
            config.timing.tickRate = 60;
            REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
            context.strings = &strings;
            context.stopTimeTotal = &stopTimeTotal;
            for (s32 seat = 0; seat < 2; ++seat) {
                CharacterSave save;
                save.name = "PROBE";
                save.character = seat;
                save.color = seat;
                save.progress().health = 2000;
                save.progress().inventory.addKeys(9);
                save.progress().inventory.addPotions(seat + 1, 9);
                party.push_back({seat, save});
            }
        } else {
            CharacterSave save;
            save.name = "WATCH";
            if (stopTime) {
                save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
            }
            REQUIRE(CharacterProfile::capture(save));
            party.push_back({0, save});
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }
    void bind() { REQUIRE(observer.bind()); }
};
} // namespace

TEST_CASE("closed scene observations retain every subsystem and explicit failures",
          "[simulation-observation]") {
    auto scene = std::make_unique<PlayScene>();
    SimulationObserver observer{*scene};
    const auto first = observer.observe();
    checkSchema(first);
    for (usize index = 0; index + 1 < kNames.size(); ++index) {
        CHECK_FALSE(first.subsystems[index].digest);
    }
    CHECK(first.subsystems.back().digest);
    CHECK(observer.observe() == first);
}

TEST_CASE("unbound scene observation reports binding failures without gating native startup",
          "[simulation-observation][assets]") {
    auto fixture = std::make_unique<Fixture>("G1");
    REQUIRE(fixture->scene.spawning());
    REQUIRE(fixture->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
    const auto observation = fixture->observer.observe();
    checkSchema(observation);
    CHECK_FALSE(named(observation, "party_combat").digest);
    CHECK(named(observation, "party_combat").unavailable.find("catalog") != std::string::npos);
    CHECK_FALSE(named(observation, "stage").digest);
    CHECK(named(observation, "stage").unavailable.find("bound") != std::string::npos);
    for (const std::string_view name :
         {"opponent_followup", "opponent_generators", "opponent_swarm", "opponent_missiles",
          "opponent_placements", "scene_control"}) {
        CAPTURE(name, named(observation, name).unavailable);
        CHECK(named(observation, name).digest);
    }
}

TEST_CASE("native agreement observes live party and opponents without replica bindings",
          "[native-agreement-scene][assets]") {
    auto fixture = std::make_unique<Fixture>("G1");
    REQUIRE(fixture->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
    const auto before = fixture->scene.nativeAgreementDigest();
    REQUIRE(before);
    CHECK(fixture->scene.nativeAgreementDigest() == before);
    CHECK(fixture->device.draws.empty());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& actor = const_cast<PlayerActor&>(*fixture->scene.actor(0));
    const auto position = actor.position();
    actor.place(position + Vec3{0.125f, 0, 0});
    CHECK(fixture->scene.nativeAgreementDigest() != before);
    actor.place(position);
    REQUIRE(fixture->scene.nativeAgreementDigest() == before);
    // Unlike the profile projection, native agreement includes the stored bank
    // copy too. Restoring a native value must not silently normalize hidden data.
    ++actor.save().progress().gold;
    CHECK(fixture->scene.nativeAgreementDigest() != before);
    --actor.save().progress().gold;
    ++actor.save().gold;
    CHECK(fixture->scene.nativeAgreementDigest() != before);
    --actor.save().gold;
    REQUIRE(fixture->scene.nativeAgreementDigest() == before);
    fixture->scene.close();
    CHECK_FALSE(fixture->scene.nativeAgreementDigest());
}

TEST_CASE("diagnostic bindings expire when a scene closes or reopens",
          "[simulation-observation][assets]") {
    auto fixture = std::make_unique<Fixture>("G1");
    fixture->bind();
    REQUIRE(named(fixture->observer.observe(), "party_combat").digest);
    const std::array party{PartyMember{0, fixture->scene.actor(0)->save()}};
    fixture->scene.close();
    const auto closed = fixture->observer.observe();
    CHECK_FALSE(named(closed, "party_combat").digest);
    CHECK_FALSE(named(closed, "stage").digest);
    CHECK_FALSE(fixture->observer.bind());

    GameContext context;
    context.config = &fixture->config;
    context.levels = &fixture->catalog;
    context.unpackedRoot = fixture->root;
    PlayOptions options;
    options.welcome = false;
    REQUIRE(fixture->scene.open(fixture->device, context, fixture->world, party, options));
    const auto unbound = fixture->observer.observe();
    CHECK_FALSE(named(unbound, "party_combat").digest);
    CHECK_FALSE(named(unbound, "stage").digest);
    REQUIRE(fixture->scene.nativeAgreementDigest());
    fixture->bind();
    const auto rebound = fixture->observer.observe();
    for (const auto& subsystem : rebound.subsystems) {
        INFO(subsystem.name << ": " << subsystem.unavailable);
        CHECK(subsystem.digest);
    }
}

TEST_CASE("native startup tower and time-stop ticks have read-only subsystem observations",
          "[simulation-observation][assets]") {
    const auto name = GENERATE(std::string_view{"G1"}, std::string_view{"L1"});
    const bool stopTime = GENERATE(false, true);
    CAPTURE(name, stopTime);
    auto fixture = std::make_unique<Fixture>(name, stopTime);
    fixture->bind();
    REQUIRE(fixture->scene.spawning());
    REQUIRE(PlayerPowerups::timeStopped(fixture->scene.participants()) == stopTime);
    for (s32 tick = 1; tick <= 4; ++tick) {
        CAPTURE(tick);
        REQUIRE(fixture->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        const auto textures = fixture->device.texturesCreated;
        const auto updates = fixture->device.textureUpdates;
        const auto position = fixture->scene.actor(0)->position();
        const auto observation = fixture->observer.observe();
        checkSchema(observation);
        for (const auto& subsystem : observation.subsystems) {
            CAPTURE(subsystem.name, subsystem.unavailable);
            REQUIRE(subsystem.digest);
        }
        CHECK(fixture->observer.observe() == observation);
        CHECK(fixture->scene.actor(0)->position() == position);
        CHECK(fixture->device.texturesCreated == textures);
        CHECK(fixture->device.textureUpdates == updates);
        CHECK(fixture->device.draws.empty());
    }

    const auto before = fixture->observer.observe();
    // Mutate an actual native owner, not a synthesized checkpoint or packet.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the fixture owns this live actor.
    auto& actor = const_cast<PlayerActor&>(*fixture->scene.actor(0));
    ++actor.save().progress().gold;
    const auto changedParty = fixture->observer.observe();
    REQUIRE(named(changedParty, "party_combat").digest);
    CHECK(named(changedParty, "party_combat").digest != named(before, "party_combat").digest);
    CHECK(named(changedParty, "stage").digest == named(before, "stage").digest);
    fixture->world.setAmbientOffset(0.125f);
    const auto changedWorld = fixture->observer.observe();
    REQUIRE(named(changedWorld, "stage").digest);
    CHECK(named(changedWorld, "stage").digest != named(changedParty, "stage").digest);
    CHECK(named(changedWorld, "party_combat").digest == named(changedParty, "party_combat").digest);
}

TEST_CASE("Ghost Town native gas transitions remain observable without replay admission",
          "[simulation-observation][simulation-observation-gas][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", false, true);
    fixture->bind();
    // Native probe journal v1, including the untouched startup and input-packet
    // quantization. This reached the first former adapter refusal at tick 650;
    // no enemy, action, placement or cloud state is injected into the scene.
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    bool gasSeen = false;
    bool secondStageSeen = false;
    for (u32 tick = 1; tick <= 900; ++tick) {
        PlayScene::Inputs inputs;
        for (u8 seat = 0; seat < 2; ++seat) {
            PlayInput input;
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
            inputs[seat] =
                SessionInputs::playInput(SessionInputs::command(input, 1, tick, 1, seat));
        }
        CAPTURE(tick);
        REQUIRE(fixture->scene.update(1.0 / 60, inputs) == PlayOutcome::Running);
        const auto missiles = fixture->scene.enemyMissiles().checkpoint(
            OpponentResources::missiles(fixture->scene.enemies()));
        REQUIRE(missiles);
        for (const auto& burst : missiles->bursts) {
            if ((burst.blast.flags & EnemyBlast::kGas) != 0) {
                gasSeen = true;
                secondStageSeen |= burst.stage != 0;
            }
        }
        if (tick < 640 && !gasSeen) {
            continue;
        }
        const auto textures = fixture->device.texturesCreated;
        const auto updates = fixture->device.textureUpdates;
        const auto observation = fixture->observer.observe();
        checkSchema(observation);
        for (const auto& subsystem : observation.subsystems) {
            CAPTURE(subsystem.name, subsystem.unavailable);
            REQUIRE(subsystem.digest);
        }
        CHECK(fixture->observer.observe() == observation);
        CHECK(fixture->scene.enemyMissiles().checkpoint(
                  OpponentResources::missiles(fixture->scene.enemies())) == missiles);
        CHECK(fixture->device.texturesCreated == textures);
        CHECK(fixture->device.textureUpdates == updates);
        CHECK(fixture->device.draws.empty());
    }
    REQUIRE(gasSeen);
    REQUIRE(secondStageSeen);
}

TEST_CASE("native observation includes silent logical narration and hold timing",
          "[simulation-observation][assets]") {
    test::assetOrSkip("audio/VOICE1.vbk");
    auto fixture = std::make_unique<Fixture>("G1");
    fixture->bind();
    // The fixture owns this soundscape; the public scene view is read-only.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& audio = const_cast<LevelSoundscape&>(fixture->scene.soundscape());
    const auto before = fixture->observer.observe();
    REQUIRE(audio.narrationTiming().empty());
    REQUIRE(audio.queueNarration("S_GRAB", LevelSoundscape::Narrator::Primary) == kNoSound);
    REQUIRE_FALSE(audio.narrationTiming().empty());
    const auto queued = fixture->observer.observe();
    checkSchema(queued);
    CHECK(named(queued, "scene_control").digest != named(before, "scene_control").digest);
    CHECK(named(queued, "party_combat").digest == named(before, "party_combat").digest);
    CHECK(named(queued, "stage").digest == named(before, "stage").digest);
    CHECK(fixture->observer.observe() == queued);

    audio.holdNarration(true);
    const auto held = fixture->observer.observe();
    CHECK(named(held, "scene_control").digest != named(queued, "scene_control").digest);
    audio.holdNarration(false);
    CHECK(fixture->observer.observe() == queued);
    audio.updateNarration(1.0f / 60);
    const auto advanced = fixture->observer.observe();
    CHECK(named(advanced, "scene_control").digest != named(queued, "scene_control").digest);
    CHECK(named(advanced, "party_combat").digest == named(queued, "party_combat").digest);
    CHECK(named(advanced, "stage").digest == named(queued, "stage").digest);
    CHECK(fixture->device.draws.empty());
}
