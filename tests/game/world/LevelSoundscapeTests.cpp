#include <array>
#include <bit>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/MusicAreas.h"

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

/** A bank of one-second lines that play once, so the narrator's queue can be timed. */
void writeSecondBank(const std::filesystem::path& root, std::string_view bank,
                     std::initializer_list<std::string_view> names) {
    const auto directory = root / "audio" / bank;
    std::filesystem::create_directories(directory);
    const std::vector<s16> pcm(48000, 4096);
    writeFile(directory / "second.wav", formats::encodeWav(pcm, 48000, 1));
    std::string sounds;
    usize index = 0;
    for (const auto name : names) {
        if (index != 0) {
            sounds += ',';
        }
        sounds += std::format(R"({{"index":{},"name":"{}","id":0,"duration":1,
            "volume":127,"duck":0,"priority":0,
            "sequence":[{{"sample":0,"loopStart":false,"loopBack":false}}]}})",
                              index++, name);
    }
    writeTextFile(directory / "sounds.json",
                  std::format(R"({{"sounds":[{}],"samples":[{{"index":0,"name":"line",
                  "file":"second.wav","sampleRate":48000,"frames":48000}}]}})",
                              sounds));
}

TEST_CASE("a narrator line the tower's ambience keeps is queued from there",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-ambient-line");
    writeSecondBank(root, "VOICE1", {"LINE"});
    writeSecondBank(root, "TOWAMB", {"S_WAITINGL"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    REQUIRE(soundscape.queueNarration("S_WAITINGL", LevelSoundscape::Narrator::Primary) ==
            kNoSound);
    REQUIRE(soundscape.queueNarration("S_WAITINGL") != kNoSound);
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(1.0));
    soundscape.close();
}

TEST_CASE("character barks have a separate bounded queue and the retail gain",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-barks");
    writeSecondBank(root, "VOICE1", {"LINE"});
    writeSecondBank(root, "CHARACTER", {"BARK"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    REQUIRE(soundscape.bark(character, "BARK") != kNoSound);
    std::array<f32, 512> samples{};
    mixer.mix(samples);
    CHECK(samples.back() == Catch::Approx(0.125f * LevelSoundscape::kBarkVolume));
    soundscape.holdNarration(true);
    REQUIRE(soundscape.bark(character, "BARK", LevelSoundscape::kPainVolume) != kNoSound);
    CHECK(soundscape.bark(character, "BARK") == kNoSound);
    soundscape.holdNarration(false);
    REQUIRE(soundscape.queueNarration("LINE") != kNoSound);
    CHECK(player.voiceCount() == 2);
    CHECK(soundscape.narrationBacklog() == Catch::Approx(1));
    CHECK(soundscape.barkBacklog() == Catch::Approx(2));
    soundscape.close();
    CHECK(soundscape.barkBacklog() == 0);
}

TEST_CASE("runestone hints use the primary narrator, gain and bounded queue",
          "[game][world][soundscape][rune-meter]") {
    const auto root = test::scratchDirectory("soundscape-runestone");
    writeSecondBank(root, "VOICE1", {"S_UGETCLOSER", "S_RUNENEAR", "LINE"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    SECTION("hints play at 224 and queue behind each other") {
        REQUIRE(soundscape.announceRune(false, Vec3{0, 0, 10}, {}) != kNoSound);
        std::array<f32, 512> samples{};
        mixer.mix(samples);
        CHECK(samples.back() == Catch::Approx(0.125f * 224.0f / 255.0f));
        REQUIRE(soundscape.announceRune(true, Vec3{0, 0, 10}, {}) != kNoSound);
        CHECK(soundscape.narrationBacklog() == Catch::Approx(2));
    }
    SECTION("Sumner holds both hints") {
        soundscape.holdNarration(true);
        CHECK(soundscape.announceRune(false, Vec3{0}, {}) == kNoSound);
        CHECK(soundscape.announceRune(true, Vec3{0}, {}) == kNoSound);
    }
    SECTION("more than three seconds waiting refuses a hint") {
        for (s32 i = 0; i < 4; ++i) {
            REQUIRE(soundscape.queueNarration("LINE") != kNoSound);
        }
        CHECK(soundscape.announceRune(false, Vec3{0}, {}) == kNoSound);
    }
    soundscape.close();
}

TEST_CASE("footstep variants and entrance play at their authored volumes",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-footsteps");
    writeBank(root, "COMMON", {"S_STEPSTAIR2", "S_ENTRANCE"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    CHECK(LevelSoundscape::footingOf(8, 0) == Footing::Stair);
    CHECK(LevelSoundscape::footingOf(8, 0x10000) == Footing::Metal);
    CHECK(LevelSoundscape::footingOf(8, 0x10000, true) == Footing::Water);
    CHECK(LevelSoundscape::footingOf(0x10000, 0) == Footing::Rock);
    SECTION("steps attenuate and alternate") {
        soundscape.playFootstep(false, Footing::Stair);
        CHECK(player.voiceCount() == 0);
        soundscape.playFootstep(true, Footing::Stair, 70);
        CHECK(player.voiceCount() == 0);
        soundscape.playFootstep(true, Footing::Stair, 45);
        std::array<f32, 512> samples{};
        mixer.mix(samples);
        CHECK(samples.back() == Catch::Approx(0.25f * LevelSoundscape::kStepVolume * 0.5f));
    }
    SECTION("entrance has its own gain") {
        soundscape.playEntrance();
        std::array<f32, 512> samples{};
        mixer.mix(samples);
        CHECK(samples.back() == Catch::Approx(0.25f * LevelSoundscape::kEntranceVolume));
    }
    soundscape.close();
}

TEST_CASE("exit flame and hourglass loops keep one voice and stop independently",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-attached-loops");
    writeBank(root, "COMMON", {"S_EXITFLAME", "S_HOURGLASS"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    for (s32 i = 0; i < 20; ++i) {
        soundscape.updateExitFlame(Vec3{1, 0, 0}, {});
        soundscape.updateHourglass(Vec3{-1, 0, 0}, {});
    }
    CHECK(player.voiceCount() == 2);
    CHECK(soundscape.exitFlameOn());
    CHECK(soundscape.hourglassOn());
    soundscape.updateExitFlame(std::nullopt, {});
    CHECK_FALSE(soundscape.exitFlameOn());
    CHECK(soundscape.hourglassOn());
    soundscape.stopCues();
    CHECK_FALSE(soundscape.hourglassOn());
    soundscape.close();
}

TEST_CASE("landing title chooses its level lesson once and obeys narration backlog",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-title-lesson");
    writeSecondBank(root, "VOICE1", {"S_SHOTSSTUN", "S_GRAB"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    soundscape.announceTitle(0);
    CHECK(soundscape.narrationBacklog() == 0);
    soundscape.announceTitle(5);
    CHECK(soundscape.narrationBacklog() == Catch::Approx(1));
    soundscape.announceTitle(4);
    CHECK(soundscape.narrationBacklog() == Catch::Approx(2));
    soundscape.announceTitle(4);
    CHECK(soundscape.narrationBacklog() == Catch::Approx(2));
    soundscape.close();
}

TEST_CASE("bridges use direction cues and traps sound at each motion edge",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-motion");
    writeBank(root, "LEVEL", {"S_BRIDOPA", "S_BRIDCLA", "S_TRAPA"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info, 'A');
    soundscape.opening({.atOnce = true, .subtype = 20});
    CHECK(player.voiceCount() == 0);
    soundscape.opening({.subtype = 20});
    CHECK(player.voiceCount() == 1);
    soundscape.settled({.subtype = 20});
    CHECK(player.voiceCount() == 1);
    soundscape.opening({.subtype = 22, .closed = true});
    CHECK(player.voiceCount() == 2);
    soundscape.opening({.sound = 10});
    soundscape.settled({.sound = 10});
    CHECK(player.voiceCount() == 4);
    soundscape.close();
    soundscape.open(root, &player, &info, 'A', true);
    soundscape.opening({.subtype = 20});
    soundscape.opening({.sound = 10});
    std::array<f32, 512> samples{};
    mixer.mix(samples);
    player.update();
    CHECK(player.voiceCount() == 0);
}

TEST_CASE("the narrator's queue plays lines in turn and turns away what would wait too long",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-queue");
    writeSecondBank(root, "VOICE1", {"LINE", "OTHER", "S_POJO2"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    REQUIRE(soundscape.narrationRoom(0.5f));
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    const SoundHandle name = soundscape.queueNarrationFrom(character, "NAME");
    REQUIRE(name != kNoSound);
    const SoundHandle line = soundscape.queueNarration("LINE");
    REQUIRE(line != kNoSound);
    REQUIRE(player.voiceCount() == 1); // the line waits for the name
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(2.0));
    // Two seconds of it to come: an announcement willing to wait half of one is turned away,
    // one willing to wait four, or for ever, is let in.
    REQUIRE_FALSE(soundscape.narrationRoom(0.5f));
    REQUIRE(soundscape.narrationRoom(4.0f));
    REQUIRE(soundscape.narrationRoom(LevelSoundscape::kAlwaysRoom));
    soundscape.updateNarration(1.6f);
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(0.4));
    REQUIRE(soundscape.narrationRoom(0.5f));
    soundscape.updateNarration(1.0f);
    REQUIRE(soundscape.narrationBacklog() == 0.0);
    // No more than sixteen wait.
    for (usize i = 0; i < LevelSoundscape::kMostNarration; ++i) {
        REQUIRE(soundscape.queueNarration(i % 2 == 0 ? "LINE" : "OTHER") != kNoSound);
    }
    REQUIRE_FALSE(soundscape.narrationRoom(LevelSoundscape::kAlwaysRoom));
    REQUIRE(soundscape.queueNarration("LINE") == kNoSound);
    REQUIRE(soundscape.queueNarration("MISSING") == kNoSound);
    // Lines stopped from elsewhere no longer hold anything up.
    player.stopAll();
    REQUIRE(soundscape.narrationBacklog() == 0.0);
    REQUIRE(soundscape.narrationRoom(0.5f));
    REQUIRE(soundscape.queueNarration("LINE") != kNoSound);
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(1.0));
    // Leaving the level lets it all go.
    soundscape.close();
    REQUIRE(soundscape.narrationBacklog() == 0.0);
}

TEST_CASE("an announcement by name is let in whole or not at all, and the wizard silences it",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-announce");
    writeSecondBank(root, "VOICE1", {"LINE", "OTHER", "S_POJO2"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    const std::array lines{std::string_view{"LINE"}, std::string_view{"OTHER"}};
    REQUIRE(soundscape.announce(character, "NAME", false, lines, 0.5f));
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(3.0)); // name and two lines
    // Too much to wait behind: none of it goes in.
    REQUIRE_FALSE(soundscape.announce(character, "NAME", false, lines, 0.5f));
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(3.0));
    // Carrying Pojo, his name is said instead of the character's.
    soundscape.updateNarration(3.0f);
    REQUIRE(soundscape.announce(character, "MISSING", true, std::span{lines}.first(1), 0.5f));
    REQUIRE(soundscape.narrationBacklog() == Catch::Approx(2.0));
    // While the wizard has the floor, even a line that would wait for ever is kept out.
    soundscape.updateNarration(2.0f);
    soundscape.holdNarration(true);
    REQUIRE_FALSE(soundscape.narrationRoom(LevelSoundscape::kAlwaysRoom));
    REQUIRE(soundscape.queueNarration("LINE") == kNoSound);
    soundscape.holdNarration(false);
    REQUIRE(soundscape.queueNarration("LINE") != kNoSound);
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

TEST_CASE("promotion speeches queue behind names and the legend uses the shared bank",
          "[game][world][soundscape][promotion]") {
    const auto root = test::scratchDirectory("soundscape-promotions");
    writeBank(root, "VOICE1", {"NAME", "S_EXP99ALL"});
    writeBank(root, "WIZTOWER", {"S_EXP30WAR"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    const auto name = soundscape.narrate("NAME");
    const auto speech = soundscape.playPromotion("S_EXP30WAR", name);
    const auto legend = soundscape.playPromotion("S_EXP99ALL", speech);
    REQUIRE(name != kNoSound);
    REQUIRE(speech != kNoSound);
    REQUIRE(legend != kNoSound);
    REQUIRE(player.isPlaying(speech));
    REQUIRE(player.isPlaying(legend));
    REQUIRE(player.voiceCount() == 1);
    REQUIRE(soundscape.playPromotion("MISSING", name) == name);
    soundscape.close();
    REQUIRE_FALSE(player.isPlaying(name));
    REQUIRE_FALSE(player.isPlaying(speech));
    REQUIRE_FALSE(player.isPlaying(legend));
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

TEST_CASE("serpent wake audio requires nearby attention and attenuates at the nearest player",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-serpent");
    writeBank(root, "LEVEL", {"S_SERPENT"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info);
    const AmbientEar ear{Vec3{0, 0, -45}, Vec3{1, 0, 0}};
    CHECK(soundscape.playSerpent(Vec3{0}, Vec3{40, 0, 0}, 0, ear) == kNoSound);
    CHECK(soundscape.playSerpent(Vec3{0}, Vec3{0}, 70, ear) == kNoSound);
    CHECK(player.voiceCount() == 0);
    CHECK(soundscape.playSerpent(Vec3{0}, Vec3{39, 0, 0}, 45, ear) != kNoSound);
    std::array<f32, 1024> output{};
    mixer.mix(output);
    CHECK(output.back() == Catch::Approx(0.25f * LevelSoundscape::kMotionVolume * 0.5f));
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

TEST_CASE("world one-shots attenuate from players and pan from the active camera",
          "[soundscape][world-destruction]") {
    const auto root = test::scratchDirectory("soundscape-world-explosion");
    writeBank(root, "LEVEL", {"S_MINECAREXPLO"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "LEVEL", .stream = {}};
    soundscape.open(root, &player, &info);
    soundscape.updateAmbience({}, {Vec3{0}, Vec3{1, 0, 0}}, 1);
    CHECK(soundscape.playAt("S_MINECAREXPLO", Vec3{10, 0, 0}, 70, 127.0f / 255) == kNoSound);
    REQUIRE(soundscape.playAt("S_MINECAREXPLO", Vec3{10, 0, 0}, 45, 127.0f / 255) != kNoSound);
    std::array<f32, 1024> output{};
    mixer.mix(output);
    CHECK(output[output.size() - 2] < output.back());
    CHECK(output.back() > 0);
    soundscape.close();
}

TEST_CASE("Temple heavy doors use the realm ICE slot rather than Tower sounds",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-temple-door");
    writeBank(root, "TOWAMB", {"S_ELVSTONEL", "S_ELVMETL"}, -8192);
    writeBank(root, "CATHEDRAL", {"S_ELVICEE", "S_ELVICESTPE", "S_ELVSTONEE"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "CATHEDRAL", .stream = {}};
    soundscape.open(root, &player, &info, 'E');
    soundscape.opening(TriggerOpening{.target = 1, .sound = 3});
    const auto creak = soundscape.fieldSound();
    REQUIRE(creak != kNoSound);
    std::array<f32, 128> output{};
    mixer.mix(output);
    CHECK(output.back() > 0);
    soundscape.settled(TriggerOpening{.target = 1, .sound = 3});
    CHECK_FALSE(player.isPlaying(creak));
    CHECK(player.voiceCount() == 2);
    soundscape.opening(TriggerOpening{.target = 2, .sound = 4});
    CHECK(soundscape.fieldSound() != kNoSound);
    const auto voices = player.voiceCount();
    soundscape.opening(TriggerOpening{.target = 3, .sound = 0});
    CHECK(player.voiceCount() == voices); // Missing realm sound is not a Tower fallback.
    soundscape.close();
}

TEST_CASE("boss elevators and shared rock rotators use retail name variants",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-boss-opening");
    writeBank(root, "BOSS", {"S_ELVSTONEEB", "S_ELVSTONESTPEB", "S_ROCKROTATE", "S_ROCKSTOP"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    const LevelAudioInfo info{.bank = "BOSS", .stream = {}};
    soundscape.open(root, &player, &info, 'E', true);
    for (const s32 slot : {4, 5}) {
        soundscape.opening(TriggerOpening{.target = slot, .sound = slot});
        const auto moving = soundscape.fieldSound();
        REQUIRE(moving != kNoSound);
        const auto count = player.voiceCount();
        soundscape.settled(TriggerOpening{.target = slot, .sound = slot});
        CHECK_FALSE(player.isPlaying(moving));
        CHECK(player.voiceCount() == count + 1);
    }
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
    soundscape.suspend();
    CHECK_FALSE(player.isPlaying(second));
    CHECK(soundscape.ambience().size() == 1);
    soundscape.updateAmbience(listeners, AmbientEar{}, 1.0f);
    const SoundHandle resumed = soundscape.ambience().emitter(0).handle;
    CHECK(player.isPlaying(resumed));
    soundscape.close();
    CHECK_FALSE(player.isPlaying(resumed));
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

TEST_CASE("Wraith music resolves its numbered ADS parts and produces audio",
          "[game][world][soundscape][assets][unpacked][wraith]") {
    const auto disc = test::assetOrSkip("STREAMS/DREAM5_1.ads").parent_path().parent_path();
    test::assetOrSkip("STREAMS/DREAM5_2.ads");
    const auto manifest = test::unpackedOrSkip("wdata/DREAM.json");
    WorldData world;
    REQUIRE(world.load(manifest));
    const auto* level = world.level("J5");
    REQUIRE(level != nullptr);
    const auto* audio = world.audio(level->audioIndex);
    REQUIRE(audio != nullptr);
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(manifest.parent_path().parent_path(), &player, audio);
    const AssetLocator assets(disc);
    soundscape.startMusic(&assets, 1);
    REQUIRE(player.isPlaying(soundscape.music()));
    std::array<f32, 2048> output{};
    bool audible = false;
    for (s32 i = 0; i < 100; ++i) {
        player.update();
        mixer.mix(output);
        for (const f32 sample : output) {
            audible |= sample > 0.001f || sample < -0.001f;
        }
    }
    REQUIRE(audible);
    soundscape.close();
}

/** A stream of a single mono DSP frame with zero predictors and positive residuals. */
std::vector<u8> dspStream() {
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
    return stream.bytes();
}

TEST_CASE("level music loops in its own category and stops on replacement and teardown",
          "[game][world][soundscape]") {
    const auto root = test::scratchDirectory("soundscape-music");
    std::filesystem::create_directories(root / "STREAMS");
    writeFile(root / "STREAMS/test.ads", dspStream());
    LevelAudioInfo info{.bank = {}, .stream = "test"};
    SECTION("single stream") {}
    SECTION("multipart stream without an unsuffixed file") {
        writeFile(root / "STREAMS/parts_1.ads", dspStream());
        writeFile(root / "STREAMS/parts_2.ads", dspStream());
        info.stream = "parts";
        info.parts[0] = 2;
    }
    SECTION("first area and its numbered parts") {
        writeFile(root / "STREAMS/areaa_1.ads", dspStream());
        writeFile(root / "STREAMS/areaa_2.ads", dspStream());
        info.stream = "area";
        info.areas = 2;
        info.parts[0] = 2;
    }
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const AssetLocator assets(root);
    LevelSoundscape soundscape;
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

/** A level of four areas under the stem "area": one stream, two parts, one, and one missing. */
struct AreaFixture {
    std::filesystem::path root;
    LevelAudioInfo info{.bank = {}, .stream = "area"};
    AudioMixer mixer{48000};
    SoundPlayer player{mixer};
    AssetLocator assets;
    LevelSoundscape soundscape;

    explicit AreaFixture(std::string_view name) : root(test::scratchDirectory(name)), assets(root) {
        std::filesystem::create_directories(root / "STREAMS");
        // The fourth area's stream is not written: asked for, it has nothing to play.
        for (const std::string_view file : {"areaa", "areab_1", "areab_2", "areac"}) {
            writeFile(root / "STREAMS" / std::format("{}.ads", file), dspStream());
        }
        info.areas = 4;
        info.parts[0] = 1;
        info.parts[1] = 2;
        info.parts[2] = 1;
        info.parts[3] = 1;
        soundscape.open(root, &player, &info);
        soundscape.startMusic(&assets, 0.5f);
    }
    ~AreaFixture() { soundscape.close(); }
    AreaFixture(const AreaFixture&) = delete;
    AreaFixture& operator=(const AreaFixture&) = delete;
    AreaFixture(AreaFixture&&) = delete;
    AreaFixture& operator=(AreaFixture&&) = delete;

    /** So many of the music's frames of game time. */
    void frames(s32 count) {
        for (s32 i = 0; i < count; ++i) {
            soundscape.updateMusic(1.0f / LevelSoundscape::kMusicRate);
        }
    }
    /** Plays the queued sound out and feeds the stream again, so its parts move on. */
    void drain() {
        std::array<f32, 4096> output{};
        for (s32 i = 0; i < 40; ++i) {
            mixer.mix(output);
        }
        player.update();
    }
};

TEST_CASE("music areas go over at once, after a fade or at the part's end",
          "[game][world][soundscape][music-areas]") {
    AreaFixture f("soundscape-areas");
    const SoundHandle first = f.soundscape.music();
    REQUIRE(f.player.isPlaying(first));
    REQUIRE(f.soundscape.musicArea() == 0);
    REQUIRE(f.soundscape.musicRequest() == 0);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    SECTION("at once") {
        f.soundscape.selectMusicArea(2, MusicSwitch::AtOnce);
        REQUIRE(f.soundscape.musicRequest() == 2);
        REQUIRE(f.soundscape.musicArea() == 0);
        f.frames(1);
        const SoundHandle second = f.soundscape.music();
        REQUIRE(second != first);
        REQUIRE(f.player.isPlaying(second));
        REQUIRE_FALSE(f.player.isPlaying(first));
        REQUIRE(f.soundscape.musicArea() == 2);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
        // The same area asked for again changes nothing.
        f.soundscape.selectMusicArea(2, MusicSwitch::AtOnce);
        f.frames(5);
        REQUIRE(f.soundscape.music() == second);
    }
    SECTION("after a fade") {
        f.soundscape.selectMusicArea(1, MusicSwitch::Faded);
        // Three a frame down to three: eighty-four frames of the old stream fading.
        f.frames(1);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel - 3);
        REQUIRE(f.soundscape.music() == first);
        f.frames(83);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFadedLevel);
        REQUIRE(f.soundscape.music() == first);
        REQUIRE(f.soundscape.musicArea() == 0);
        // Then the new stream from its first part, rising eight a frame back to full.
        f.frames(1);
        const SoundHandle second = f.soundscape.music();
        REQUIRE(second != first);
        REQUIRE_FALSE(f.player.isPlaying(first));
        REQUIRE(f.player.isPlaying(second));
        REQUIRE(f.soundscape.musicArea() == 1);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFadedLevel + 8);
        f.frames(30);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFadedLevel + 8 * 31);
        f.frames(1);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
        f.frames(10);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
        REQUIRE(f.soundscape.music() == second);
    }
    SECTION("at the part's end") {
        // Asked for and withdrawn before the part ends, the switch waits and is dropped.
        f.soundscape.selectMusicArea(2, MusicSwitch::AtPartEnd);
        f.frames(1);
        REQUIRE(f.soundscape.musicFollowing() == 2);
        REQUIRE(f.soundscape.music() == first);
        f.soundscape.selectMusicArea(0, MusicSwitch::AtPartEnd);
        f.frames(1);
        REQUIRE(f.soundscape.musicFollowing() == -1);
        // Asked for and left standing, it goes through as the part playing runs out, on the
        // same voice at full level.
        f.soundscape.selectMusicArea(1, MusicSwitch::AtPartEnd);
        f.frames(1);
        REQUIRE(f.soundscape.musicFollowing() == 1);
        REQUIRE(f.soundscape.musicArea() == 0);
        f.drain();
        f.frames(1);
        REQUIRE(f.soundscape.musicArea() == 1);
        REQUIRE(f.soundscape.musicFollowing() == -1);
        REQUIRE(f.soundscape.music() == first);
        REQUIRE(f.player.isPlaying(first));
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    }
    SECTION("an area without a stream is silent until another is asked for") {
        f.soundscape.selectMusicArea(7, MusicSwitch::AtOnce);
        f.frames(1);
        REQUIRE(f.soundscape.music() == kNoSound);
        REQUIRE_FALSE(f.player.isPlaying(first));
        REQUIRE(f.soundscape.musicArea() == 3); // clamped to the last area
        f.frames(5);
        REQUIRE(f.soundscape.music() == kNoSound);
        f.soundscape.selectMusicArea(0, MusicSwitch::AtOnce);
        f.frames(1);
        REQUIRE(f.player.isPlaying(f.soundscape.music()));
        REQUIRE(f.soundscape.musicArea() == 0);
    }
    SECTION("stopping the cues lets the music go until it is started again") {
        f.soundscape.stopCues();
        REQUIRE(f.soundscape.musicArea() == -1);
        f.soundscape.selectMusicArea(1, MusicSwitch::AtOnce);
        f.frames(3);
        REQUIRE(f.soundscape.music() == kNoSound);
        f.soundscape.startMusic(&f.assets, 1.0f);
        REQUIRE(f.soundscape.musicArea() == 0);
        REQUIRE(f.soundscape.musicRequest() == 0);
        REQUIRE(f.player.isPlaying(f.soundscape.music()));
    }
}

TEST_CASE("a rune sting ducks music temporarily without losing area selection",
          "[game][world][soundscape][music-areas]") {
    AreaFixture f("soundscape-rune-duck");
    f.soundscape.duckMusic(2, 0.5f);
    f.frames(30);
    CHECK(f.soundscape.musicArea() == 0);
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    f.frames(90);
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    f.soundscape.duckMusic(10, 0.5f);
    f.soundscape.selectMusicArea(1, MusicSwitch::Faded);
    f.frames(100);
    CHECK(f.soundscape.musicArea() == 1);
}

TEST_CASE("the zones ask the music for their areas and a boss waking asks for the second",
          "[game][world][soundscape][music-areas]") {
    AreaFixture f("soundscape-zones");
    const auto level = test::scratchDirectory("soundscape-zones-level");
    // One zone of ten units at (15, 0, 0) naming the second area, cutting over at once.
    writeTextFile(level / "world.json", R"({
  "objects": [{"name": "FLOOR", "position": [0, 0, 0], "next": -1, "child": -1}],
  "locators": [],
  "itemInfos": [{"type": 13, "subtype": 0, "name": ""}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "name": "", "position": [15, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 32, 65, 2, 0, 0, 0, 2, 0, 0, 0]}
  ]
})");
    WorldLayout layout;
    REQUIRE(layout.load(level));
    f.soundscape.bindAmbience(layout);
    REQUIRE(f.soundscape.musicAreas().size() == 1);
    REQUIRE(f.soundscape.ambience().size() == 0);
    const SoundHandle first = f.soundscape.music();
    SECTION("a party crossing into a zone") {
        const std::array<Vec3, 1> outside{Vec3{-30.0f, 0.0f, 0.0f}};
        f.soundscape.updateMusicAreas(outside);
        f.frames(1);
        REQUIRE(f.soundscape.music() == first);
        const std::array<Vec3, 1> inside{Vec3{15.0f, 0.0f, 0.0f}};
        f.soundscape.updateMusicAreas(inside);
        REQUIRE(f.soundscape.musicRequest() == 1);
        f.frames(1);
        REQUIRE(f.soundscape.musicArea() == 1);
        const SoundHandle second = f.soundscape.music();
        REQUIRE(second != first);
        // Staying, or leaving for open ground, changes nothing more.
        f.soundscape.updateMusicAreas(inside);
        f.soundscape.updateMusicAreas(outside);
        f.frames(3);
        REQUIRE(f.soundscape.music() == second);
        REQUIRE(f.soundscape.musicArea() == 1);
    }
    SECTION("the boss waking") {
        f.soundscape.bossAwake(false);
        f.frames(1);
        REQUIRE(f.soundscape.musicRequest() == 0);
        f.soundscape.bossAwake(true);
        REQUIRE(f.soundscape.musicRequest() == 1);
        f.frames(1);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel - 3);
        f.frames(84);
        REQUIRE(f.soundscape.musicArea() == 1);
        const SoundHandle second = f.soundscape.music();
        REQUIRE(second != first);
        // Told again that it is awake, nothing more is asked.
        f.soundscape.bossAwake(true);
        f.frames(40);
        REQUIRE(f.soundscape.music() == second);
        REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    }
}

TEST_CASE("a level of one area ignores requests for others",
          "[game][world][soundscape][music-areas]") {
    const auto root = test::scratchDirectory("soundscape-one-area");
    std::filesystem::create_directories(root / "STREAMS");
    writeFile(root / "STREAMS/test.ads", dspStream());
    const LevelAudioInfo info{.bank = {}, .stream = "test"};
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const AssetLocator assets(root);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, &info);
    soundscape.startMusic(&assets, 1.0f);
    const SoundHandle first = soundscape.music();
    REQUIRE(player.isPlaying(first));
    soundscape.selectMusicArea(1, MusicSwitch::AtOnce);
    soundscape.updateMusic(1.0f / LevelSoundscape::kMusicRate);
    REQUIRE(soundscape.music() == first);
    REQUIRE(soundscape.musicRequest() == 0);
    soundscape.selectMusicArea(1, MusicSwitch::Faded);
    for (s32 i = 0; i < 100; ++i) {
        soundscape.updateMusic(1.0f / LevelSoundscape::kMusicRate);
    }
    REQUIRE(soundscape.music() == first);
    REQUIRE(soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    soundscape.close();
}

TEST_CASE("the dream's zones switch its music between eight areas",
          "[game][world][soundscape][music-areas][assets][unpacked]") {
    const auto disc = test::assetOrSkip("STREAMS/DREAM1A.ads").parent_path().parent_path();
    test::assetOrSkip("STREAMS/dream1e_1.ads");
    test::assetOrSkip("STREAMS/dream1e_2.ads");
    const auto manifest = test::unpackedOrSkip("wdata/DREAM.json");
    const auto root = manifest.parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELJ1/world.json");
    WorldData world;
    REQUIRE(world.load(manifest));
    const auto* level = world.level("J1");
    REQUIRE(level != nullptr);
    const auto* audio = world.audio(level->audioIndex);
    REQUIRE(audio != nullptr);
    REQUIRE(audio->areas == 8);
    REQUIRE(audio->parts[4] == 2);
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELJ1"));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, audio, 'J');
    soundscape.bindAmbience(layout);
    REQUIRE(soundscape.musicAreas().size() == 15);
    const AssetLocator assets(disc);
    soundscape.startMusic(&assets, 1.0f);
    const SoundHandle first = soundscape.music();
    REQUIRE(player.isPlaying(first));
    // The zone of the fifth area (the two-part dream1e) fades the music over to it.
    std::optional<Vec3> spot;
    for (usize i = 0; i < soundscape.musicAreas().size(); ++i) {
        const MusicZone& zone = soundscape.musicAreas().zone(i);
        if (zone.area == 4) {
            REQUIRE(zone.how == MusicSwitch::Faded);
            spot = zone.position;
        }
    }
    REQUIRE(spot.has_value());
    const std::array<Vec3, 1> party{*spot};
    soundscape.updateMusicAreas(party);
    REQUIRE(soundscape.musicRequest() == 4);
    for (s32 i = 0; i < 85; ++i) {
        soundscape.updateMusic(1.0f / LevelSoundscape::kMusicRate);
    }
    REQUIRE(soundscape.musicArea() == 4);
    REQUIRE(soundscape.music() != first);
    REQUIRE(player.isPlaying(soundscape.music()));
    std::array<f32, 2048> output{};
    bool audible = false;
    for (s32 i = 0; i < 100; ++i) {
        player.update();
        mixer.mix(output);
        for (const f32 sample : output) {
            audible |= sample > 0.001f || sample < -0.001f;
        }
    }
    REQUIRE(audible);
    soundscape.close();
}

} // namespace
