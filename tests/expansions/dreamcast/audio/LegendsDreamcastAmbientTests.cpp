#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastAmbient.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::ItemInstanceRecord emitter(std::string name, Vec3 position = Vec3{0}, f32 radius = 20,
                                    u16 flags = 0) {
    formats::ItemInstanceRecord result;
    result.info = 0;
    result.minPlayers = 1;
    result.name = std::move(name);
    result.position = position;
    const auto bits = std::bit_cast<u32>(radius);
    for (usize i = 0; i < 4; ++i) {
        result.params[i] = static_cast<u8>(bits >> (i * 8));
    }
    result.params[8] = static_cast<u8>(flags);
    result.params[9] = static_cast<u8>(flags >> 8);
    return result;
}

formats::LegendsDreamcastScene markers() {
    formats::LegendsDreamcastScene scene;
    formats::ItemInfoRecord info;
    info.type = 13;
    scene.itemInfos.push_back(info);
    return scene;
}

std::optional<u32> token(std::string_view name) {
    if (name == "S_ORGANIST") {
        return 0x2b001b;
    }
    if (name == "S_THNDERSTRM") {
        return 0x2b0020;
    }
    return {};
}

struct TempleAmbientFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;
    std::filesystem::path root;

    TempleAmbientFixture()
        : root(test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native") {
        const auto directory = root / "levels/levele1";
        if (!std::filesystem::is_regular_file(directory / "worlds.rom") ||
            !std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
            SKIP("Dreamcast extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.rom");
        auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
        map.textures =
            formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
        REQUIRE_FALSE(level->cameraAttention());
    }
};

f64 mixTicks(AudioMixer& mixer, SoundPlayer& output, usize ticks) {
    std::array<f32, 1600> buffer{};
    f64 energy = 0;
    for (usize tick = 0; tick < ticks; ++tick) {
        output.update();
        mixer.mix(buffer);
        REQUIRE(std::ranges::all_of(buffer, [](f32 sample) { return std::isfinite(sample); }));
        for (const f32 sample : buffer) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    output.update();
    return energy;
}
} // namespace

TEST_CASE("Dreamcast ambient uses the native radius ramp and stronger music duck",
          "[dreamcast-ambient]") {
    auto scene = markers();
    scene.itemInstances.push_back(emitter("S_organist", Vec3{0}, 20, 1));
    LegendsDreamcastAmbient ambient;
    REQUIRE(ambient.reset(scene, 1, token));
    REQUIRE(ambient.emitters().size() == 1);
    for (const f32 distance : {0.f, 10.f, 20.f}) {
        const auto request = ambient.request(Vec3{distance, 0, 0});
        REQUIRE(request);
        CHECK(request->loudness == 1);
        CHECK(request->volume == Catch::Approx(224.f / 255.f));
        REQUIRE(request->musicScale);
        CHECK(*request->musicScale == Catch::Approx(.2f));
    }
    // Distance is three-dimensional and is supplied as camera attention.
    const auto edge = ambient.request(Vec3{0, 25, 0});
    REQUIRE(edge);
    CHECK(edge->loudness == .5f);
    CHECK(edge->volume == Catch::Approx(112.f / 255.f));
    CHECK(edge->musicScale == .5f);
    CHECK_FALSE(ambient.request(Vec3{30, 0, 0}));
    CHECK_FALSE(ambient.request(Vec3{100, 0, 0}));
    CHECK(LegendsDreamcastAmbient::kMusicDuckHold == .5f);
}

TEST_CASE("Dreamcast ambient selects one nearest source and follows native moving parents",
          "[dreamcast-ambient]") {
    auto scene = markers();
    scene.itemInstances.push_back(emitter("S_organist", Vec3{-10, 0, 0}, 20, 1));
    scene.itemInstances.push_back(emitter("S_thnderstrm", Vec3{10, 0, 0}));
    LegendsDreamcastAmbient ambient;
    REQUIRE(ambient.reset(scene, 1, token));
    REQUIRE(ambient.request(Vec3{0}));
    CHECK(ambient.request(Vec3{0})->nativeIndex == 0); // stable equal-distance tie
    REQUIRE(ambient.request(Vec3{1, 0, 0}));
    CHECK(ambient.request(Vec3{1, 0, 0})->nativeIndex == 1);
    CHECK_FALSE(ambient.request(Vec3{1, 0, 0})->musicScale);
    const auto moved = ambient.request(Vec3{0}, [](u32 index) -> std::optional<Vec3> {
        return index == 0 ? Vec3{100, 0, 0} : Vec3{0};
    });
    REQUIRE(moved);
    CHECK(moved->nativeIndex == 1);
    CHECK(moved->position == Vec3{0});
    CHECK_FALSE(moved->musicScale);
    // Queries never retain a stale animated position when the resolver changes.
    CHECK(ambient.request(Vec3{0})->nativeIndex == 0);
}

TEST_CASE(
    "Dreamcast ambient respects party admission and excludes music selectors or missing banks",
    "[dreamcast-ambient]") {
    auto scene = markers();
    auto marker = emitter("S_organist");
    marker.minPlayers = 2;
    scene.itemInstances.push_back(marker);
    auto music = emitter("S_thnderstrm");
    music.params[4] = 1;
    scene.itemInstances.push_back(music);
    scene.itemInstances.push_back(emitter("NOT_INSTALLED"));
    scene.itemInstances.push_back(
        emitter("S_organist", Vec3{0}, std::numeric_limits<f32>::quiet_NaN()));
    auto wrong = emitter("S_organist");
    wrong.info = 99;
    scene.itemInstances.push_back(wrong);
    LegendsDreamcastAmbient ambient;
    CHECK_FALSE(ambient.reset(scene, 1, token));
    CHECK_FALSE(ambient.request(Vec3{0}));
    REQUIRE(ambient.reset(scene, 2, token));
    CHECK(ambient.emitters().size() == 1);
    CHECK_FALSE(ambient.request(Vec3{std::numeric_limits<f32>::infinity(), 0, 0}));
    CHECK_FALSE(ambient.reset(scene, 2, {}));
    CHECK(ambient.emitters().empty());
    CHECK_FALSE(ambient.reset(scene, 0, token));
}

TEST_CASE("Dreamcast small-radius sounds remain global without changing ordinary attenuation",
          "[dreamcast-ambient]") {
    for (const f32 radius : {0.f, 1.f, 2.f}) {
        auto scene = markers();
        scene.itemInstances.push_back(emitter("S_organist", Vec3{0}, radius));
        LegendsDreamcastAmbient ambient;
        REQUIRE(ambient.reset(scene, 1, token));
        const auto request = ambient.request(Vec3{10000});
        REQUIRE(request);
        CHECK(request->loudness == 1);
        CHECK_FALSE(request->musicScale);
    }
}

TEST_CASE("Native Temple organist and storm markers resolve and produce audible Dreamcast samples",
          "[dreamcast-ambient][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    const auto world = root / "levels/levele1/worlds.rom";
    if (!std::filesystem::is_regular_file(world) ||
        !std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
        SKIP("Dreamcast extraction is not installed");
    }
    LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    const auto scene = formats::LegendsDreamcastScene::parse(readFile(world));
    LegendsDreamcastAmbient ambient;
    REQUIRE(
        ambient.reset(scene, 1, [&](std::string_view name) { return sounds.namedToken(name); }));
    REQUIRE(ambient.emitters().size() == 2);
    CHECK(ambient.emitters()[0].nativeIndex == 204);
    CHECK(ambient.emitters()[0].token == 0x2b001b);
    CHECK(ambient.emitters()[1].nativeIndex == 205);
    CHECK(ambient.emitters()[1].token == 0x2b0020);
    for (const auto& source : ambient.emitters()) {
        CAPTURE(source.nativeIndex, source.token);
        CHECK(source.radius == 20);
        CHECK(source.flags == 1);
        const auto request = ambient.request(source.position);
        REQUIRE(request);
        CHECK(request->nativeIndex == source.nativeIndex);
        REQUIRE(request->musicScale);
        CHECK(*request->musicScale == Catch::Approx(.2f));
        AudioMixer mixer{48000};
        SoundPlayer output{mixer};
        const auto handle = sounds.play(output, request->token, request->volume);
        REQUIRE(handle != kNoSound);
        std::array<f32, 1600> buffer{};
        f64 energy = 0;
        for (usize tick = 0; tick < 180; ++tick) {
            output.update();
            mixer.mix(buffer);
            for (const f32 sample : buffer) {
                REQUIRE(std::isfinite(sample));
                energy += static_cast<f64>(sample) * sample;
            }
        }
        CHECK(energy > .001);
        output.stop(handle);
    }
    CHECK_FALSE(ambient.request(Vec3{10000}));
}

TEST_CASE("Dreamcast level starts one native proximity voice and stops it outside the zone or on "
          "teardown",
          "[dreamcast-ambient][dreamcast-ambient-playback][assets]") {
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    TempleAmbientFixture fixture;
    Vec3 position{33, -.0625f, -120.25f};
    SECTION("Temple organist") {}
    SECTION("Temple thunderstorm") {
        position = Vec3{0, 0, 94.25f};
    }
    fixture.level->playSounds(output, position);
    REQUIRE(output.voiceCount() == 1);
    const auto duck = fixture.level->ambientMusicDuck();
    REQUIRE(duck);
    CHECK(duck->seconds == .5f);
    CHECK(duck->scale == Catch::Approx(.2f));
    for (usize tick = 0; tick < 12; ++tick) {
        fixture.level->playSounds(output, position);
    }
    CHECK(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->playSounds(output, position + Vec3{0, 1000, 0});
    CHECK_FALSE(fixture.level->ambientMusicDuck());
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->playSounds(output, position);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->stopSounds();
    CHECK_FALSE(fixture.level->ambientMusicDuck());
    fixture.level.reset();
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
}

TEST_CASE("Dreamcast organist resumes the same native PCM cursor after an effects pause",
          "[dreamcast-ambient][dreamcast-ambient-playback][assets]") {
    AudioMixer mixer{48000};
    AudioMixer referenceMixer{48000};
    SoundPlayer output{mixer};
    SoundPlayer reference{referenceMixer};
    TempleAmbientFixture fixture;
    LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(fixture.root));
    REQUIRE(sounds.play(reference, 0x2b001b, 224.f / 255.f) != kNoSound);
    const Vec3 organist{33, -.0625f, -120.25f};
    fixture.level->playSounds(output, organist);
    REQUIRE(output.voiceCount() == 1);
    std::array<f32, 1600> actual{};
    std::array<f32, 1600> expected{};
    const auto compare = [&] {
        output.update();
        reference.update();
        mixer.mix(actual);
        referenceMixer.mix(expected);
        REQUIRE(actual == expected);
    };
    for (usize tick = 0; tick < 12; ++tick) {
        compare();
    }
    output.setCategoryPaused(SoundCategory::Effects, true);
    fixture.level->pauseSounds();
    for (usize tick = 0; tick < 90; ++tick) {
        output.update();
        mixer.mix(actual);
        CHECK(std::ranges::all_of(actual, [](f32 sample) { return sample == 0; }));
        REQUIRE(output.voiceCount() == 1);
    }
    output.setCategoryPaused(SoundCategory::Effects, false);
    fixture.level->playSounds(output, organist);
    for (usize tick = 0; tick < 60; ++tick) {
        compare();
    }
    fixture.level->stopSounds();
    reference.stopAll();
}

TEST_CASE("Dreamcast collapsing scenery retains its native realm sound table",
          "[dreamcast-ambient][dreamcast-collapse]") {
    constexpr std::array<u32, 9> kExpected{0,        0x23003b, 0x210001, 0x240029, 0x250001,
                                           0x2c0000, 0x2d001e, 0x22002a, 0};
    for (usize realm = 0; realm < kExpected.size(); ++realm) {
        CHECK(dreamcastCollapseSound(static_cast<s32>(realm)) == kExpected[realm]);
        CHECK(dreamcastCollapseSound(static_cast<s32>(realm), true) ==
              (realm == 6 ? 0x2e0000 : kExpected[realm]));
    }
    CHECK(dreamcastCollapseSound(-1) == 0);
    CHECK(dreamcastCollapseSound(9) == 0);
}
