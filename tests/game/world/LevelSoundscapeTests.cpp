#include <array>
#include <bit>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/world/LevelSoundscape.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Tiny looping banks keep timing and ownership tests independent of retail assets. */
void writeBank(const std::filesystem::path& root, std::string_view bank,
               std::initializer_list<std::string_view> names, s16 sample = 8192,
               bool broken = false) {
    const auto directory = root / "audio" / bank;
    std::filesystem::create_directories(directory);
    const std::array<s16, 4> pcm{sample, sample, sample, sample};
    writeFile(directory / "sample.wav", formats::encodeWav(pcm, 48000, 1));
    std::string sounds;
    usize index = 0;
    for (const auto name : names) {
        if (index != 0) {
            sounds += ',';
        }
        sounds += std::format(R"({{"index":{},"name":"{}","id":0,"duration":-1,
            "volume":127,"duck":0,"priority":0,
            "sequence":[{{"sample":0,"loopStart":true,"loopBack":true}}]}})",
                              index++, name);
    }
    writeTextFile(directory / "sounds.json",
                  std::format(R"({{"sounds":[{}],"samples":[{{"index":0,"name":"tone",
                  "file":"{}","sampleRate":48000,"frames":4}}]}})",
                              sounds, broken ? "missing.wav" : "sample.wav"));
}

TEST_CASE("level sound lookup preserves bank precedence and broken first matches",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-lookup");
    writeBank(root, "LEVEL", {"SHARED"});
    writeBank(root, "COMMON", {"SHARED", "COMMON_FIRST"}, -8192);
    writeBank(root, "TOWAMB", {"SHARED", "COMMON_FIRST", "AMBIENT_ONLY"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info);
    std::array<f32, 128> output{};
    SECTION("level overrides common and ambient") {
        REQUIRE(soundscape.playNamed("SHARED") != kNoSound);
        mixer.mix(output);
        REQUIRE(output.back() > 0.0f);
    }
    SECTION("common overrides ambient") {
        REQUIRE(soundscape.playNamed("COMMON_FIRST") != kNoSound);
        mixer.mix(output);
        REQUIRE(output.back() < 0.0f);
    }
    SECTION("ambient fallback and missing names") {
        REQUIRE(soundscape.playNamed("AMBIENT_ONLY") != kNoSound);
        REQUIRE(soundscape.playNamed("MISSING") == kNoSound);
        REQUIRE(soundscape.playNamed("") == kNoSound);
    }
    SECTION("a corrupt first bank does not fall through") {
        writeBank(root, "LEVEL", {"SHARED"}, 8192, true);
        soundscape.open(root, &player, &info);
        REQUIRE(soundscape.playNamed("SHARED") == kNoSound);
        REQUIRE(player.voiceCount() == 0);
    }
    soundscape.close();
}

TEST_CASE("level narration selects its bank and queues after the character name",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-narration");
    writeBank(root, "VOICE1", {"PRIMARY", "BOTH"});
    writeBank(root, "VOICE2", {"SECONDARY", "BOTH"}, -8192);
    writeBank(root, "CHARACTER", {"NAME"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    SECTION("the primary bank wins duplicate names") {
        REQUIRE(soundscape.narrate("BOTH") != kNoSound);
        std::array<f32, 128> output{};
        mixer.mix(output);
        REQUIRE(output.back() > 0.0f);
    }
    SECTION("primary-only cues do not search the secondary bank") {
        REQUIRE(soundscape.narrate("SECONDARY", LevelSoundscape::Narrator::Primary) == kNoSound);
        REQUIRE(soundscape.narrate("PRIMARY", LevelSoundscape::Narrator::Primary) != kNoSound);
        REQUIRE(soundscape.narrate("SECONDARY") != kNoSound);
        REQUIRE(soundscape.narrate("MISSING") == kNoSound);
    }
    SECTION("teardown cancels queued narration before releasing its bank") {
        const SoundHandle first = soundscape.narrate("PRIMARY");
        const SoundHandle waiting =
            soundscape.narrate("SECONDARY", LevelSoundscape::Narrator::Either, first);
        REQUIRE(player.isPlaying(waiting));
        REQUIRE(player.voiceCount() == 1);
        soundscape.close();
        REQUIRE_FALSE(player.isPlaying(first));
        REQUIRE_FALSE(player.isPlaying(waiting));
        std::array<f32, 512> output{};
        mixer.mix(output);
        player.update();
        REQUIRE(player.voiceCount() == 0);
    }
    SECTION("the follow-up waits until the name ends") {
        SoundSet character;
        REQUIRE(character.load(root / "audio/CHARACTER"));
        REQUIRE(soundscape.playFrom(character, "PRIMARY") == kNoSound);
        const SoundHandle name = soundscape.playFrom(character, "NAME");
        REQUIRE(name != kNoSound);
        const SoundHandle line =
            soundscape.narrate("SECONDARY", LevelSoundscape::Narrator::Either, name);
        REQUIRE(line != kNoSound);
        REQUIRE(player.isPlaying(line));
        REQUIRE(player.voiceCount() == 1);
        soundscape.stop(name);
        player.update();
        REQUIRE(player.isPlaying(line));
        // The old voice can remain in its fade-out, but the narrator now has a stream.
        REQUIRE(mixer.streamCount() == 2);
    }
    soundscape.close();
}

TEST_CASE("scroll voices replace each other without stopping unrelated sounds",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-scroll");
    writeBank(root, "COMMON", {"LINE", "EFFECT"});
    writeBank(root, "EXTERNAL", {"UNRELATED"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    SoundSet external;
    REQUIRE(external.load(root / "audio/EXTERNAL"));
    const SoundHandle unrelated = player.play(external.sequence(0));
    const SoundHandle effect = soundscape.playNamed("EFFECT");
    soundscape.speakOverScroll("LINE");
    const SoundHandle first = soundscape.voice();
    REQUIRE(player.isPlaying(first));
    soundscape.speakOverScroll("LINE");
    REQUIRE_FALSE(player.isPlaying(first));
    const SoundHandle second = soundscape.voice();
    REQUIRE(second != first);
    REQUIRE(player.isPlaying(second));
    soundscape.close();
    REQUIRE_FALSE(player.isPlaying(second));
    REQUIRE_FALSE(player.isPlaying(effect));
    REQUIRE(player.isPlaying(unrelated));
    REQUIRE(soundscape.voice() == kNoSound);
    soundscape.close();
}

TEST_CASE("opening sounds stop by target and play the settled cue", "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-opening");
    writeBank(root, "TOWAMB", {"S_ELVMETL", "S_ELVMETSTPL"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    soundscape.opening(TriggerOpening{.target = 1, .atOnce = true, .sound = 0});
    soundscape.opening(TriggerOpening{.target = 1, .sound = -1});
    soundscape.opening(TriggerOpening{.target = 1, .sound = 4});
    REQUIRE(soundscape.fieldSound() == kNoSound);
    REQUIRE(player.voiceCount() == 0);
    soundscape.opening(TriggerOpening{.target = 1, .sound = 0});
    const SoundHandle first = soundscape.fieldSound();
    soundscape.opening(TriggerOpening{.target = 1, .sound = 0});
    const SoundHandle repeated = soundscape.fieldSound();
    soundscape.opening(TriggerOpening{.target = 2, .sound = 0});
    const SoundHandle other = soundscape.fieldSound();
    REQUIRE(player.isPlaying(first));
    REQUIRE(player.isPlaying(repeated));
    REQUIRE(player.isPlaying(other));
    soundscape.settled(TriggerOpening{.target = 1, .sound = 0});
    REQUIRE_FALSE(player.isPlaying(first));
    REQUIRE_FALSE(player.isPlaying(repeated));
    REQUIRE(player.isPlaying(other));
    REQUIRE(soundscape.fieldSound() == other);
    REQUIRE(player.voiceCount() == 4); // three moving voices (two fading) plus the settled cue
    soundscape.stopCues();
    REQUIRE_FALSE(player.isPlaying(other));
    REQUIRE(soundscape.fieldSound() == kNoSound);
    const usize voices = player.voiceCount();
    soundscape.settled(TriggerOpening{.target = 3, .atOnce = true, .sound = 0});
    REQUIRE(player.voiceCount() == voices + 1); // a settled cue needs no preceding loop
    soundscape.close();
}

TEST_CASE("reopening a level clears bank and common sound identities",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-reopen");
    writeBank(root, "LEVEL", {"LEVEL_ONLY"});
    writeBank(root, "COMMON", {"S_STEPROCK1", "S_STEPROCK2", "S_PICKUPMAGIC"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info);
    const SoundHandle level = soundscape.playNamed("LEVEL_ONLY");
    REQUIRE(level != kNoSound);
    soundscape.playFootstep(false);
    soundscape.playFootstep(true);
    soundscape.playPickup();
    REQUIRE(player.voiceCount() == 4);
    soundscape.open(test::scratchDirectory("soundscape-empty"), &player, nullptr);
    REQUIRE_FALSE(player.isPlaying(level));
    REQUIRE(soundscape.playNamed("LEVEL_ONLY") == kNoSound);
    REQUIRE_NOTHROW(soundscape.playPickup());
    soundscape.playFootstep(false);
    soundscape.playFootstep(true);
    REQUIRE(player.voiceCount() == 4);
    std::array<f32, 512> output{};
    mixer.mix(output); // drain stopped voices, then exercise updates after banks were freed
    player.update();
    REQUIRE(player.voiceCount() == 0);
    soundscape.close();
}

TEST_CASE("level ambience outlives early cue teardown but not close or rebind",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-ambience");
    writeBank(root, "TOWAMB", {"FIRE"});
    writeBank(root, "LEVEL", {"FIRE"}, -8192);
    writeTextFile(root / "world.json", R"({"objects":[
        {"name":"FLOOR","position":[0,0,0],"next":-1,"child":-1}],"locators":[],
        "itemInfos":[{"type":13,"subtype":0,"name":""}],
        "itemInstances":[{"info":0,"minPlayers":1,"name":"FIRE","position":[0,0,0],
        "rotation":[0,0,0],"params":[0,0,128,64,0,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info);
    soundscape.bindAmbience(layout);
    REQUIRE(soundscape.ambience().size() == 1);
    const std::array<Vec3, 1> listeners{Vec3{0.0f}};
    soundscape.updateAmbience(listeners, AmbientEar{}, 1.0f);
    const SoundHandle first = soundscape.ambience().emitter(0).handle;
    REQUIRE(player.isPlaying(first));
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0.0f); // ambient items prefer TOWAMB, unlike named effects
    soundscape.stopCues();
    REQUIRE(player.isPlaying(first));
    soundscape.bindAmbience(layout);
    REQUIRE_FALSE(player.isPlaying(first));
    soundscape.updateAmbience(listeners, AmbientEar{}, 1.0f);
    const SoundHandle second = soundscape.ambience().emitter(0).handle;
    REQUIRE(player.isPlaying(second));
    soundscape.close();
    REQUIRE_FALSE(player.isPlaying(second));
    REQUIRE(soundscape.ambience().size() == 0);
}

TEST_CASE("level soundscape is silent without an output and tolerates missing music",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-silent");
    writeBank(root, "COMMON", {"LINE", "S_PICKUPMAGIC"});
    const AssetLocator assets(root);
    const LevelAudioInfo info{.bank = {}, .stream = "missing"};
    LevelSoundscape soundscape;
    soundscape.open(root, nullptr, &info);
    REQUIRE(soundscape.playNamed("LINE") == kNoSound);
    REQUIRE(soundscape.narrate("LINE") == kNoSound);
    SoundSet bank;
    REQUIRE(bank.load(root / "audio/COMMON"));
    REQUIRE(soundscape.playFrom(bank, "LINE") == kNoSound);
    soundscape.playPickup();
    soundscape.playFootstep(false);
    soundscape.speakOverScroll("LINE");
    soundscape.startMusic(&assets, 1.0f);
    REQUIRE(soundscape.voice() == kNoSound);
    REQUIRE(soundscape.music() == kNoSound);
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    soundscape.open(root, &player, &info);
    soundscape.startMusic(nullptr, 1.0f);
    soundscape.startMusic(&assets, 1.0f);
    REQUIRE(soundscape.music() == kNoSound);
    soundscape.close();
}

TEST_CASE("level music loops in its own category and stops on replacement and teardown",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-music");
    std::filesystem::create_directories(root / "STREAMS");
    // A single mono DSP frame with zero predictors and positive residuals.
    test::ByteWriter stream;
    stream.putFourcc("dhSS");
    for (const u32 value : {24U, 32U, 48000U, 1U, 8U, 0xFFFFFFFFU, 0U}) {
        stream.putU32(std::byteswap(value));
    }
    stream.putFourcc("dbSS").putU32(std::byteswap(8U));
    std::array<u8, 96> channel{};
    channel[3] = 14;
    const std::array<u8, 8> frame{0, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    stream.putBytes(channel).putBytes(frame);
    writeFile(root / "STREAMS/test.ads", stream.bytes());
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const AssetLocator assets(root);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = {}, .stream = "test"};
    soundscape.open(root, &player, &info);
    soundscape.startMusic(&assets, 0.5f);
    const SoundHandle first = soundscape.music();
    REQUIRE(player.isPlaying(first));
    // 256 stereo frames also let the mixer's five-millisecond gain ramp settle.
    std::array<f32, 512> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0.0f); // beyond the single frame: the stream loops
    player.setCategoryVolume(SoundCategory::Music, 0.0f);
    mixer.mix(output);
    REQUIRE(output.back() == 0.0f);
    soundscape.startMusic(&assets, 1.0f);
    const SoundHandle second = soundscape.music();
    REQUIRE(second != first);
    REQUIRE_FALSE(player.isPlaying(first));
    REQUIRE(player.isPlaying(second));
    soundscape.stopCues();
    REQUIRE(soundscape.music() == kNoSound);
    REQUIRE_FALSE(player.isPlaying(second));
    soundscape.startMusic(&assets, 1.0f);
    const SoundHandle third = soundscape.music();
    REQUIRE(player.isPlaying(third));
    soundscape.close();
    REQUIRE_FALSE(player.isPlaying(third));
    REQUIRE(soundscape.music() == kNoSound);
}

} // namespace
