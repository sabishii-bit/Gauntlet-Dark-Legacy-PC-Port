#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/AttackSceneFixture.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr u64 kEpoch = 71;
constexpr f64 kStep = 1.0 / 60;

struct GeneratorSounds {
    std::filesystem::path root = test::attackSceneAssets();
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    SceneSoundEvents events;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;

    GeneratorSounds() {
        const auto suffix = world.ref().name.empty() ? 'G' : world.ref().name.front();
        const std::array<test::NativeSoundSample, 1> samples{
            {{24000, std::vector<s16>(24000, 4096)}}};
        test::writeNativeSoundBank(root / "audio/COMMON",
                                   std::format(R"({{"sounds":[
                {{"name":"S_GENDAM{}","sequence":[{{"sample":0}}]}},
                {{"name":"S_GENKILL{}","sequence":[{{"sample":0}}]}}
            ]}})",
                                               suffix, suffix),
                                   samples);
        audio.open(root, nullptr, nullptr);
        players[0].actor.spawn(3, {}, nullptr, {0, 0, 30}, 0);
        opponents.open({device, world, weapons, effects, audio, root}, players);
        REQUIRE(opponents.enemies().loadKind(kGruntKind));
        const auto directory = test::scratchDirectory("generator-sound-identities");
        writeTextFile(directory / "world.json", R"({
            "objects":[{"name":"GROUND","position":[0,0,0]}],
            "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                "collisionType":1,"hitPoints":10}],
            "itemInstances":[
                {"info":0,"minPlayers":1,"position":[0,0,0],
                 "params":[3,0,7,0,2,0,1,0,0,0,0,0]},
                {"info":0,"minPlayers":1,"position":[30,0,0],
                 "params":[3,0,7,0,2,0,1,0,0,0,0,0]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(opponents.generators().bind(device, layout, opponents.enemies(), nullptr, {}, 1));
        REQUIRE(opponents.generators().count() == 2);
        events.open(kEpoch, nullptr);
        audio.recordTo(&events);
    }
    GeneratorSounds(const GeneratorSounds&) = delete;
    GeneratorSounds& operator=(const GeneratorSounds&) = delete;
    ~GeneratorSounds() {
        opponents.close();
        audio.close();
    }
    SceneSoundEvents::Frame strike(std::span<const s32> order, f32 damage = 1) {
        events.beginTick(1);
        {
            auto scope = audio.soundScope({90, 0, 1}, 1);
            for (const s32 generator : order) {
                opponents.strikeGenerator(generator, damage, -1, players);
            }
        }
        return events.endFrame();
    }
};

TEST_CASE("generator damage audio retains source identity when another strike disappears",
          "[native-sound-identity][native-combat-audio][scene-sound-events]") {
    GeneratorSounds fixture;
    auto& generators = fixture.opponents.generators();
    Generators::LocalSnapshot before;
    generators.saveLocal(before);
    const std::array<s32, 2> forward{0, 1};
    const auto original = fixture.strike(forward);
    REQUIRE(original.finite.events.size() == 2);
    for (usize index = 0; index < forward.size(); ++index) {
        const auto& id = original.finite.events[index].id;
        CHECK(id.emitter == FrameEventEmitter{46, index, generators.instanceOf(forward[index])});
        CHECK(id.cue == 1);
        CHECK(id.occurrence == 0);
    }
    REQUIRE(generators.restoreLocal(before));
    const std::array<s32, 2> reverse{1, 0};
    const auto reordered = fixture.strike(reverse);
    REQUIRE(reordered.finite.events.size() == 2);
    CHECK(reordered.finite.events[0] == original.finite.events[1]);
    CHECK(reordered.finite.events[1] == original.finite.events[0]);

    REQUIRE(fixture.events.reconcileFrames(1, std::span{&original, 1}));
    REQUIRE(generators.restoreLocal(before));
    const std::array<s32, 1> remaining{1};
    const auto corrected = fixture.strike(remaining);
    REQUIRE(corrected.finite.events.size() == 1);
    CHECK(corrected.finite.events[0] == original.finite.events[1]);
    const auto delta = fixture.events.reconcileFrames(1, std::span{&corrected, 1});
    REQUIRE(delta);
    CHECK(delta.actions.empty());
    REQUIRE(delta.unretractable.size() == 1);
    CHECK(delta.unretractable[0] == original.finite.events[0].id);

    REQUIRE(generators.restoreLocal(before));
    const auto destroyed = fixture.strike(remaining, 1000);
    REQUIRE(destroyed.finite.events.size() == 1);
    CHECK(destroyed.finite.events[0].id.emitter == original.finite.events[1].id.emitter);
    CHECK(destroyed.finite.events[0].id.cue == 3);
}

struct PartySounds {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    GameServices context;
    PlayScene scene;

    PartySounds() {
        REQUIRE(catalog.load(root));
        REQUIRE(world.load(device, root, *catalog.byName("G1")));
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        CharacterSave save;
        save.name = "SOUND";
        save.progress().health = 5000;
        // Party vector order is deliberately not the hardware/input seat order.
        const std::array party{PartyMember{3, save}, PartyMember{1, save}};
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{18.8f, 0.2f, 0.7f};
        REQUIRE(scene.open(device, context, world, party, options));
        const auto entering = [&] {
            return scene.spawning() || scene.animator(3)->entering() ||
                   scene.animator(1)->entering();
        };
        for (s32 tick = 0; tick < 480 && entering(); ++tick) {
            REQUIRE(scene.update(kStep, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
        REQUIRE(scene.beginRecordedAudio(kEpoch));
    }
    PlayerRuntime& player(s32 seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native ending fixture
        return *const_cast<PlayerRuntime*>(scene.runtime(seat));
    }
    std::vector<SceneSoundEvents::Ledger::Event> endings() {
        const auto step = scene.recordStep(kEpoch, 1, kStep, {});
        REQUIRE(step.outcome == PlayOutcome::Running);
        REQUIRE(step.audio);
        std::vector<SceneSoundEvents::Ledger::Event> result;
        for (const auto& event : step.audio->finite.events) {
            if (event.id.emitter.nameSpace == 45) {
                result.push_back(event);
            }
        }
        return result;
    }
};

TEST_CASE("powerup ending sounds use real player seats and independent semantic cues",
          "[native-sound-identity][play-scene-audio][assets]") {
    auto fixture = std::make_unique<PartySounds>();
    for (const s32 seat : {3, 1}) {
        fixture->player(seat).wornSpecial =
            powerup::kPojo | powerup::kGrowth | powerup::kLevitation;
    }
    PlayScene::LocalSnapshot before;
    REQUIRE(fixture->scene.saveLocal(before));
    const auto original = fixture->endings();
    REQUIRE(original.size() == 6);
    for (const s32 seat : {3, 1}) {
        for (const u32 cue : {1U, 2U, 3U}) {
            CHECK(std::ranges::count_if(original, [&](const auto& event) {
                      return event.id.emitter ==
                                 FrameEventEmitter{45, 0, 1, static_cast<u8>(seat)} &&
                             event.id.cue == cue && event.id.occurrence == 0;
                  }) == 1);
        }
    }
    REQUIRE(fixture->scene.restoreLocal(before));
    fixture->player(3).wornSpecial = 0;
    fixture->player(1).wornSpecial = powerup::kLevitation;
    const auto corrected = fixture->endings();
    REQUIRE(corrected.size() == 1);
    const auto expected = std::ranges::find_if(original, [](const auto& event) {
        return event.id.emitter.seat == 1 && event.id.cue == 3;
    });
    REQUIRE(expected != original.end());
    CHECK(corrected[0] == *expected);
}
} // namespace
