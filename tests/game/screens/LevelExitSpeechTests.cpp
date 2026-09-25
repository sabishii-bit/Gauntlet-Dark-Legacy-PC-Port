#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/screens/LevelExitSpeech.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::array<std::string_view, 8> kCues{"S_SKORNTAUNT1", "S_TOOHASTY",    "S_SKORNTAUNT2",
                                                "S_SRCHBTTR",    "S_SKORNTAUNT3", "S_QUIK2LEAVE",
                                                "S_SKORNTAUNT4", "S_FRGTLGND"};

std::filesystem::path voiceBank() {
    const auto root = test::scratchDirectory("exit-speech");
    const auto bank = root / "audio/VOICE1";
    std::filesystem::create_directories(bank);
    std::string sounds;
    std::string samples;
    for (usize i = 0; i < kCues.size(); ++i) {
        const auto file = std::format("{}.wav", i);
        const std::vector<s16> pcm(usize{48000} * 6, static_cast<s16>((i + 1) * 1024));
        writeFile(bank / file, formats::encodeWav(pcm, 48000, 1));
        if (i != 0) {
            sounds += ',';
            samples += ',';
        }
        sounds += std::format(R"({{"index":{},"name":"{}","id":{},"duration":6,
            "volume":127,"sequence":[{{"sample":{}}}]}})",
                              i, kCues[i], i, i);
        samples += std::format(R"({{"index":{},"name":"tone","file":"{}",
            "sampleRate":48000,"frames":288000}})",
                               i, file);
    }
    writeTextFile(bank / "sounds.json",
                  std::format(R"({{"sounds":[{}],"samples":[{}]}})", sounds, samples));
    return root;
}

TEST_CASE("exit reminders rotate retail rune and legend lines independently", "[exit-speech]") {
    // GUNE5D do_exit calls 800A1994 once; 8009C378/8009C3EC use tables
    // 80124340/80124330 respectively. A finite PCM fixture verifies the actual cue.
    const auto root = voiceBank();
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelExitSpeech speech;
    SoundSet expected;
    REQUIRE(expected.load(root / "audio/VOICE1"));
    const std::array<PartyMember, 1> party{{{3, {}}}};
    const auto checkCue = [&](const ExitRelics& relics, std::string_view name) {
        const auto handle = speech.begin(root, &output, relics, party);
        REQUIRE(output.isPlaying(handle));
        AudioMixer referenceMixer(48000);
        SoundPlayer reference(referenceMixer);
        const auto cue = expected.find(name);
        REQUIRE(cue.has_value());
        reference.play(expected.sequence(*cue));
        std::array<f32, 1024> actual{};
        std::array<f32, 1024> wanted{};
        mixer.mix(actual);
        referenceMixer.mix(wanted);
        CHECK(actual == wanted);
        speech.close();
        // Drain the stopped voice's five-millisecond fade before the next comparison.
        mixer.mix(actual);
        output.update();
    };
    for (usize i = 0; i < 5; ++i) {
        checkCue({.rune = 7}, kCues[i % 4]);
        checkCue({.legend = 9}, kCues[4 + i % 4]);
    }
}

TEST_CASE("exit reminder checks the whole party and only its selected classes", "[exit-speech]") {
    const auto root = voiceBank();
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelExitSpeech speech;
    std::array<PartyMember, 2> party{{{3, {}}, {1, {}}}};
    SECTION("owned by another player from an earlier visit") {
        party[1].save.progress().relics.addRune(7);
        CHECK(speech.begin(root, &output, {.rune = 7}, party) == kNoSound);
    }
    SECTION("an inactive class does not own this character's rune") {
        party[1].save.classes[1].relics.addRune(7);
        CHECK(speech.begin(root, &output, {.rune = 7}, party) != kNoSound);
    }
    SECTION("an owned legendary item still takes priority over a missed rune") {
        party[1].save.progress().relics.addLegend(9);
        CHECK(speech.begin(root, &output, {.rune = 7, .legend = 9}, party) == kNoSound);
    }
    SECTION("no remaining pickup, no party, or invalid indices") {
        CHECK(speech.begin(root, &output, {}, party) == kNoSound);
        CHECK(speech.begin(root, &output, {.rune = 7}, {}) == kNoSound);
        CHECK(speech.begin(root, &output, {.rune = -1}, party) == kNoSound);
        CHECK(speech.begin(root, &output, {.rune = 13}, party) == kNoSound);
        CHECK(speech.begin(root, &output, {.legend = 16}, party) == kNoSound);
    }
    speech.close();
}

TEST_CASE("exit speech survives scene sound teardown and finishes without holding travel",
          "[exit-speech]") {
    const auto root = voiceBank();
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelExitSpeech speech;
    LevelSoundscape sceneAudio;
    sceneAudio.open(root, &output, nullptr);
    const std::array<PartyMember, 1> party{{{0, {}}}};
    const auto voice = speech.begin(root, &output, {.rune = 0}, party);
    REQUIRE(voice != kNoSound);
    sceneAudio.close();
    std::array<f32, 1600> frame{};
    for (s32 tick = 0; tick < 400; ++tick) {
        mixer.mix(frame);
        output.update();
        if (tick == 180) {
            CHECK(output.isPlaying(voice)); // still audible after the portal's cover
            CHECK(frame.back() > 0.0f);
        }
    }
    CHECK_FALSE(output.isPlaying(voice));
    const auto next = speech.begin(root, &output, {.rune = 0}, party);
    REQUIRE(output.isPlaying(next));
    speech.close();
    CHECK_FALSE(output.isPlaying(next));
    speech.close();
}

TEST_CASE("exit speech tolerates disabled audio and unavailable clips", "[exit-speech]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelExitSpeech speech;
    const std::array<PartyMember, 1> party{{{0, {}}}};
    const auto root = test::scratchDirectory("exit-speech-missing");
    CHECK(speech.begin(root, nullptr, {.rune = 0}, party) == kNoSound);
    CHECK(speech.begin(root, &output, {.rune = 0}, party) == kNoSound);
    speech.close();
}

TEST_CASE("Fields exit uses its actual runestone and collection removes the reminder",
          "[exit-speech][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELG/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.setPlayerCount(1);
    auto remaining = ExitRelics::remaining(world.placedItems());
    REQUIRE(remaining.rune == 7); // pickup values are zero-based; WDATA says 8
    REQUIRE_FALSE(remaining.legend.has_value());
    std::optional<Vec3> position;
    for (usize i = 0; i < world.placedItems().size(); ++i) {
        const auto& item = world.placedItems().item(i);
        if (item.subtype == ItemInfo::kRunestone) {
            position = item.position;
        }
    }
    REQUIRE(position.has_value());
    const std::array<Collector, 1> collectors{{{*position, 1, 2}}};
    world.collect(device, collectors);
    CHECK_FALSE(ExitRelics::remaining(world.placedItems()).rune.has_value());
    world.clear();
}

TEST_CASE("all exit taunts decode from the original narrator bank", "[exit-speech][unpacked]") {
    const auto directory = test::unpackedOrSkip("audio/VOICE1/sounds.json").parent_path();
    SoundSet bank;
    REQUIRE(bank.load(directory));
    for (const auto name : kCues) {
        INFO(name);
        const auto index = bank.find(name);
        REQUIRE(index.has_value());
        CHECK(bank.entry(*index).duration > 0);
        CHECK_NOTHROW(bank.sequence(*index));
    }
}
} // namespace
