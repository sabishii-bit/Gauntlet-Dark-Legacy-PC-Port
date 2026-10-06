#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/players/CharacterSave.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayScene.h"
#include "game/world/LevelWorld.h"

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
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.sounds = &sounds;
    context.tower = &world;
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

} // namespace
