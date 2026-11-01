#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/effects/EnemyFeedbackEmission.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/ImportedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

class ImportedRoster final : public ImportedLevel {
public:
    std::vector<ImportedGenerator> roster;
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::span<const ImportedGenerator> importedGenerators() const override { return roster; }
};

bool mixAudible(AudioMixer& mixer, SoundPlayer& output) {
    std::array<f32, 1600> samples{};
    bool heard = false;
    for (usize i = 0; i < 60; ++i) {
        mixer.mix(samples);
        heard |= std::ranges::any_of(samples, [](f32 value) { return std::abs(value) > .00001f; });
        output.update();
    }
    output.stopAll();
    output.update();
    mixer.mix(samples);
    mixer.mix(samples);
    return heard;
}
} // namespace

TEST_CASE("Imported opponent counterparts retain audible hit death and generator feedback",
          "[legends][audio][imported-opponent-sounds][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/objects.ngc").parent_path().parent_path().parent_path();
    auto imported = std::make_shared<ImportedRoster>();
    // Every native species currently admitted by legendsN64Generator. These
    // are GDL counterparts, so use their actual WDATA voice roster and banks.
    constexpr std::array<s32, 14> kKinds{0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19};
    for (const auto kind : kKinds) {
        ImportedGenerator generator;
        generator.nativeIndex = static_cast<u32>(kind);
        generator.kind = kind;
        generator.tier = 3;
        generator.position = {0, 0, 20};
        generator.body.type = ItemInfo::kGenerator;
        generator.body.collisionType = 1;
        generator.body.radius = 2;
        generator.body.height = 6;
        generator.body.hitPoints = 100;
        imported->roster.push_back(generator);
    }
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {{{-100, 0, -100}, {0, 0, 100}, {100, 0, -100}}};
    floor.objectFlags = 4;
    WorldCollision collision;
    collision.build({floor});
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:opponent-audio-test";
    const bool loaded = world.loadImported(
        device, root, ref, WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 20, 100}),
        std::move(collision), imported);
    REQUIRE(loaded);
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape audio;
    audio.open(root, &output, nullptr);
    ItemArchive weapons;
    EffectTrees effects;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> party;
    opponents.open({device, world, weapons, effects, audio, root}, party);
    REQUIRE(opponents.levelNow());
    REQUIRE(opponents.levelNow()->enemies.size() == kKinds.size());
    REQUIRE(opponents.importedGeneratorStates().size() == kKinds.size());
    const auto roster = opponents.levelNow()->enemies;
    std::set<std::string> tested;
    for (const s32 kind : kKinds) {
        CAPTURE(kind);
        for (const s32 tier : {1, 3}) {
            for (const bool close : {false, true}) {
                for (const bool killed : {false, true}) {
                    for (const s32 hitCount : {0, 2}) {
                        EnemyFeedback feedback;
                        feedback.kind = kind;
                        feedback.tier = tier;
                        feedback.close = close;
                        feedback.killed = killed;
                        feedback.hitCount = hitCount;
                        const auto name = feedback.sound(roster);
                        CAPTURE(name);
                        REQUIRE_FALSE(name.empty());
                        REQUIRE(audio.hasNamed(name));
                        if (tested.insert(name).second) {
                            REQUIRE(audio.playNamed(name) != kNoSound);
                            CHECK(mixAudible(mixer, output));
                        }
                    }
                }
            }
        }
    }
    // Exercise the real damage dispatch, not only successful bank lookup.
    // Placed births share exactly the same feedback lane as generated ones.
    LevelOpponents::Events events;
    events.award = [](s32, s32, bool) {};
    events.levels = [] {};
    const std::array views{EnemyView{.player = 0, .position = {0, 0, -10}}};
    for (usize i = 0; i < kKinds.size(); ++i) {
        CAPTURE(kKinds[i]);
        EnemySpawn spawn;
        spawn.kind = kKinds[i];
        spawn.tier = 3;
        spawn.placed = true;
        spawn.position = {0, 0, 0};
        const auto enemy = opponents.enemies().spawn(spawn, views);
        REQUIRE(enemy);
        opponents.strikeEnemy(*enemy, 10000, 0, {0, 0, 1}, 0, party);
        opponents.settleRewards(party, events);
        CHECK(mixAudible(mixer, output));
        const auto state = opponents.importedGeneratorStates()[i];
        REQUIRE(state.pool >= 0);
        std::string generatorSound;
        REQUIRE(opponents.strikeGenerator(
            state.pool, 1, 0, party,
            [&](const GeneratorFeedbackEmission& emission) -> std::optional<u32> {
                generatorSound = emission.sound;
                return 0;
            }));
        CAPTURE(generatorSound);
        REQUIRE(audio.hasNamed(generatorSound));
        // The sorted catalog begins with TEST/T1/T2 (realm0), whose roster
        // contains these species but has no generator bank for the T prefix.
        // Native campaign WDATA instead assigns MOUNT/B1 and BATTLE/H1.
        if (kKinds[i] == 0 || kKinds[i] == 1) {
            CHECK(generatorSound == "S_GENDAMB");
        } else if (kKinds[i] == 19) {
            CHECK(generatorSound == "S_GENDAMH");
        }
        opponents.strikeGenerator(state.pool, 110, 0, party);
        CHECK(mixAudible(mixer, output));
        opponents.strikeGenerator(state.pool, 10000, 0, party);
        CHECK(mixAudible(mixer, output));
    }
    opponents.close();
    effects.clear();
    audio.close();
}
