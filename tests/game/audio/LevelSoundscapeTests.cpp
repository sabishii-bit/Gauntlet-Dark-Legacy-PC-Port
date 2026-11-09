#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
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
#include "fixtures/NativeSoundBank.h"
#include "game/audio/LevelSoundscape.h"
#include "game/audio/MusicAreas.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Tiny banks keep timing and ownership tests independent of retail assets. */
void writeBank(const std::filesystem::path& root, std::string_view bank,
               std::initializer_list<std::string_view> names, s16 sample = 8192,
               bool broken = false, bool loops = true) {
    const auto directory = root / "audio" / bank;
    const std::array<test::NativeSoundSample, 1> samples{
        {{48000, {sample, sample, sample, sample}}}};
    std::string sounds;
    usize index = 0;
    for (const auto name : names) {
        if (index != 0) {
            sounds += ',';
        }
        sounds += std::format(R"({{"index":{},"name":"{}","id":0,"duration":-1,
            "volume":127,"duck":0,"priority":0,
            "sequence":[{{"sample":0,"loopStart":{},"loopBack":{}}}]}})",
                              index++, name, loops, loops);
    }
    test::writeNativeSoundBank(directory, std::format(R"({{"sounds":[{}]}})", sounds), samples);
    if (broken) {
        writeTextFile(root / "audio" / (std::string(bank) + ".VBK"), "invalid native bank");
    }
}

TEST_CASE("borrowed console sound banks rewind activation while retaining their loaded resources",
          "[native-console][soundscape][soundscape-local-rewind]") {
    const auto root = test::scratchDirectory("console-borrowed-sounds");
    writeBank(root, "BORROWED", {"S_CONSOLE_ONLY"});
    LevelSoundscape sounds;
    sounds.open(root, nullptr, nullptr);
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    CHECK_FALSE(sounds.hasNamed("S_CONSOLE_ONLY"));
    REQUIRE(sounds.borrowBank(root / "audio" / "BORROWED"));
    CHECK(sounds.hasNamed("S_CONSOLE_ONLY"));
    LevelSoundscape::LocalSnapshot after;
    sounds.saveLocal(after);
    REQUIRE(sounds.borrowBank(root / "audio" / "BORROWED"));
    LevelSoundscape::LocalSnapshot repeated;
    sounds.saveLocal(repeated);
    CHECK(sounds.equalLocal(after, repeated));
    REQUIRE(sounds.restoreLocal(before));
    CHECK_FALSE(sounds.hasNamed("S_CONSOLE_ONLY"));
    REQUIRE(sounds.borrowBank(root / "audio" / "BORROWED"));
    sounds.saveLocal(repeated);
    CHECK(sounds.equalLocal(after, repeated));
    CHECK(sounds.hasNamed("S_CONSOLE_ONLY"));
    REQUIRE(sounds.restoreLocal(before));
    REQUIRE(sounds.restoreLocal(after));
    CHECK(sounds.hasNamed("S_CONSOLE_ONLY"));
}

TEST_CASE("tower ceremony cues reach wizard common and tower banks and stop on close",
          "[game][tower-completion][soundscape]") {
    const auto root = test::scratchDirectory("tower-completion-sounds");
    writeBank(root, "WIZTOWER", {"S_RUNE13YES", "S_RUNEHIT"});
    writeBank(root, "COMMON", {"S_STNDGLASS"});
    writeBank(root, "TOWAMB", {"S_4KEYVOX"});
    writeBank(root, "VOICE1", {"S_EXP99ALL"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &player, nullptr);
    std::vector<SoundHandle> voices;
    for (const auto* name :
         {"S_RUNE13YES", "S_RUNEHIT", "S_STNDGLASS", "S_4KEYVOX", "S_EXP99ALL"}) {
        INFO(name);
        const auto voice = sounds.playPromotion(name);
        REQUIRE(voice != kNoSound);
        REQUIRE(player.isPlaying(voice));
        voices.push_back(voice);
    }
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0);
    REQUIRE(sounds.playPromotion("MISSING") == kNoSound);
    sounds.close();
    for (const auto voice : voices) {
        REQUIRE_FALSE(player.isPlaying(voice));
    }
}

TEST_CASE("level sound lookup preserves bank precedence and rejects corrupt banks",
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
    SECTION("a corrupt bank is not loaded and valid common audio remains available") {
        writeBank(root, "LEVEL", {"SHARED"}, 8192, true);
        soundscape.open(root, &player, &info);
        REQUIRE(soundscape.playNamed("SHARED") != kNoSound);
        mixer.mix(output);
        REQUIRE(output.back() < 0.0f);
    }
    soundscape.close();
}

TEST_CASE("crystal scroll voice uses the wizard bank and is owned by the scroll",
          "[soundscape][tower-crystals]") {
    const auto root = test::scratchDirectory("crystal-scroll-sound");
    writeBank(root, "WIZTOWER", {"S_CRYS4MNT"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &player, nullptr);
    sounds.speakOverScroll("S_CRYS4MNT");
    const auto voice = sounds.voice();
    REQUIRE(voice != kNoSound);
    REQUIRE(player.isPlaying(voice));
    std::array<f32, 128> output{};
    mixer.mix(output);
    CHECK(output.back() > 0);
    sounds.stopVoice();
    CHECK_FALSE(player.isPlaying(voice));
}

TEST_CASE("native crystal announcement is playable from the tower sound bank",
          "[tower-crystals][assets]") {
    const auto root = test::assetOrSkip("AUDIO/WIZTOWER.VBK").parent_path().parent_path();
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &player, nullptr);
    sounds.speakOverScroll("S_CRYS4MNT");
    REQUIRE(sounds.voice() != kNoSound);
    CHECK(player.isPlaying(sounds.voice()));
}

TEST_CASE("the runestone's fall and landing are playable from the tower sound bank",
          "[tower-relics][soundscape][assets]") {
    const auto root = test::assetOrSkip("AUDIO/WIZTOWER.VBK").parent_path().parent_path();
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &player, nullptr);
    std::vector<f32> output(1600);
    const auto loudness = [&](usize sixtieths) {
        f64 energy = 0;
        for (usize i = 0; i < sixtieths; ++i) {
            mixer.mix(output);
            for (const f32 sample : output) {
                energy += static_cast<f64>(sample) * sample;
            }
        }
        return std::sqrt(energy / static_cast<f64>(sixtieths * output.size()));
    };
    for (const auto* name : {"S_RUNEFALL", "S_RUNEHIT"}) {
        INFO(name);
        const auto voice = sounds.playPromotion(name);
        REQUIRE(voice != kNoSound);
        CHECK(player.isPlaying(voice));
        CHECK(loudness(24) > 0.02); // its first two fifths of a second, well above silence
        player.stop(voice);
    }
    // The landing, cued 91 ticks into the fall's rumble, must stand out over it.
    const auto fall = sounds.playPromotion("S_RUNEFALL");
    REQUIRE(fall != kNoSound);
    loudness(91);
    const f64 rumbleAlone = loudness(25);
    player.stop(fall);
    const auto again = sounds.playPromotion("S_RUNEFALL");
    REQUIRE(again != kNoSound);
    loudness(91);
    REQUIRE(sounds.playPromotion("S_RUNEHIT") != kNoSound);
    const f64 withLanding = loudness(25);
    CAPTURE(rumbleAlone, withLanding);
    CHECK(withLanding > rumbleAlone * 1.1);
}

/** A bank of one-second lines that play once, so the narrator's queue can be timed. */
void writeSecondBank(const std::filesystem::path& root, std::string_view bank,
                     std::initializer_list<std::string_view> names) {
    const auto directory = root / "audio" / bank;
    const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(48000, 4096)}}};
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
    test::writeNativeSoundBank(directory, std::format(R"({{"sounds":[{}]}})", sounds), samples);
}

TEST_CASE("speech output failures preserve decoded native timing and chained waits",
          "[soundscape][speech-timing][determinism]") {
    const auto root = test::scratchDirectory("speech-output-failure");
    writeSecondBank(root, "COMMON", {"NAMED"});
    writeSecondBank(root, "WIZTOWER", {"PROMOTION"});
    writeSecondBank(root, "VOICE1", {"NARRATION"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    writeBank(root, "LOOPING", {"LOOP"});
    SoundSet character;
    SoundSet looping;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    REQUIRE(looping.load(root / "audio/LOOPING"));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape audible;
    LevelSoundscape silent;
    audible.open(root, &output, nullptr);
    silent.open(root, nullptr, nullptr);
    const auto after = silent.speechNamed("NAMED");
    REQUIRE(after.timing.remainingSeconds == Catch::Approx(1));
    usize failures = 0;
    std::array<LevelSoundscape::Speech, 5> failed;
    {
        // Exercise an exception from the real optional output call, after its
        // source has decoded. No failure is injected into gameplay timing.
        const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play) {
                ++failures;
                throw std::runtime_error("test audio output failure");
            }
        });
        REQUIRE_NOTHROW(failed[0] = audible.speechNamed("NAMED", after));
        REQUIRE_NOTHROW(failed[1] = audible.speechPromotion("PROMOTION", after));
        REQUIRE_NOTHROW(failed[2] = audible.speechNarration("NARRATION"));
        REQUIRE_NOTHROW(failed[3] = audible.speechFrom(character, "NAME"));
        REQUIRE_NOTHROW(failed[4] = audible.speechFrom(looping, "LOOP"));
    }
    REQUIRE(failures == failed.size());
    std::array<LevelSoundscape::Speech, 5> expected{
        silent.speechNamed("NAMED", after), silent.speechPromotion("PROMOTION", after),
        silent.speechNarration("NARRATION"), silent.speechFrom(character, "NAME"),
        silent.speechFrom(looping, "LOOP")};
    for (usize index = 0; index < failed.size(); ++index) {
        CAPTURE(index);
        CHECK(failed[index].timing == expected[index].timing);
        CHECK(failed[index].timing.playing());
    }
    REQUIRE(failed[0].timing.remainingSeconds == Catch::Approx(2));
    REQUIRE(failed[4].timing.looping);
    for (s32 tick = 0; tick < 180; ++tick) {
        for (usize index = 0; index < failed.size(); ++index) {
            failed[index].timing.advance(1.0 / 60);
            expected[index].timing.advance(1.0 / 60);
            REQUIRE(failed[index].timing == expected[index].timing);
        }
    }
    for (usize index = 0; index < failed.size() - 1; ++index) {
        CHECK_FALSE(failed[index].timing.playing());
    }
    CHECK(failed.back().timing.playing());
    CHECK(audible.speechNamed("MISSING", after).timing == after.timing);
    // The observer throws after SoundPlayer has started a voice. Its output-only
    // cleanup is explicit; no partially started voice drives the logical clock.
    output.stopAll();
    audible.close();
    silent.close();
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
    // Command 192 -> 191 at the full options setting: -1.5 dB master, -3 dB center pan.
    CHECK(samples.back() == Catch::Approx(0.125f * std::pow(10.0f, -45.0f / 200.0f)));
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
        // Command 224 -> 223: -0.8 dB master and the centered stereo mix's -3 dB.
        CHECK(samples.back() == Catch::Approx(0.125f * std::pow(10.0f, -38.0f / 200.0f)));
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
        // Half of command 127 truncates to 63, then 62 after the options scale;
        // the DCS master curve gives -6.2 dB, plus -3 dB center pan.
        CHECK(samples.back() == Catch::Approx(0.25f * std::pow(10.0f, -92.0f / 200.0f)));
    }
    SECTION("entrance has its own gain") {
        soundscape.playEntrance();
        std::array<f32, 512> samples{};
        mixer.mix(samples);
        CHECK(samples.back() == Catch::Approx(0.25f * std::pow(10.0f, -38.0f / 200.0f)));
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
    soundscape.opening({.atOnce = true, .sound = 0, .subtype = 20});
    CHECK(player.voiceCount() == 0);
    soundscape.opening({.sound = -1, .subtype = 20});
    soundscape.opening({.sound = -1, .subtype = 22, .closed = true});
    CHECK(player.voiceCount() == 0); // a silent authored bridge must not dispatch its cue
    soundscape.opening({.sound = 0, .subtype = 20});
    CHECK(player.voiceCount() == 1);
    soundscape.settled({.subtype = 20});
    CHECK(player.voiceCount() == 1);
    soundscape.opening({.sound = 0, .subtype = 22, .closed = true});
    CHECK(player.voiceCount() == 2);
    soundscape.opening({.sound = 10});
    soundscape.settled({.sound = 10});
    CHECK(player.voiceCount() == 4);
    soundscape.close();
    soundscape.open(root, &player, &info, 'A', true);
    soundscape.opening({.sound = 0, .subtype = 20});
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
    // A device stop cannot change gameplay admission or the game-over wait.
    player.stopAll();
    REQUIRE(soundscape.narrationBacklog() == LevelSoundscape::kMostNarration);
    REQUIRE_FALSE(soundscape.narrationRoom(0.5f));
    // The owning level explicitly cancels the logical queue when suspended.
    soundscape.suspend();
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

TEST_CASE("failed voice output preserves the complete announcement and logical backlog",
          "[soundscape][voice-queue][speech-timing][determinism]") {
    const auto root = test::scratchDirectory("soundscape-phrase-output-failure");
    writeSecondBank(root, "VOICE1", {"LINE", "OTHER"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape failing;
    LevelSoundscape silent;
    failing.open(root, &output, nullptr);
    silent.open(root, nullptr, nullptr);
    const std::array lines{std::string_view{"LINE"}, std::string_view{"OTHER"}};
    usize failures = 0;
    {
        const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play) {
                ++failures;
                throw std::runtime_error("test queued audio output failure");
            }
        });
        REQUIRE(failing.announce(character, "NAME", false, lines, 0.5f));
        REQUIRE(silent.announce(character, "NAME", false, lines, 0.5f));
        REQUIRE(failures == 3); // The name's output failure must not skip either line.
        REQUIRE(failing.narrationBacklog() == Catch::Approx(3));
        REQUIRE(silent.narrationBacklog() == Catch::Approx(3));
        REQUIRE_FALSE(failing.announce(character, "NAME", false, lines, 0.5f));
        REQUIRE_FALSE(silent.announce(character, "NAME", false, lines, 0.5f));
        CHECK(failures == 3);
    }
    for (s32 tick = 0; tick <= 180; ++tick) {
        CAPTURE(tick);
        const auto actual = failing.narrationTiming();
        const auto expected = silent.narrationTiming();
        REQUIRE(actual.size() == expected.size());
        for (usize line = 0; line < actual.size(); ++line) {
            CHECK(actual[line] == expected[line]);
        }
        CHECK(failing.narrationBacklog() == silent.narrationBacklog());
        CHECK(failing.narrationRoom(0.5f) == silent.narrationRoom(0.5f));
        failing.updateNarration(1.0f / 60);
        silent.updateNarration(1.0f / 60);
    }
    CHECK(failing.narrationBacklog() == 0);
    CHECK(silent.narrationBacklog() == 0);
    // The injected observer may fail after an output voice starts; its lifetime
    // is independent from the logical queue and is explicitly ended here.
    output.stopAll();
    failing.close();
    silent.close();
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

TEST_CASE("paused object motion loops resume only surviving targets and leave menu audio alone",
          "[game][world][soundscape][pause-motion-audio]") {
    const auto root = test::scratchDirectory("soundscape-paused-motion");
    writeBank(root, "TOWAMB", {"S_ELVMETL", "S_ELVMETSTPL", "MENU"});
    writeBank(root, "VOICE1", {"NARRATION"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    const SoundClip musicClip{48000, 1, {0.25f, 0.25f}};
    const auto music = player.play({{{&musicClip, true, true}}}, 1, SoundCategory::Music);
    const auto menu = soundscape.playNamed("MENU");
    const auto narration = soundscape.narrate("NARRATION");
    REQUIRE(player.isPlaying(music));
    REQUIRE(player.isPlaying(menu));
    REQUIRE(player.isPlaying(narration));
    soundscape.opening({.target = 1, .sound = 0});
    const auto first = soundscape.fieldSound();
    soundscape.opening({.target = 2, .sound = 0});
    const auto second = soundscape.fieldSound();
    REQUIRE(player.isPlaying(first));
    REQUIRE(player.isPlaying(second));
    soundscape.pauseAmbience();
    CHECK_FALSE(player.isPlaying(first));
    CHECK_FALSE(player.isPlaying(second));
    CHECK(soundscape.fieldSound() == kNoSound);
    CHECK(player.isPlaying(menu));
    CHECK(player.isPlaying(narration));
    CHECK(player.isPlaying(music));
    const auto pausedVoices = player.voiceCount();
    soundscape.pauseAmbience();
    CHECK(player.voiceCount() == pausedVoices);
    SECTION("only movement that survives the pause is restarted exactly once") {
        soundscape.settled({.target = 1, .sound = 0});
        const auto beforeResume = player.voiceCount();
        soundscape.updateAmbience({}, {}, 1);
        const auto resumed = soundscape.fieldSound();
        REQUIRE(player.isPlaying(resumed));
        CHECK(resumed != second);
        CHECK(player.voiceCount() == beforeResume + 1);
        soundscape.updateAmbience({}, {}, 1);
        CHECK(soundscape.fieldSound() == resumed);
        CHECK(player.voiceCount() == beforeResume + 1);
        soundscape.settled({.target = 2, .sound = 0});
        CHECK_FALSE(player.isPlaying(resumed));
        CHECK(soundscape.fieldSound() == kNoSound);
        const auto settledVoices = player.voiceCount();
        soundscape.updateAmbience({}, {}, 1);
        CHECK(player.voiceCount() == settledVoices);
    }
    SECTION("teardown while paused cannot resurrect pending motion") {
        soundscape.stopCues();
        const auto closedVoices = player.voiceCount();
        soundscape.updateAmbience({}, {}, 1);
        CHECK(player.voiceCount() == closedVoices);
        CHECK(soundscape.fieldSound() == kNoSound);
    }
    soundscape.close();
    CHECK(player.isPlaying(music)); // separately owned music remains independent
}

TEST_CASE("pausing never restarts an opening cue that naturally finished before its target",
          "[game][world][soundscape][pause-motion-audio]") {
    const auto root = test::scratchDirectory("soundscape-paused-finite-motion");
    // Retail's Tower metal opening is finite; rope has a repeating tail.
    writeBank(root, "TOWAMB", {"S_ELVMETL"}, 8192, false, false);
    writeBank(root, "COMMON", {"S_ELVROPEL"});
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape soundscape;
    soundscape.open(root, &player, nullptr);
    soundscape.opening({.target = 1, .sound = 0});
    const auto finished = soundscape.fieldSound();
    REQUIRE(player.isPlaying(finished));
    std::array<f32, 1024> output{};
    mixer.mix(output);
    player.update();
    REQUIRE_FALSE(player.isPlaying(finished));
    // The target has not settled, so its opening record still exists.
    soundscape.opening({.target = 2, .sound = 1});
    const auto moving = soundscape.fieldSound();
    REQUIRE(player.isPlaying(moving));
    soundscape.pauseAmbience();
    CHECK_FALSE(player.isPlaying(moving));
    soundscape.pauseAmbience(); // retain the already-paused live movement
    mixer.mix(output);          // drain the stopped voice's short fade before counting resumes
    player.update();
    soundscape.updateAmbience({}, {}, 1);
    const auto resumed = soundscape.fieldSound();
    REQUIRE(player.isPlaying(resumed));
    CHECK(resumed != moving);
    CHECK(player.voiceCount() == 1);
    soundscape.updateAmbience({}, {}, 1);
    CHECK(player.voiceCount() == 1);
    soundscape.settled({.target = 2, .sound = 1});
    CHECK(soundscape.fieldSound() == kNoSound);
    CHECK_FALSE(player.isPlaying(resumed));
    soundscape.pauseAmbience();
    soundscape.updateAmbience({}, {}, 1);
    mixer.mix(output);
    player.update();
    CHECK(player.voiceCount() == 0);
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
    // Half of command 224 becomes 111 after the options scale: -3.8 dB master.
    CHECK(output.back() == Catch::Approx(0.25f * std::pow(10.0f, -68.0f / 200.0f)));
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
    SECTION("pause silences ambience without muting unrelated menu voices") {
        const SoundHandle menu = soundscape.playNamed("FIRE");
        REQUIRE(player.isPlaying(menu));
        soundscape.pauseAmbience();
        CHECK_FALSE(player.isPlaying(first));
        CHECK(player.isPlaying(menu));
        CHECK(soundscape.ambience().playingCount() == 0);
        soundscape.pauseAmbience(); // repeated pause is harmless
        soundscape.updateAmbience(listeners, AmbientEar{}, 1.0f);
        const SoundHandle restarted = soundscape.ambience().emitter(0).handle;
        CHECK(player.isPlaying(restarted));
        CHECK(restarted != first);
        soundscape.close();
    }
    SECTION("cue teardown preserves ambience until suspension or close") {
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
          "[game][world][soundscape][assets][wraith]") {
    const auto disc = test::assetOrSkip("STREAMS/DREAM5_1.ads").parent_path().parent_path();
    test::assetOrSkip("STREAMS/DREAM5_2.ads");
    const auto manifest = test::assetOrSkip("WDATA/DREAM.WAD");
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

/** Audible mono DSP frames followed by the format's unplayed transport trailer. */
std::vector<u8> dspStream(u32 frames = 1) {
    test::SyntheticBytes stream;
    stream.putFourcc("dhSS");
    for (const u32 value : {24U, 32U, 48000U, 1U, 8U, 0xFFFFFFFFU, 0U}) {
        stream.putU32(std::byteswap(value));
    }
    stream.putFourcc("dbSS").putU32(std::byteswap((frames + 1) * 8));
    stream.putU32(std::byteswap((frames + 1) * 14));
    const std::array<u8, 92> channel{};
    const std::array<u8, 8> frame{0, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    stream.putBytes(channel);
    for (u32 i = 0; i <= frames; ++i) {
        stream.putBytes(frame);
    }
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

    explicit AreaFixture(std::string_view name, u32 dspFrames = 1)
        : root(test::scratchDirectory(name)), assets(root) {
        std::filesystem::create_directories(root / "STREAMS");
        // The fourth area's stream is not written: asked for, it has nothing to play.
        for (const std::string_view file : {"areaa", "areab_1", "areab_2", "areac"}) {
            writeFile(root / "STREAMS" / std::format("{}.ads", file), dspStream(dspFrames));
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

TEST_CASE("imported ambient music duck changes playback without overriding native event duck",
          "[game][world][soundscape][music-areas][ambient-duck]") {
    AreaFixture f("soundscape-imported-ambient-duck");
    f.soundscape.duckAmbientMusic(10, .2f);
    f.frames(30);
    CHECK(f.soundscape.musicLevel() == 51);
    f.soundscape.duckAmbientMusic(std::numeric_limits<f32>::quiet_NaN(), 1);
    f.soundscape.duckAmbientMusic(1, std::numeric_limits<f32>::infinity());
    f.frames(1);
    CHECK(f.soundscape.musicLevel() == 51);
    f.soundscape.duckMusic(2, .5f);
    f.frames(30);
    CHECK(f.soundscape.musicLevel() == 127);
    // Refreshing local proximity cannot replace the active rune/event request.
    f.soundscape.duckAmbientMusic(10, .2f);
    f.frames(15);
    CHECK(f.soundscape.musicLevel() == 127);
    f.frames(30);
    CHECK(f.soundscape.musicLevel() == 51);
    f.soundscape.duckAmbientMusic(0, 1);
    f.frames(30);
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    CHECK(f.soundscape.musicArea() == 0);
}

TEST_CASE("flagged proximity sounds duck music while near and release after leaving",
          "[game][world][ambience][soundscape][music-areas]") {
    AreaFixture f("soundscape-proximity-duck");
    writeBank(f.root, "TOWAMB", {"S_SPEECH", "S_FIRE"});
    f.soundscape.open(f.root, &f.player, &f.info);
    f.soundscape.startMusic(&f.assets, 1.0f);
    writeTextFile(f.root / "world.json", R"({
      "objects": [{"name":"FLOOR","position":[0,0,0],"next":-1,"child":-1}],
      "locators": [],
      "itemInfos": [{"type":13,"subtype":0,"name":""}],
      "itemInstances": [
        {"info":0,"name":"S_speech","position":[0,0,0],
         "params":[0,0,128,64,0,0,0,0,1,0,0,0]},
        {"info":0,"name":"S_fire","position":[20,0,0],
         "params":[0,0,128,64,0,0,0,0,2,0,0,0]}
      ]
    })");
    WorldLayout layout;
    REQUIRE(layout.load(f.root));
    f.soundscape.bindAmbience(layout);
    REQUIRE(f.soundscape.ambience().size() == 2);
    const AmbientEar ear;
    const std::array<Vec3, 1> near{Vec3{0.0f}};
    const std::array<Vec3, 1> edge{Vec3{5.0f, 0.0f, 0.0f}};
    const std::array<Vec3, 1> far{Vec3{20.0f, 0.0f, 0.0f}};
    f.soundscape.updateAmbience(far, ear, 1.0f);
    f.frames(30);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    REQUIRE_FALSE(f.soundscape.ambience().musicScale().has_value());
    const auto approach = [&](const auto& party, bool ducked = false) {
        for (s32 frame = 0; frame < 30; ++frame) {
            f.soundscape.updateAmbience(party, ear, 1.0f, ducked);
            f.frames(1);
        }
    };
    approach(near);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    const SoundHandle speech = f.soundscape.ambience().emitter(0).handle;
    REQUIRE(f.player.isPlaying(speech));
    approach(edge);
    REQUIRE(f.soundscape.musicLevel() == 191); // 255 * (1 - .5 * .5)
    REQUIRE(f.soundscape.ambience().emitter(0).handle == speech);
    approach(near, true); // trigger-camera attenuation does not cancel the item's music flag
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    f.soundscape.updateAmbience(far, ear, 1.0f);
    REQUIRE_FALSE(f.player.isPlaying(speech));
    f.frames(3);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    f.frames(45);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    approach(near);
    REQUIRE(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    REQUIRE(f.soundscape.ambience().emitter(0).handle != speech);
}

TEST_CASE("soundscape forwards population changes to ambience and music zones",
          "[game][world][soundscape][ambient-population]") {
    AreaFixture f("soundscape-population");
    writeBank(f.root, "TOWAMB", {"S_SPEECH"});
    f.soundscape.open(f.root, &f.player, &f.info);
    f.soundscape.startMusic(&f.assets, 1);
    writeTextFile(f.root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":13}], "itemInstances":[
        {"info":0,"minPlayers":3,"name":"S_SPEECH","position":[0,0,0],
         "params":[0,0,128,64,0,0,0,0,1,0,0,0]},
        {"info":0,"minPlayers":3,"position":[0,0,0],
         "params":[0,0,32,65,2,0,0,0,2,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(f.root));
    f.soundscape.bindAmbience(layout);
    const std::array listeners{Vec3{0}};
    const auto update = [&] {
        f.soundscape.updateAmbience(listeners, {}, 1);
        f.soundscape.updateMusicAreas(listeners);
        f.frames(1);
    };
    f.soundscape.setPlayerCount(2);
    update();
    CHECK(f.soundscape.ambience().playingCount() == 0);
    CHECK(f.soundscape.musicRequest() == 0);
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    f.soundscape.setPlayerCount(3);
    for (s32 tick = 0; tick < 30; ++tick) {
        update();
    }
    CHECK(f.soundscape.ambience().playingCount() == 1);
    CHECK(f.soundscape.musicRequest() == 1);
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
    const auto voice = f.soundscape.ambience().emitter(0).handle;
    f.soundscape.setPlayerCount(2);
    for (s32 tick = 0; tick < 60; ++tick) {
        update();
    }
    CHECK_FALSE(f.player.isPlaying(voice));
    CHECK_FALSE(f.soundscape.ambience().musicScale().has_value());
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    // Leaving a zone does not choose a different track; it only stops requesting it.
    CHECK(f.soundscape.musicRequest() == 1);
    f.soundscape.selectMusicArea(0, MusicSwitch::AtOnce);
    update();
    CHECK(f.soundscape.musicRequest() == 0);
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
          "[game][world][soundscape][music-areas][assets]") {
    const auto disc = test::assetOrSkip("STREAMS/DREAM1A.ads").parent_path().parent_path();
    test::assetOrSkip("STREAMS/dream1e_1.ads");
    test::assetOrSkip("STREAMS/dream1e_2.ads");
    const auto manifest = test::assetOrSkip("WDATA/DREAM.WAD");
    const auto root = manifest.parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELJ1/WORLDS.PS2");
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

TEST_CASE("native soundscape rewind restores both logical queues without emitting audio",
          "[soundscape-local-rewind]") {
    const auto root = test::scratchDirectory("soundscape-local-speech");
    writeSecondBank(root, "VOICE1", {"LINE", "OTHER"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    writeSecondBank(root, "COMMON", {"BARK"});
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape reference;
    LevelSoundscape predicted;
    reference.open(root, nullptr, nullptr);
    predicted.open(root, &output, nullptr);
    LevelSoundscape::LocalSnapshot saved;
    usize commands = 0;
    // A missing-output reference and an actually failing output must make the
    // same logical admission choices before and after every corrected branch.
    const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
        ++commands;
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            throw std::runtime_error("test optional narration output");
        }
    });
    const std::array lines{std::string_view{"LINE"}, std::string_view{"OTHER"}};
    const auto advance = [&](LevelSoundscape& sounds, s32 tick) {
        sounds.holdNarration(tick % 19 >= 12);
        if (tick % 11 == 0) {
            sounds.announce(character, "NAME", false, lines, 0.5f);
            sounds.barkNamed("BARK");
        }
        sounds.updateNarration(1.0f / 60);
    };
    s32 tick = 0;
    for (s32 window = 0; window < 56; ++window) {
        CAPTURE(window, tick);
        predicted.saveLocal(saved);
        predicted.holdNarration(false);
        for (s32 step = 0; step < 1 + window % 8; ++step) {
            predicted.queueNarration("OTHER");
            predicted.barkNamed("BARK", 1, -1);
            predicted.updateNarration(0.25f);
        }
        predicted.holdNarration(true);
        const auto count = commands;
        REQUIRE(predicted.restoreLocal(saved));
        CHECK(commands == count);
        for (s32 step = 0; step < 1 + window % 8; ++step, ++tick) {
            advance(reference, tick);
            advance(predicted, tick);
            CHECK(std::ranges::equal(predicted.narrationTiming(), reference.narrationTiming()));
            CHECK(std::ranges::equal(predicted.barkTiming(), reference.barkTiming()));
            CHECK(predicted.narrationHeld() == reference.narrationHeld());
            CHECK(predicted.narrationRoom(0.5f) == reference.narrationRoom(0.5f));
            CHECK(predicted.barkRoom() == reference.barkRoom());
        }
        REQUIRE(predicted.canRestoreLocal(saved));
    }
    CHECK(commands > 0);
    output.stopAll();
    predicted.close();
    reference.close();
}

TEST_CASE("native music requests agree with absent stalled and progressing mixers through rewind",
          "[soundscape-local-rewind]") {
    // Exceed the output's lookahead: a one-frame looping fixture
    // repeatedly reinitializes both restoration filters while priming playback.
    AreaFixture reference("soundscape-music-native-reference", 6144);
    AreaFixture progressing("soundscape-music-native-progressing", 6144);
    AreaFixture stalled("soundscape-music-native-stalled", 6144);
    AreaFixture absent("soundscape-music-native-absent", 6144);
    absent.soundscape.open(absent.root, nullptr, &absent.info);
    absent.soundscape.startMusic(&absent.assets, 0.5f);
    const auto advance = [](LevelSoundscape& sounds, s32 tick) {
        if (tick % 17 == 0) {
            sounds.selectMusicArea((tick / 17) % 3, MusicSwitch::AtPartEnd);
        }
        if (tick % 23 == 0) {
            sounds.selectMusicArea((tick / 23 + 1) % 3, MusicSwitch::Faded);
        }
        if (tick % 29 == 0) {
            sounds.duckMusic(0.5f, 0.2f);
        }
        if (tick == 35) {
            sounds.stopCues();
        }
        sounds.bossAwake(tick % 47 < 20);
        sounds.updateMusic(1.0f / 60);
    };
    s32 tick = 0;
    for (s32 window = 0; window < 48; ++window) {
        CAPTURE(window, tick);
        const s32 distance = window % 8 + 1;
        for (AreaFixture* fixture : {&progressing, &stalled, &absent}) {
            LevelSoundscape::LocalSnapshot saved;
            fixture->soundscape.saveLocal(saved);
            fixture->soundscape.stopCues();
            fixture->soundscape.startMusic(&fixture->assets, 0.5f);
            fixture->soundscape.selectMusicArea(2, MusicSwitch::AtOnce);
            fixture->soundscape.duckMusic(5, 0.8f);
            for (s32 i = 0; i < distance; ++i) {
                fixture->soundscape.updateMusic(1.0f / 60);
            }
            const auto handle = fixture->soundscape.music();
            const auto voices = fixture->player.voiceCount();
            usize commands = 0;
            {
                const SoundPlayer::Capture capture(&fixture->player,
                                                   [&](const SoundPlayer::Event&) { ++commands; });
                REQUIRE(fixture->soundscape.restoreLocal(saved));
            }
            CHECK(commands == 0);
            CHECK(fixture->soundscape.music() == handle);
            CHECK(fixture->player.voiceCount() == voices);
            CHECK(fixture->soundscape.desiredMusic() == reference.soundscape.desiredMusic());
        }
        for (s32 i = 0; i < distance; ++i, ++tick) {
            for (AreaFixture* fixture : {&reference, &progressing, &stalled, &absent}) {
                if (tick == 40) {
                    fixture->soundscape.startMusic(&fixture->assets, 0.5f);
                }
                advance(fixture->soundscape, tick);
            }
            if (tick % 3 == 0) {
                progressing.drain();
            }
            CHECK(progressing.soundscape.desiredMusic() == reference.soundscape.desiredMusic());
            CHECK(stalled.soundscape.desiredMusic() == reference.soundscape.desiredMusic());
            CHECK(absent.soundscape.desiredMusic() == reference.soundscape.desiredMusic());
            CHECK(absent.soundscape.music() == kNoSound);
        }
    }
}

TEST_CASE("native restored music resumes a discarded stop only at the next service",
          "[soundscape-local-rewind]") {
    AreaFixture f("soundscape-music-restored-stop", 6144);
    LevelSoundscape::LocalSnapshot saved;
    f.soundscape.selectMusicArea(2, MusicSwitch::AtOnce);
    f.frames(1);
    const auto wanted = f.soundscape.desiredMusic();
    f.soundscape.saveLocal(saved);
    f.soundscape.stopCues();
    CHECK(f.soundscape.music() == kNoSound);
    REQUIRE_FALSE(f.soundscape.desiredMusic().enabled);
    usize commands = 0;
    {
        const SoundPlayer::Capture capture(&f.player,
                                           [&](const SoundPlayer::Event&) { ++commands; });
        REQUIRE(f.soundscape.restoreLocal(saved));
    }
    CHECK(commands == 0);
    CHECK(f.soundscape.desiredMusic() == wanted);
    CHECK(f.soundscape.music() == kNoSound);
    f.frames(1);
    CHECK(f.soundscape.musicArea() == 2);
    CHECK(f.player.isPlaying(f.soundscape.music()));
    f.soundscape.stopCues();
    f.soundscape.saveLocal(saved);
    f.soundscape.startMusic(&f.assets, 1);
    const auto speculative = f.soundscape.music();
    REQUIRE(f.player.isPlaying(speculative));
    REQUIRE(f.soundscape.restoreLocal(saved));
    CHECK(f.player.isPlaying(speculative));
    f.frames(1);
    CHECK(f.soundscape.music() == kNoSound);
    CHECK_FALSE(f.player.isPlaying(speculative));
}

TEST_CASE("native soundscape history restores music zone party filters and guards loaded banks",
          "[soundscape-local-rewind]") {
    const auto root = test::scratchDirectory("soundscape-local-areas");
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0],"next":-1,"child":-1}],
      "itemInfos":[{"type":13,"name":"ZONE"}],
      "itemInstances":[{"info":0,"minPlayers":2,"position":[0,0,0],
                       "params":[0,0,32,65,3,0,0,0,2,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    LevelSoundscape sounds;
    LevelSoundscape foreign;
    sounds.open(root, nullptr, nullptr);
    foreign.open(root, nullptr, nullptr);
    sounds.bindAmbience(layout);
    sounds.setPlayerCount(1);
    LevelSoundscape::LocalSnapshot saved;
    CHECK_FALSE(sounds.restoreLocal(saved));
    sounds.saveLocal(saved);
    CHECK_FALSE(foreign.restoreLocal(saved));
    sounds.setPlayerCount(2);
    const std::array listeners{Vec3{0}};
    sounds.updateMusicAreas(listeners);
    REQUIRE(sounds.musicRequest() == 2);
    REQUIRE(sounds.restoreLocal(saved));
    sounds.updateMusicAreas(listeners);
    CHECK(sounds.musicRequest() == 0);
    REQUIRE(sounds.restoreLocal(saved));
    sounds.stopCues();
    REQUIRE(sounds.canRestoreLocal(saved));
    REQUIRE(sounds.restoreLocal(saved));
    sounds.bindAmbience(layout);
    CHECK_FALSE(sounds.restoreLocal(saved));
    sounds.saveLocal(saved);
    sounds.suspend();
    CHECK_FALSE(sounds.restoreLocal(saved));
    sounds.saveLocal(saved);
    sounds.close();
    CHECK_FALSE(sounds.restoreLocal(saved));
    sounds.open(root, nullptr, nullptr);
    CHECK_FALSE(sounds.restoreLocal(saved));
}

TEST_CASE("scene soundscape captures finite cues and typed speech chains before output",
          "[soundscape-local-rewind][scene-sound-events]") {
    const auto root = test::scratchDirectory("soundscape-event-boundary");
    writeSecondBank(root, "COMMON", {"NAMED", "S_PICKUPMAGIC", "S_STEPROCK1", "S_ENTRANCE"});
    writeSecondBank(root, "WIZTOWER", {"PROMOTION"});
    writeSecondBank(root, "VOICE1", {"NARRATION"});
    writeSecondBank(root, "CHARACTER", {"NAME"});
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    SceneSoundEvents events;
    events.open(21, &output);
    sounds.recordTo(&events);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    events.beginTick(1);
    LevelSoundscape::Speech first;
    LevelSoundscape::Speech second;
    {
        auto scope = events.scoped({1, 1, 1, 0}, 1);
        CHECK(sounds.playNamed("NAMED") == kNoSound);
        CHECK(sounds.playAt("NAMED", {2, 0, 0}, 0, 0.5f) == kNoSound);
        CHECK(sounds.playFrom(character, "NAME") == kNoSound);
        CHECK(sounds.playPromotion("PROMOTION") == kNoSound);
        CHECK(sounds.narrate("NARRATION") == kNoSound);
        sounds.playPickup();
        sounds.playEntrance();
        sounds.playFootstep(false);
        first = sounds.speechNamed("NAMED");
        second = sounds.speechPromotion("PROMOTION", first);
        CHECK(first.handle == kNoSound);
        CHECK(second.handle == kNoSound);
        CHECK(first.timing.remainingSeconds == Catch::Approx(1));
        CHECK(second.timing.remainingSeconds == Catch::Approx(2));
        CHECK(first.cue);
        CHECK(second.cue);
        const auto missing = sounds.speechNamed("MISSING", second);
        CHECK(missing.cue == second.cue);
        CHECK(missing.timing == second.timing);
        sounds.queueNarrationFrom(character, "NAME");
        sounds.queueNarration("NARRATION");
        sounds.bark(character, "NAME");
    }
    CHECK(commands == 0);
    CHECK(output.voiceCount() == 0);
    CHECK(sounds.narrationBacklog() == Catch::Approx(2));
    CHECK(sounds.barkBacklog() == Catch::Approx(1));
    const std::array journals{events.endTick()};
    REQUIRE(journals.front().events.size() == 13);
    CHECK(journals.front().events[9].payload.after == first.cue);
    CHECK(journals.front().events[11].payload.after ==
          SoundCueToken{journals.front().events[10].id});
    const auto result = events.reconcile(1, journals);
    REQUIRE(result);
    CHECK(commands == 0);
    events.dispatch(result);
    CHECK(commands > 0);
    const auto count = commands;
    const auto replayed = events.reconcile(1, journals);
    REQUIRE(replayed);
    CHECK(replayed.actions.empty());
    events.dispatch(replayed);
    CHECK(commands == count);
    CHECK(events.unsupportedCount() == 0);
    events.close();
    sounds.close();
}

TEST_CASE("native soundscape rewind restores narration predecessor independently of playback",
          "[soundscape-local-rewind][scene-sound-events][native-equality]") {
    const auto root = test::scratchDirectory("soundscape-event-predecessor");
    writeSecondBank(root, "VOICE1", {"FIRST", "WRONG", "CORRECT"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    SceneSoundEvents events;
    events.open(22, &output);
    sounds.recordTo(&events);
    const auto capture = [&](u64 tick, u32 cue, std::string_view line) {
        events.beginTick(tick);
        {
            auto scope = events.scoped({1, 2, 1, 0}, cue);
            sounds.queueNarration(line);
        }
        return events.endTick();
    };
    const auto first = capture(1, 1, "FIRST");
    REQUIRE(first.events.size() == 1);
    LevelSoundscape::LocalSnapshot saved;
    sounds.saveLocal(saved);
    LevelSoundscape::LocalSnapshot changed;
    LevelSoundscape::LocalSnapshot correct;
    LevelSoundscape::LocalSnapshot replayed;
    sounds.saveLocal(changed);
    CHECK(sounds.equalLocal(saved, changed));
    const LevelSoundscape foreign;
    CHECK_FALSE(foreign.equalLocal(saved, changed));
    const auto wrong = capture(2, 99, "WRONG");
    REQUIRE(wrong.events.size() == 1);
    sounds.saveLocal(changed);
    CHECK_FALSE(sounds.equalLocal(saved, changed));
    REQUIRE(sounds.restoreLocal(saved));
    CHECK(sounds.narrationBacklog() == Catch::Approx(1));
    const auto corrected = capture(2, 2, "CORRECT");
    REQUIRE(corrected.events.size() == 1);
    CHECK(corrected.events.front().payload.after == SoundCueToken{first.events.front().id});
    CHECK(corrected.events.front().payload.after != SoundCueToken{wrong.events.front().id});
    CHECK(sounds.narrationBacklog() == Catch::Approx(2));
    sounds.saveLocal(correct);
    CHECK_FALSE(sounds.equalLocal(correct, changed)); // Same queue duration, different predecessor.
    REQUIRE(sounds.restoreLocal(saved));
    CHECK(capture(2, 2, "CORRECT") == corrected);
    sounds.saveLocal(replayed);
    CHECK(sounds.equalLocal(correct, replayed));
    sounds.holdNarration(true);
    sounds.saveLocal(changed);
    CHECK_FALSE(sounds.equalLocal(correct, changed));
    REQUIRE(sounds.restoreLocal(correct));
    sounds.duckMusic(1.25f, 0.5f);
    sounds.saveLocal(changed);
    CHECK_FALSE(sounds.equalLocal(correct, changed));
    REQUIRE(sounds.restoreLocal(correct));
    sounds.servicePresentation(1.0f / 60);
    sounds.saveLocal(replayed);
    CHECK(sounds.equalLocal(correct, replayed));
    CHECK(output.voiceCount() == 0);
    events.close();
    CHECK_FALSE(sounds.equalLocal(correct, replayed));
    sounds.close();
}

TEST_CASE("recorded soundscape reports uncovered mixer paths instead of emitting them",
          "[soundscape-local-rewind][scene-sound-events]") {
    const auto root = test::scratchDirectory("soundscape-event-coverage");
    writeSecondBank(root, "COMMON", {"NAMED"});
    writeBank(root, "VOICE1", {"LOOP"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    SceneSoundEvents events;
    events.open(23, &output);
    sounds.recordTo(&events);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    CHECK_THROWS_AS(sounds.playNamed("NAMED"), SceneSoundEvents::Unsupported);
    events.beginTick(1);
    {
        auto scope = events.scoped({1, 1, 1, 0}, 1);
        CHECK_THROWS_AS(sounds.playNamed("NAMED", 1, 37), SceneSoundEvents::Unsupported);
        CHECK_THROWS_AS(sounds.queueNarration("LOOP"), SceneSoundEvents::Unsupported);
        CHECK_THROWS_AS(sounds.isPlaying(37), SceneSoundEvents::Unsupported);
        CHECK_THROWS_AS(sounds.holdSounds(), SceneSoundEvents::Unsupported);
        CHECK_NOTHROW(sounds.updateMusic(1.0f / 60));
        CHECK_NOTHROW(sounds.updateExitFlame(Vec3{0}, {}));
        CHECK_THROWS_AS(sounds.stop(37), SceneSoundEvents::Unsupported);
    }
    CHECK(events.endTick().events.empty());
    CHECK(events.unsupportedCount() == 6);
    CHECK(commands == 0);
    CHECK(sounds.narrationBacklog() == 0);
    CHECK(output.voiceCount() == 0);
    events.close();
    sounds.close();
}

TEST_CASE("native soundscape restores opening and scroll lifetimes before final output",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-owned-rewind");
    writeSecondBank(root, "COMMON",
                    {"S_ELVMETL", "S_ELVMETSTPL", "SCROLL", "WRONG", "S_EXITFLAME", "S_HOURGLASS"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    SceneSoundEvents events;
    events.open(71, &output);
    sounds.recordTo(&events);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    const auto gather = [&] {
        sounds.gatherLoops({}, {}, 1, false, nullptr, Vec3{1, 0, 0}, Vec3{-1, 0, 0});
        return events.endFrame();
    };
    events.beginTick(1);
    sounds.opening({.target = 8, .sound = 0});
    sounds.speakOverScroll("SCROLL");
    sounds.duckMusic(0.5f, 0.5f);
    const std::array initial{gather()};
    REQUIRE(initial[0].loops.size() == 4);
    CHECK(initial[0].finite.events.empty());
    CHECK(commands == 0);
    LevelSoundscape::LocalSnapshot saved;
    sounds.saveLocal(saved);
    for (u64 distance = 1; distance <= 8; ++distance) {
        INFO(distance);
        REQUIRE(sounds.restoreLocal(saved));
        events.beginTick(2);
        sounds.settled({.target = 8, .sound = 0});
        sounds.speakOverScroll("WRONG");
        const auto wrong = gather();
        REQUIRE(wrong.loops.size() == 3);
        REQUIRE(wrong.finite.events.size() == 1);
        REQUIRE(sounds.restoreLocal(saved));
        for (u64 tick = 2; tick < 2 + distance; ++tick) {
            events.beginTick(tick);
            sounds.updateMusic(1.0f / 30);
            const auto corrected = gather();
            CHECK(corrected.loops == initial[0].loops);
            CHECK(corrected.finite.events.empty());
        }
        CHECK(commands == 0);
    }
    const auto result = events.reconcileFrames(1, initial);
    REQUIRE(result);
    events.dispatch(result);
    CHECK(commands > 0);
    const auto before = commands;
    REQUIRE(sounds.restoreLocal(saved));
    CHECK(commands == before);
    events.beginTick(2);
    sounds.stopVoice();
    sounds.settled({.target = 8, .sound = 0});
    sounds.gatherLoops({}, {}, 1, false, nullptr, {}, {});
    const std::array ended{events.endFrame()};
    CHECK(ended[0].loops.empty());
    const auto stopped = events.reconcileFrames(2, ended);
    REQUIRE(stopped);
    events.dispatch(stopped);
    CHECK(events.unsupportedCount() == 0);
    sounds.close();
}

TEST_CASE("recorded sound attach waits for native narration and replaces legacy proximity loops",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-attach");
    writeSecondBank(root, "COMMON", {"S_EXITFLAME", "S_HOURGLASS"});
    writeSecondBank(root, "VOICE1", {"INTRO"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    const auto intro = sounds.queueNarration("INTRO");
    REQUIRE(output.isPlaying(intro));
    sounds.updateExitFlame(Vec3{0}, {});
    sounds.updateHourglass(Vec3{0}, {});
    REQUIRE(sounds.exitFlameOn());
    REQUIRE(sounds.hourglassOn());
    SceneSoundEvents events;
    events.open(72, &output);
    CHECK_FALSE(sounds.canAttachRecorder());
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    CHECK_THROWS_AS(sounds.recordTo(&events), SceneSoundEvents::Unsupported);
    CHECK_FALSE(sounds.recordingSounds());
    CHECK(sounds.canRestoreLocal(before));
    CHECK(sounds.exitFlameOn());
    CHECK(sounds.hourglassOn());
    CHECK(sounds.narrationBacklog() == Catch::Approx(1));
    sounds.updateNarration(1);
    CHECK(sounds.canAttachRecorder());
    sounds.recordTo(&events);
    // Output progress is irrelevant: the native queue has drained even though
    // this deliberately stalled mixer still holds the old introductory voice.
    CHECK(output.isPlaying(intro));
    CHECK_FALSE(sounds.exitFlameOn());
    CHECK_FALSE(sounds.hourglassOn());
    CHECK(sounds.narrationBacklog() == 0);
    CHECK(sounds.namedTiming("S_EXITFLAME").remainingSeconds == Catch::Approx(1));
    CHECK_FALSE(sounds.namedTiming("MISSING").playing());
    sounds.close();
}
TEST_CASE("recorded frame camera seeding removes discarded branch pan from replay",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-replay-ear");
    writeSecondBank(root, "COMMON", {"HIT"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    SceneSoundEvents events;
    events.open(73, &output);
    sounds.recordTo(&events);
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    const auto capture = [&](u64 tick, const AmbientEar& ear) {
        events.beginTick(tick);
        sounds.prepareRecordedFrame(ear);
        {
            const auto scope = sounds.soundScope({1, 0, 1, 0}, 1);
            sounds.playAt("HIT", Vec3{2, 0, 0}, 0);
        }
        return events.endFrame();
    };
    const auto original = capture(1, {});
    REQUIRE(original.finite.events.size() == 1);
    CHECK(original.finite.events[0].payload.pan == 1);
    const auto wrong = capture(2, {{0, 0, 0}, {-1, 0, 0}});
    CHECK(wrong.finite.events[0].payload.pan == -1);
    REQUIRE(sounds.restoreLocal(before));
    CHECK(capture(1, {}) == original);
    CHECK(commands == 0);
    sounds.close();
}

TEST_CASE("recorded frame preparation abandons a prior ambient duck on terminal branches",
          "[soundscape-local-rewind][scene-sound-loops][music-areas]") {
    AreaFixture f("soundscape-terminal-ambient", 2048);
    writeSecondBank(f.root, "TOWAMB", {"FIRE"});
    f.soundscape.open(f.root, &f.player, &f.info);
    f.soundscape.startMusic(&f.assets, 1);
    writeTextFile(f.root / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":13}],
      "itemInstances":[{"info":0,"name":"FIRE","position":[0,0,0],
        "params":[0,0,128,64,0,0,0,0,1,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(f.root));
    f.soundscape.bindAmbience(layout);
    SceneSoundEvents events;
    events.open(74, &f.player);
    f.soundscape.recordTo(&events);
    const std::array listeners{Vec3{0}};
    for (u64 tick = 1; tick <= 65; ++tick) {
        events.beginTick(tick);
        f.soundscape.prepareRecordedFrame({});
        f.soundscape.updateMusic(1.0f / 30);
        if (tick <= 20) {
            f.soundscape.gatherLoops(listeners, {}, 1, false, nullptr, {}, {});
        } // A terminal outcome intentionally gathers no more stage desires.
        const auto frame = events.endFrame();
        CHECK(frame.loops.size() == (tick <= 20 ? 1U : 0U));
        f.soundscape.servicePresentation(1.0f / 30);
        if (tick == 20) {
            CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel / 2);
        }
    }
    CHECK(f.soundscape.musicLevel() == LevelSoundscape::kFullLevel);
    f.soundscape.close();
}

TEST_CASE("sound recorder attachment preflight preserves existing owned lifetimes on refusal",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-attach-preflight");
    writeSecondBank(root, "COMMON", {"SCROLL", "S_ELVMETL", "S_ELVMETSTPL"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    CHECK(sounds.canAttachRecorder());
    bool scroll = false;
    SECTION("scroll owns a physical voice until explicitly closed") {
        scroll = true;
        sounds.speakOverScroll("SCROLL");
    }
    SECTION("opening owns its lifetime until the native target settles") {
        sounds.opening({.target = 2, .sound = 0});
    }
    CHECK_FALSE(sounds.canAttachRecorder());
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    SceneSoundEvents events;
    events.open(75, &output);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    CHECK_THROWS_AS(sounds.recordTo(&events), SceneSoundEvents::Unsupported);
    CHECK(commands == 0);
    CHECK_FALSE(sounds.recordingSounds());
    CHECK(sounds.canRestoreLocal(before));
    if (scroll) {
        sounds.stopVoice();
    } else {
        sounds.settled({.target = 2, .sound = 0});
    }
    CHECK(sounds.canAttachRecorder());
    sounds.recordTo(&events);
    CHECK(sounds.recordingSounds());
    CHECK_FALSE(sounds.canAttachRecorder());
    CHECK_FALSE(sounds.canRestoreLocal(before));
    sounds.close();
}

TEST_CASE("soundscape recorder refuses queued barks before changing any owned output",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-bark-attach");
    writeSecondBank(root, "COMMON", {"HURT", "S_EXITFLAME", "S_HOURGLASS"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    REQUIRE(sounds.barkNamed("HURT") != kNoSound);
    sounds.updateExitFlame(Vec3{0}, {});
    sounds.updateHourglass(Vec3{0}, {});
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    SceneSoundEvents events;
    events.open(76, &output);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    CHECK_FALSE(sounds.canAttachRecorder());
    CHECK_THROWS_AS(sounds.recordTo(&events), SceneSoundEvents::Unsupported);
    CHECK(commands == 0);
    CHECK_FALSE(sounds.recordingSounds());
    CHECK(sounds.canRestoreLocal(before));
    CHECK(sounds.barkBacklog() == Catch::Approx(1));
    CHECK(sounds.exitFlameOn());
    CHECK(sounds.hourglassOn());
    sounds.updateNarration(1);
    REQUIRE(sounds.canAttachRecorder());
    sounds.recordTo(&events);
    CHECK(sounds.recordingSounds());
    CHECK_FALSE(sounds.canRestoreLocal(before));
    sounds.close();
}

TEST_CASE("soundscape recorder refuses closed epochs before silencing proximity loops",
          "[soundscape-local-rewind][scene-sound-loops]") {
    const auto root = test::scratchDirectory("soundscape-closed-attach");
    writeSecondBank(root, "COMMON", {"S_EXITFLAME"});
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape sounds;
    sounds.open(root, &output, nullptr);
    sounds.updateExitFlame(Vec3{0}, {});
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    SceneSoundEvents closed;
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    CHECK_THROWS_AS(sounds.recordTo(&closed), SceneSoundEvents::Unsupported);
    CHECK(commands == 0);
    CHECK(sounds.canRestoreLocal(before));
    CHECK_FALSE(sounds.recordingSounds());
    CHECK(sounds.exitFlameOn());
    sounds.close();
}
} // namespace
