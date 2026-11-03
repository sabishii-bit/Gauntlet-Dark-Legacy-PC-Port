#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/PlayScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Castle Treasury coffin audio obeys the native party restriction and active ears",
          "[game][screens][ambient-population][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG3/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("G3");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    constexpr usize kCoffin = 320;
    REQUIRE(world.layout().itemInstances().size() > kCoffin);
    const auto& coffin = world.layout().itemInstances()[kCoffin];
    REQUIRE(coffin.name == "S_COFFINVOX");
    REQUIRE(coffin.minPlayers == 3);
    const GameConfig config;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.sounds = &sounds;
    context.unpackedRoot = root;
    PlayOptions options;
    options.welcome = false;
    options.position = coffin.position;
    const s32 joined = GENERATE(1, 2, 3, 4);
    CAPTURE(joined);
    std::vector<PartyMember> party;
    for (s32 id = 0; id < joined; ++id) {
        PartyMember member{id, CharacterSave{}};
        // Waiting players count for ItemVisible but cannot make a sound audible.
        member.fallen = id != 0;
        party.push_back(member);
    }
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const AmbientEmitter* emitter = nullptr;
    for (usize i = 0; i < scene.ambience().size(); ++i) {
        if (scene.ambience().emitter(i).instance == static_cast<s32>(kCoffin)) {
            emitter = &scene.ambience().emitter(i);
        }
    }
    REQUIRE(emitter);
    CHECK(emitter->minPlayers == 3);
    scene.update(1.0 / 60, {});
    REQUIRE((emitter->loudness > 0) == (joined >= 3));
    CHECK(sounds.isPlaying(emitter->handle) == (joined >= 3));
    const auto voice = emitter->handle;
    scene.harm(0, 100000, HurtKind::Burn);
    REQUIRE(scene.runtime(0)->life == PlayerLife::Dying);
    scene.update(1.0 / 60, {});
    CHECK(emitter->loudness == 0);
    CHECK(emitter->handle == kNoSound);
    CHECK_FALSE(sounds.isPlaying(voice));
    scene.close();
}

TEST_CASE("pausing a native moving crossing silences its loop until gameplay resumes",
          "[game][screens][pause-motion-audio][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const GameConfig config;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.sounds = &sounds;
    context.unpackedRoot = root;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    for (s32 frame = 0; frame < 600 && scene.spawning(); ++frame) {
        scene.update(1.0 / 60, {});
    }
    REQUIRE_FALSE(scene.spawning());
    const LevelTrigger* crossing = nullptr;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.target >= 0 &&
            world.layout().objects()[static_cast<usize>(trigger.target)].name ==
                "L1CROSSING_EASY") {
            crossing = &trigger;
            break;
        }
    }
    REQUIRE(crossing);
    const std::array visitors{TriggerVisitor{.position = crossing->spot}};
    world.updateTriggers(1.0f / 60, visitors);
    scene.update(1.0 / 60, {});
    const auto first = scene.fieldSound();
    REQUIRE(sounds.isPlaying(first));
    scene.pauseGameplaySounds();
    CHECK_FALSE(sounds.isPlaying(first));
    CHECK(scene.fieldSound() == kNoSound);
    scene.pauseGameplaySounds();
    CHECK(scene.fieldSound() == kNoSound);
    scene.update(1.0 / 60, {});
    const auto resumed = scene.fieldSound();
    REQUIRE(sounds.isPlaying(resumed));
    CHECK(resumed != first);
    for (s32 frame = 0; frame < 1200 && sounds.isPlaying(resumed); ++frame) {
        scene.update(1.0 / 60, {});
    }
    CHECK_FALSE(sounds.isPlaying(resumed));
    scene.pauseGameplaySounds();
    scene.close();
    CHECK(scene.fieldSound() == kNoSound);
    scene.update(1.0 / 60, {});
    CHECK(scene.fieldSound() == kNoSound);
}

} // namespace
