#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/world/AmbientSounds.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** A bank with one looping fire and a level placing it at the origin, four units wide. */
struct Fixture {
    std::filesystem::path bank;
    std::filesystem::path level;

    explicit Fixture(std::string_view name) {
        bank = test::scratchDirectory(std::string(name) + "-bank") / "TEST";
        std::filesystem::create_directories(bank / "samples");
        const std::array<s16, 4> kCrackle{8192, -8192, 8192, -8192};
        const std::array<test::NativeSoundSample, 1> bankSamples{
            {{48000, {kCrackle.begin(), kCrackle.end()}}}};
        test::writeNativeSoundBank(bank, R"({
  "bank": "TEST",
  "sounds": [
    {"index": 0, "name": "S_SFIREL", "id": 0, "duration": -1.0, "volume": 127, "duck": 0,
     "priority": 0, "sequence": [{"sample": 0, "loopStart": true, "loopBack": true}]}
  ]
})",
                                   bankSamples);
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
    {"info": 0, "minPlayers": 1, "name": "S_sfirel", "position": [0, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 128, 64, 0, 0, 0, 0, 1, 0, 0, 0]},
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
    REQUIRE(AmbientSounds::loudness(100.0f, 1.0f) == 1.0f);
    REQUIRE(AmbientSounds::loudness(100.0f, 2.0f) == 1.0f);
    REQUIRE(AmbientSounds::loudness(100.0f, 2.01f) == 0.0f);
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

TEST_CASE("sound items follow their nearest animated parent through repeated activations",
          "[game][world][ambience][ambient-parent]") {
    const Fixture f("ambient-parent");
    writeTextFile(f.level / "world.json", R"({
      "objects": [
        {"name":"MAN", "position":[1,0,0]},
        {"name":"OTHER", "position":[5,0,0]},
        {"name":"STATIC", "position":[0,0,0]}],
      "animations": [
        {"object":1,"frames":2,"track":{"flags":16,"frames":[0,1],"values":[0,1]}},
        {"object":0,"frames":2,"track":{"flags":16,"frames":[0,1],"values":[0,1]}}],
      "itemInfos":[{"type":13}],
      "itemInstances":[{"info":0,"name":"S_SFIREL","position":[0,0,0],
        "params":[0,0,128,64,0,0,0,0,1,0,0,0]}]
    })");
    WorldLayout layout;
    SoundSet bank;
    REQUIRE(layout.load(f.level));
    REQUIRE(bank.load(f.bank));
    AmbientSounds ambience;
    const std::array banks{&bank};
    REQUIRE(ambience.bind(layout, banks));
    REQUIRE(ambience.emitter(0).parent == 0);
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldScene world;
    // No meshes are needed to retain and animate the sound parent's placement.
    CHECK_FALSE(world.build(layout, models, textures, device));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const std::array listeners{Vec3{20, 0, 0}};
    AmbientEar ear;
    ear.position = listeners.front();
    std::array<f32, 1600> output{};
    SoundHandle previous = kNoSound;
    for (s32 activation = 0; activation < 3; ++activation) {
        world.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{1, 0, 0}));
        ambience.update(player, listeners, ear, 1, std::nullopt, &world);
        CHECK(ambience.playingCount() == 0);
        CHECK_FALSE(player.isPlaying(previous));
        mixer.mix(output);
        mixer.mix(output);
        CHECK(std::ranges::all_of(output, [](f32 sample) { return sample == 0; }));
        world.setObjectTransform(0, glm::translate(Mat4{1}, listeners.front()));
        ambience.update(player, listeners, ear, 1, std::nullopt, &world);
        CHECK(ambience.emitter(0).position == listeners.front());
        REQUIRE(ambience.playingCount() == 1);
        REQUIRE(ambience.emitter(0).handle != previous);
        previous = ambience.emitter(0).handle;
        mixer.mix(output);
        CHECK(std::ranges::any_of(output, [](f32 sample) { return std::abs(sample) > 0.01f; }));
    }
    ambience.stop(player);
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
    REQUIRE_FALSE(ambience.musicScale().has_value());
    const std::array<Vec3, 2> party{Vec3{30.0f, 0.0f, 0.0f}, Vec3{3.0f, 0.0f, 0.0f}};
    ambience.update(player, party, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 1);
    REQUIRE(ambience.emitter(0).loudness == 1.0f);
    REQUIRE(ambience.emitter(0).flags == AmbientSounds::kDuckMusic);
    REQUIRE(ambience.musicScale() == 0.5f);
    REQUIRE(player.isPlaying(ambience.emitter(0).handle));
    REQUIRE(player.voiceCount() == 1);
    std::array<f32, 1600> output{};
    mixer.mix(output);
    REQUIRE(std::ranges::any_of(output, [](f32 sample) { return std::abs(sample) > 0.01f; }));
    // Halfway out it plays on, quieter; the handle is kept rather than restarted.
    const SoundHandle handle = ambience.emitter(0).handle;
    const std::array<Vec3, 1> edge{Vec3{5.0f, 0.0f, 0.0f}};
    ambience.update(player, edge, ear, 1.0f);
    REQUIRE(ambience.emitter(0).handle == handle);
    REQUIRE(ambience.emitter(0).loudness == Approx(0.5f));
    REQUIRE(ambience.musicScale() == 0.75f);
    REQUIRE(player.voiceCount() == 1);
    // Beyond one and a half radii it stops; with nobody about too.
    ambience.update(player, edge, ear, 1.0f, 16.0f / 255.0f);
    CHECK(ambience.emitter(0).handle == handle);
    CHECK(ambience.emitter(0).loudness == Approx(0.5f));
    const std::array<Vec3, 1> gone{Vec3{7.0f, 0.0f, 0.0f}};
    ambience.update(player, gone, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 0);
    REQUIRE_FALSE(player.isPlaying(handle));
    REQUIRE_FALSE(ambience.musicScale().has_value());
    mixer.mix(output); // drain the stop ramp
    mixer.mix(output);
    REQUIRE(std::ranges::all_of(output, [](f32 sample) { return sample == 0.0f; }));
    ambience.update(player, party, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 1);
    REQUIRE(ambience.emitter(0).handle != handle);
    mixer.mix(output);
    REQUIRE(std::ranges::any_of(output, [](f32 sample) { return std::abs(sample) > 0.01f; }));
    ambience.update(player, {}, ear, 1.0f);
    REQUIRE(ambience.playingCount() == 0);
    // Stopping and clearing leave nothing behind.
    ambience.update(player, party, ear, 1.0f);
    ambience.stop(player);
    REQUIRE(ambience.playingCount() == 0);
    REQUIRE(ambience.size() == 1);
    REQUIRE_FALSE(ambience.musicScale().has_value());
    ambience.clear();
    REQUIRE(ambience.size() == 0);
    // Without a bank that holds anything, there is nothing to bind.
    SoundSet empty;
    const std::array<SoundSet*, 1> none{&empty};
    REQUIRE_FALSE(ambience.bind(layout, none));
}

TEST_CASE("ambient population gates use joined slots and release both voice and music duck",
          "[game][world][ambience][ambient-population]") {
    const Fixture f("ambient-population");
    writeTextFile(f.level / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":13}], "itemInstances":[
        {"info":0,"minPlayers":3,"name":"S_SFIREL","position":[0,0,0],
         "params":[0,0,128,64,0,0,0,0,1,0,0,0]},
        {"info":0,"minPlayers":12,"name":"S_SFIREL","position":[0,0,0],
         "params":[0,0,128,64,0,0,0,0,1,0,0,0]}]})");
    WorldLayout layout;
    SoundSet bank;
    REQUIRE(layout.load(f.level));
    REQUIRE(bank.load(f.bank));
    AmbientSounds ambience;
    REQUIRE(ambience.bind(layout, std::array{&bank}));
    REQUIRE(ambience.size() == 2);
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const std::array listeners{Vec3{0}}; // one standing listener, not the joined population
    std::array<f32, 1600> samples{};
    SoundHandle previous = kNoSound;
    for (const s32 joined : {1, 2, 3, 4, 1, 3}) {
        CAPTURE(joined);
        ambience.setPlayerCount(joined);
        ambience.update(player, listeners, {}, 1);
        CHECK(ambience.emitter(0).loudness == (joined >= 3 ? 1 : 0));
        CHECK(ambience.emitter(1).loudness == (joined == 2 ? 1 : 0));
        CHECK(ambience.playingCount() == (joined > 1 ? 1 : 0));
        CHECK(ambience.musicScale().has_value() == (joined > 1));
        if (joined == 1) {
            CHECK_FALSE(player.isPlaying(previous));
        } else {
            previous = ambience.emitter(joined == 2 ? 1 : 0).handle;
            CHECK(player.isPlaying(previous));
        }
        mixer.mix(samples);
        mixer.mix(samples); // drain any stopped voice's fade
        CHECK(std::ranges::any_of(samples, [](f32 sample) { return std::abs(sample) > 0.01f; }) ==
              (joined > 1));
    }
    // An eligible joined party with no active ears still hears nothing.
    ambience.update(player, {}, {}, 1);
    CHECK(ambience.playingCount() == 0);
    CHECK_FALSE(ambience.musicScale().has_value());
    CHECK_FALSE(player.isPlaying(previous));
    ambience.clear();
    REQUIRE(ambience.bind(layout, std::array{&bank}));
    ambience.update(player, listeners, {}, 1);
    CHECK(ambience.playingCount() == 0); // bind resets population to one
    ambience.stop(player);
}

TEST_CASE("ambient parents bind from the initial animated pose rather than the rest position",
          "[game][world][ambience][ambient-parent]") {
    const Fixture f("ambient-posed-parent");
    writeTextFile(f.level / "world.json", R"({
      "objects":[{"name":"LIFT","position":[100,0,0]},
                 {"name":"WRONG","position":[1,0,0]}],
      "animations":[
        {"object":0,"frames":2,"track":{"flags":16,"frames":[0,1],"values":[0,1]}},
        {"object":1,"frames":2,"track":{"flags":16,"frames":[0,1],"values":[0,1]}}],
      "itemInfos":[{"type":13}],
      "itemInstances":[{"info":0,"name":"S_SFIREL","position":[0,0,0],
        "params":[0,0,128,64,0,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(f.level));
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldScene world;
    world.build(layout, models, textures, device);
    world.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{3, 0, 0}));
    world.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{50, 0, 0}));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "TEST", .stream = {}};
    // Exercise the production soundscape forwarding path, not just AmbientSounds::bind.
    std::filesystem::create_directories(f.level / "audio");
    for (const auto& entry : std::filesystem::directory_iterator(f.bank.parent_path())) {
        if (entry.is_regular_file()) {
            std::filesystem::copy_file(entry.path(), f.level / "audio" / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }
    soundscape.open(f.level, &player, &info);
    soundscape.bindAmbience(layout, &world);
    REQUIRE(soundscape.ambience().size() == 1);
    CHECK(soundscape.ambience().emitter(0).parent == 0);
    const Vec3 moved{100, 20, -10};
    world.setObjectTransform(0, glm::translate(Mat4{1}, moved));
    soundscape.updateAmbience(std::array{moved}, {}, 1, false, &world);
    CHECK(soundscape.ambience().emitter(0).position == moved);
    CHECK(soundscape.ambience().playingCount() == 1);
    soundscape.updateAmbience(std::array{Vec3{3, 0, 0}}, {}, 1, false, &world);
    CHECK(soundscape.ambience().playingCount() == 0);
    soundscape.close();
}

TEST_CASE("the Temple trigger repeatedly brings its organist sound and light into range",
          "[game][world][ambience][organist-trigger][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    REQUIRE(levels.byName("E1"));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("E1")));
    SoundSet bank;
    REQUIRE(bank.load(root / "audio/CATHEDRAL"));
    AmbientSounds ambience;
    const std::array banks{&bank};
    REQUIRE(ambience.bind(world.layout(), banks));
    usize organ = ambience.size();
    for (usize i = 0; i < ambience.size(); ++i) {
        if (bank.entry(ambience.emitter(i).sound).name == "S_ORGANIST") {
            organ = i;
        }
    }
    REQUIRE(organ < ambience.size());
    REQUIRE(ambience.emitter(organ).parent == 667);
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    std::array<f32, 3200> output{};
    const Vec3 near{30.09375f, 0.1640625f, -117.1328125f};
    const Vec3 far{0, 0, 0};
    const auto standAt = [&](const Vec3& position) {
        const std::array visitors{TriggerVisitor{.position = position, .height = 5}};
        const std::array listeners{position};
        AmbientEar ear;
        ear.position = position;
        f32 peak = 0;
        for (s32 tick = 0; tick < 120; ++tick) {
            world.update(1.0f / 30);
            world.updateTriggers(1.0f / 30, visitors);
            player.update();
            ambience.update(player, listeners, ear, 1, std::nullopt, &world.scene());
            mixer.mix(output);
            for (const f32 sample : output) {
                peak = std::max(peak, std::abs(sample));
            }
        }
        return peak;
    };
    SoundHandle previous = kNoSound;
    for (s32 visit = 0; visit < 3; ++visit) {
        standAt(far);
        CHECK_FALSE(world.triggers().opened(663));
        CHECK_FALSE(world.triggers().opened(667));
        CHECK_FALSE(player.isPlaying(previous));
        CHECK(ambience.emitter(organ).loudness == 0);
        CHECK(standAt(near) > 0.01f);
        CHECK(world.triggers().opened(663));
        CHECK(world.triggers().opened(667));
        REQUIRE(player.isPlaying(ambience.emitter(organ).handle));
        CHECK(ambience.emitter(organ).handle != previous);
        CHECK(glm::distance(ambience.emitter(organ).position,
                            Vec3{world.scene().worldTransform(667)[3]}) < 0.001f);
        previous = ambience.emitter(organ).handle;
    }
    ambience.stop(player);
}

TEST_CASE("the tower's ambience stands at the realms' portals and its braziers",
          "[game][world][ambience][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("audio/TOWAMB.vbk").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    SoundSet ambient;
    REQUIRE(ambient.load(root / "audio/TOWAMB"));
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    AmbientSounds ambience;
    const std::array<SoundSet*, 1> banks{&ambient};
    REQUIRE(ambience.bind(layout, banks));
    REQUIRE(ambience.size() == 53);
    usize drums = 0;
    usize fires = 0;
    for (usize i = 0; i < ambience.size(); ++i) {
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

TEST_CASE("authored Temple organist and Battlefield hoop sound items are audible on approach",
          "[game][world][ambience][soundscape][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("audio/TOWAMB.vbk").parent_path().parent_path();
    struct Example {
        std::string_view level;
        std::string_view bank;
        std::string_view manifest;
        std::string_view levelName;
        std::string_view cue;
        usize emitters;
        bool loop;
    };
    for (const Example& example :
         {Example{"LEVELE1", "CATHEDRAL", "TEMPLE", "E1", "S_ORGANIST", 2, true},
          Example{"LEVELH3", "BATTLE", "BATTLE", "H3", "S_SCATHEAD", 4, false}}) {
        CAPTURE(example.level);
        test::assetOrSkip("AUDIO/AUDATPS2.ROM");
        const auto world =
            test::assetOrSkip(std::string("LEVELS/") + std::string(example.level) + "/WORLDS.PS2");
        WorldLayout layout;
        REQUIRE(layout.load(world.parent_path()));
        const auto item =
            std::ranges::find(layout.itemInstances(), example.cue, &ItemInstance::name);
        REQUIRE(item != layout.itemInstances().end());
        AudioMixer mixer(48000);
        SoundPlayer player(mixer);
        LevelSoundscape sounds;
        WorldData data;
        const auto manifest =
            test::assetOrSkip(std::string("wdata/") + std::string(example.manifest) + ".WAD");
        REQUIRE(data.load(manifest));
        const LevelInfo* level = data.level(example.levelName);
        REQUIRE(level != nullptr);
        const LevelAudioInfo* info = data.audio(level->audioIndex);
        REQUIRE(info != nullptr);
        REQUIRE(info->bank == example.bank);
        sounds.open(root, &player, info);
        sounds.bindAmbience(layout);
        const AmbientSounds& ambience = sounds.ambience();
        REQUIRE(ambience.size() == example.emitters);
        usize index = ambience.size();
        for (usize i = 0; i < ambience.size(); ++i) {
            if (ambience.emitter(i).instance == item - layout.itemInstances().begin()) {
                index = i;
            }
        }
        REQUIRE(index < ambience.size());
        const AmbientEmitter& emitter = ambience.emitter(index);
        REQUIRE(emitter.bank->entry(emitter.sound).name == example.cue);
        REQUIRE(emitter.bank->sequence(emitter.sound).loops() == example.loop);
        REQUIRE((emitter.flags & AmbientSounds::kDuckMusic) != 0);
        const AmbientEar ear{.position = item->position};
        const std::array<Vec3, 1> far{item->position + Vec3{10000.0f, 0.0f, 0.0f}};
        const std::array<Vec3, 1> near{item->position};
        std::array<f32, 1600> output{}; // one sixtieth of a second, stereo
        sounds.updateAmbience(far, ear, 1.0f);
        REQUIRE(ambience.playingCount() == 0);
        mixer.mix(output);
        REQUIRE(std::ranges::all_of(output, [](f32 sample) { return sample == 0.0f; }));
        sounds.updateAmbience(near, ear, 1.0f);
        const SoundHandle first = emitter.handle;
        REQUIRE(player.isPlaying(first));
        REQUIRE(ambience.playingCount() == 1); // only this authored emitter can contribute
        f32 peak = 0.0f;
        // Cross the organist's 25.298-second loop and the basket's 6.178-second one-shot.
        const s32 frames = example.loop ? 80 * 60 : 8 * 60;
        f32 windowPeak = 0.0f;
        for (s32 frame = 0; frame < frames; ++frame) {
            player.update();
            sounds.updateAmbience(near, ear, 1.0f);
            mixer.mix(output);
            for (const f32 sample : output) {
                peak = std::max(peak, std::abs(sample));
                windowPeak = std::max(windowPeak, std::abs(sample));
            }
            if (frame % (5 * 60) == 5 * 60 - 1) {
                CAPTURE(frame);
                CHECK(windowPeak > 0.01f);
                windowPeak = 0.0f;
            }
            if (example.loop || frame < 60) {
                REQUIRE(emitter.handle == first); // approaching again must not restart it
            }
        }
        REQUIRE(peak > 0.01f);
        if (!example.loop) {
            // ProcessItems/AudioSecretProc renews a completed voice while still in range.
            REQUIRE(emitter.handle != first);
        }
        const SoundHandle playing = emitter.handle;
        sounds.updateAmbience(far, ear, 1.0f);
        REQUIRE_FALSE(player.isPlaying(playing));
        REQUIRE(ambience.playingCount() == 0);
        mixer.mix(output);
        mixer.mix(output);
        REQUIRE(std::ranges::all_of(output, [](f32 sample) { return sample == 0.0f; }));
        sounds.updateAmbience(near, ear, 1.0f);
        REQUIRE(player.isPlaying(emitter.handle));
        REQUIRE(emitter.handle != playing);
        peak = 0.0f;
        for (s32 frame = 0; frame < 60; ++frame) {
            player.update();
            sounds.updateAmbience(near, ear, 1.0f);
            mixer.mix(output);
            for (const f32 sample : output) {
                peak = std::max(peak, std::abs(sample));
            }
        }
        REQUIRE(peak > 0.01f);
        // These authored flags also ask the real level's soundtrack to make room.
        const auto disc =
            test::assetOrSkip("STREAMS/" + info->stream + ".ads").parent_path().parent_path();
        const AssetLocator assets(disc);
        sounds.startMusic(&assets, level->musicVolume);
        REQUIRE(player.isPlaying(sounds.music()));
        for (s32 frame = 0; frame < 60; ++frame) {
            sounds.updateAmbience(near, ear, level->soundVolume);
            sounds.updateMusic(1.0f / 60.0f);
        }
        REQUIRE(sounds.musicLevel() == LevelSoundscape::kFullLevel / 2);
        sounds.updateAmbience(far, ear, level->soundVolume);
        sounds.updateMusic(0.1f);
        REQUIRE(sounds.musicLevel() == LevelSoundscape::kFullLevel / 2);
        sounds.updateMusic(1.0f);
        REQUIRE(sounds.musicLevel() == LevelSoundscape::kFullLevel);
        sounds.close();
        mixer.mix(output);
        mixer.mix(output);
        REQUIRE(std::ranges::all_of(output, [](f32 sample) { return sample == 0.0f; }));
    }
}

} // namespace
