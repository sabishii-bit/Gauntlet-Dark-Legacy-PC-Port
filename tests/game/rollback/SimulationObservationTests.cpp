#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterProfile.h"
#include "game/combat/PowerupFlags.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerPowerups.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/NativeDigestProfileSupport.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;

    explicit Fixture(std::string_view name, bool stopTime = false, bool probeParty = false) {
        REQUIRE(catalog.load(root));
        const auto level = name == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        std::vector<PartyMember> party;
        PlayScene::Links links;
        if (probeParty) {
            config.timing.tickRate = 60;
            REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
            context.strings = &strings;
            links.stopTimeTotal = &stopTimeTotal;
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
        REQUIRE(scene.open(device, context, world, party, options, links));
    }
};
} // namespace

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

TEST_CASE("native startup tower and time-stop ticks have read-only subsystem observations",
          "[simulation-observation][assets]") {
    const auto name = GENERATE(std::string_view{"G1"}, std::string_view{"L1"});
    const bool stopTime = GENERATE(false, true);
    CAPTURE(name, stopTime);
    auto fixture = std::make_unique<Fixture>(name, stopTime);
    REQUIRE(fixture->scene.spawning());
    REQUIRE(PlayerPowerups::timeStopped(fixture->scene.participants()) == stopTime);
    for (s32 tick = 1; tick <= 4; ++tick) {
        CAPTURE(tick);
        REQUIRE(fixture->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        const auto textures = fixture->device.texturesCreated;
        const auto updates = fixture->device.textureUpdates;
        const auto position = fixture->scene.actor(0)->position();
        PlayScene::NativeDigestProfile profile;
        const auto digest = fixture->scene.nativeAgreementDigest(&profile);
        for (const auto& sample : profile.samples) {
            CAPTURE(sample.owner);
            REQUIRE(sample.runningDigest);
        }
        CHECK(fixture->scene.nativeAgreementDigest() == digest);
        CHECK(fixture->scene.actor(0)->position() == position);
        CHECK(fixture->device.texturesCreated == textures);
        CHECK(fixture->device.textureUpdates == updates);
        CHECK(fixture->device.draws.empty());
    }

    PlayScene::NativeDigestProfile before;
    REQUIRE(fixture->scene.nativeAgreementDigest(&before));
    LevelWorld::LocalSnapshot worldBefore;
    fixture->world.saveLocal(worldBefore);
    // Mutate an actual native owner, not a synthesized checkpoint or packet.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the fixture owns this live actor.
    auto& actor = const_cast<PlayerActor&>(*fixture->scene.actor(0));
    ++actor.save().progress().gold;
    PlayScene::NativeDigestProfile changedParty;
    REQUIRE(fixture->scene.nativeAgreementDigest(&changedParty));
    CHECK(test::firstDivergence(before, changedParty) == "players");
    LevelWorld::LocalSnapshot worldAfterGold;
    fixture->world.saveLocal(worldAfterGold);
    CHECK(fixture->world.equalLocal(worldAfterGold, worldBefore));
    fixture->world.setAmbientOffset(0.125f);
    PlayScene::NativeDigestProfile changedWorld;
    REQUIRE(fixture->scene.nativeAgreementDigest(&changedWorld));
    // The stream runs control, players, then world, so the party's part is as it was.
    CHECK(test::firstDivergence(changedParty, changedWorld) == "world");
}

TEST_CASE("Ghost Town native gas transitions remain observable without replay admission",
          "[simulation-observation][simulation-observation-gas][assets]") {
    auto fixture = std::make_unique<Fixture>("G2", false, true);
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
        const EnemyMissiles& missiles = fixture->scene.enemyMissiles();
        for (const auto& burst : missiles.bursts()) {
            if ((burst.blast.flags & EnemyBlast::kGas) != 0) {
                gasSeen = true;
                secondStageSeen |= burst.stage != 0;
            }
        }
        if (tick < 640 && !gasSeen) {
            continue;
        }
        EnemyMissiles::LocalSnapshot flights;
        missiles.saveLocal(flights);
        const auto textures = fixture->device.texturesCreated;
        const auto updates = fixture->device.textureUpdates;
        PlayScene::NativeDigestProfile profile;
        const auto digest = fixture->scene.nativeAgreementDigest(&profile);
        for (const auto& sample : profile.samples) {
            CAPTURE(sample.owner);
            REQUIRE(sample.runningDigest);
        }
        CHECK(fixture->scene.nativeAgreementDigest() == digest);
        EnemyMissiles::LocalSnapshot observed;
        missiles.saveLocal(observed);
        CHECK(missiles.equalLocal(observed, flights));
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
    // The fixture owns this soundscape; the public scene view is read-only.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& audio = const_cast<LevelSoundscape&>(fixture->scene.soundscape());
    PlayScene::NativeDigestProfile before;
    REQUIRE(fixture->scene.nativeAgreementDigest(&before));
    REQUIRE(audio.narrationTiming().empty());
    REQUIRE(audio.queueNarration("S_GRAB", LevelSoundscape::Narrator::Primary) == kNoSound);
    REQUIRE_FALSE(audio.narrationTiming().empty());
    // The audio owner comes after the party and the world, so a first difference there
    // leaves both as they were.
    PlayScene::NativeDigestProfile queued;
    const auto queuedDigest = fixture->scene.nativeAgreementDigest(&queued);
    REQUIRE(queuedDigest);
    CHECK(test::firstDivergence(before, queued) == "audio");
    CHECK(fixture->scene.nativeAgreementDigest() == queuedDigest);

    audio.holdNarration(true);
    PlayScene::NativeDigestProfile held;
    REQUIRE(fixture->scene.nativeAgreementDigest(&held));
    CHECK(test::firstDivergence(queued, held) == "audio");
    audio.holdNarration(false);
    CHECK(fixture->scene.nativeAgreementDigest() == queuedDigest);
    audio.updateNarration(1.0f / 60);
    PlayScene::NativeDigestProfile advanced;
    REQUIRE(fixture->scene.nativeAgreementDigest(&advanced));
    CHECK(test::firstDivergence(queued, advanced) == "audio");
    CHECK(fixture->device.draws.empty());
}
