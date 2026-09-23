#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/world/AmbientSounds.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** A bank with one looping fire and a level placing it at the origin, four units wide. */
struct Fixture {
    std::filesystem::path bank;
    std::filesystem::path level;

    explicit Fixture(std::string_view name) {
        bank = test::scratchDirectory(std::string(name) + "-bank");
        std::filesystem::create_directories(bank / "samples");
        const std::array<std::int16_t, 4> kCrackle{8192, -8192, 8192, -8192};
        writeFile(bank / "samples/000.wav", formats::encodeWav(kCrackle, 48000, 1));
        writeTextFile(bank / "sounds.json", R"({
  "bank": "TEST",
  "sounds": [
    {"index": 0, "name": "S_SFIREL", "id": 0, "duration": -1.0, "volume": 127, "duck": 0,
     "priority": 0, "sequence": [{"sample": 0, "loopStart": true, "loopBack": true}]}
  ],
  "samples": [
    {"index": 0, "name": "fire", "file": "samples/000.wav", "sampleRate": 48000, "frames": 4}
  ]
})");
        level = test::scratchDirectory(std::string(name) + "-level");
        writeTextFile(level / "world.json", R"({
  "objects": [
    {"name": "FLOOR", "position": [0, 0, 0], "next": -1, "child": -1}
  ],
  "locators": [],
  "itemInfos": [
    {"type": 13, "subtype": 0, "name": ""},
    {"type": 1, "subtype": 15, "name": "GEMORANGE"}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "name": "S_SFIREL", "position": [0, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 128, 64, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "name": "S_NOWHERE", "position": [50, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 128, 64, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 1, "position": [5, 0, 5], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}
  ]
})");
    }
};

TEST_CASE("loudness holds within the radius and fades to nothing half a radius out",
          "[game][world][ambience]") {
    REQUIRE(AmbientSounds::loudness(0.0f, 4.0f) == 1.0f);
    REQUIRE(AmbientSounds::loudness(4.0f, 4.0f) == 1.0f);
    REQUIRE(AmbientSounds::loudness(5.0f, 4.0f) == Approx(0.5f));
    REQUIRE(AmbientSounds::loudness(6.0f, 4.0f) == 0.0f);
    REQUIRE(AmbientSounds::loudness(9.0f, 4.0f) == 0.0f);
    REQUIRE(AmbientSounds::loudness(3.0f, 0.0f) == 1.0f); // no radius: heard anywhere
    // Pan follows where the spot lies along the ear's right hand, flat on the ground.
    AmbientEar ear;
    ear.position = Vec3{0.0f, 0.0f, 0.0f};
    ear.right = Vec3{1.0f, 0.0f, 0.0f};
    REQUIRE(AmbientSounds::panOf(Vec3{3.0f, 5.0f, 0.0f}, ear) == Approx(1.0f));
    REQUIRE(AmbientSounds::panOf(Vec3{-2.0f, 0.0f, 0.0f}, ear) == Approx(-1.0f));
    REQUIRE(AmbientSounds::panOf(Vec3{0.0f, 0.0f, 7.0f}, ear) == Approx(0.0f));
    REQUIRE(AmbientSounds::panOf(Vec3{1.0f, 0.0f, 1.0f}, ear) == Approx(0.7071f).margin(1e-3f));
    REQUIRE(AmbientSounds::panOf(ear.position, ear) == 0.0f);
}

TEST_CASE("a level's sound items loop while a listener is near and stop when none is",
          "[game][world][ambience]") {
    const Fixture f("ambient-sounds");
    SoundSet bank;
    REQUIRE(bank.load(f.bank));
    WorldLayout layout;
    REQUIRE(layout.load(f.level));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    AmbientSounds ambience;
    const std::array<SoundSet*, 2> banks{nullptr, &bank};
    REQUIRE(ambience.bind(layout, banks));
    // The fire binds; the sound nobody holds and the gem are left out.
    REQUIRE(ambience.size() == 1);
    REQUIRE(ambience.emitter(0).instance == 0);
    REQUIRE(ambience.emitter(0).radius == 4.0f);
    REQUIRE(ambience.emitter(0).bank == &bank);
    REQUIRE(ambience.playingCount() == 0);

    AmbientEar ear;
    ear.position = Vec3{0.0f, 0.0f, -10.0f};
    // Far off, silence; inside the radius, the loop starts at the level's volume.
    const std::array<Vec3, 1> far{Vec3{0.0f, 0.0f, 20.0f}};
    ambience.update(player, far, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 0);
    REQUIRE(player.voiceCount() == 0);
    const std::array<Vec3, 2> party{Vec3{30.0f, 0.0f, 0.0f}, Vec3{3.0f, 0.0f, 0.0f}};
    ambience.update(player, party, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 1);
    REQUIRE(ambience.emitter(0).loudness == 1.0f);
    REQUIRE(player.isPlaying(ambience.emitter(0).handle));
    REQUIRE(player.voiceCount() == 1);
    // Halfway out it plays on, quieter; the handle is kept rather than restarted.
    const SoundHandle handle = ambience.emitter(0).handle;
    const std::array<Vec3, 1> edge{Vec3{5.0f, 0.0f, 0.0f}};
    ambience.update(player, edge, ear, 1.0f);
    REQUIRE(ambience.emitter(0).handle == handle);
    REQUIRE(ambience.emitter(0).loudness == Approx(0.5f));
    REQUIRE(player.voiceCount() == 1);
    // Beyond one and a half radii it stops; with nobody about too.
    const std::array<Vec3, 1> gone{Vec3{7.0f, 0.0f, 0.0f}};
    ambience.update(player, gone, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 0);
    REQUIRE_FALSE(player.isPlaying(handle));
    ambience.update(player, party, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 1);
    ambience.update(player, {}, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 0);
    // Stopping and clearing leave nothing behind.
    ambience.update(player, party, ear, 1.0f);
    ambience.stop(player);
    REQUIRE(ambience.playingCount() == 0);
    REQUIRE(ambience.size() == 1);
    ambience.clear();
    REQUIRE(ambience.size() == 0);
    // Without a bank that holds anything, there is nothing to bind.
    SoundSet empty;
    const std::array<SoundSet*, 1> none{&empty};
    REQUIRE_FALSE(ambience.bind(layout, none));
}

TEST_CASE("the tower's ambience stands at the realms' portals and its braziers",
          "[game][world][ambience][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("audio/TOWAMB/sounds.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELL1/world.json");
    SoundSet ambient;
    REQUIRE(ambient.load(root / "audio/TOWAMB"));
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    AmbientSounds ambience;
    const std::array<SoundSet*, 1> banks{&ambient};
    REQUIRE(ambience.bind(layout, banks));
    REQUIRE(ambience.size() == 53);
    std::size_t drums = 0;
    std::size_t fires = 0;
    for (std::size_t i = 0; i < ambience.size(); ++i) {
        const AmbientEmitter& emitter = ambience.emitter(i);
        const std::string& name = ambient.entry(emitter.sound).name;
        drums += name == "S_SDRUMSL" ? 1 : 0;
        fires += name == "S_SFIREL" ? 1 : 0;
        if (name == "S_SDRUMSL") {
            REQUIRE(emitter.radius == 25.0f);
        }
    }
    REQUIRE(drums == 1);
    REQUIRE(fires == 43);
}

} // namespace
