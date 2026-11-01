#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAnnouncements.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSpeech.h"
#include "fixtures/NativeSoundBank.h"
#include "game/audio/LevelSoundscape.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted";
}
void requireNative() {
    if (!std::filesystem::is_regular_file(nativeRoot() / "audio/mort.mdir") ||
        !std::filesystem::is_regular_file(nativeRoot() / "_program/game.bin"))
        SKIP("Native N64 dialogue assets are not installed");
}
void writeBank(const std::filesystem::path& root, std::string_view bank,
               std::initializer_list<std::string_view> names) {
    std::string records;
    usize index = 0;
    for (const auto name : names) {
        if (index != 0)
            records += ',';
        records += std::format(
            R"({{"index":{},"name":"{}","id":0,"duration":-1,"volume":127,"duck":0,"priority":0,"sequence":[{{"sample":0,"loopStart":false,"loopBack":false}}]}})",
            index++, name);
    }
    const std::array<test::NativeSoundSample, 1> samples{{{8000, std::vector<s16>(800, 4096)}}};
    test::writeNativeSoundBank(root / "audio" / bank, std::format(R"({{"sounds":[{}]}})", records),
                               samples);
}
bool silent(std::span<const f32> samples) {
    return std::ranges::all_of(samples, [](f32 value) { return value == 0; });
}
} // namespace

TEST_CASE("Native announcement adapters are optional and invalidate borrowed sound mappings",
          "[legends][audio][native-announcements]") {
    LegendsSpeech speech;
    LegendsAnnouncements announcements;
    const std::array lines{std::string_view{"S_BADLY"}};
    CHECK_FALSE(announcements.load(test::scratchDirectory("no-native-announcements"), speech));
    CHECK_FALSE(announcements.resolve(lines, AnnouncementSubject{0, 2}));
    LevelSoundscape sounds;
    LevelSoundscape::LocalSnapshot before;
    sounds.saveLocal(before);
    REQUIRE(sounds.canAttachRecorder());
    sounds.bindAnnouncements(
        [&](auto names, auto subject) { return announcements.resolve(names, subject); });
    CHECK_FALSE(sounds.canRestoreLocal(before));
    CHECK_FALSE(sounds.canAttachRecorder());
    sounds.bindAnnouncements({});
    CHECK(sounds.canAttachRecorder());

    // Missing optional assets must leave a playable, complete base-game phrase,
    // not merely return a null metadata result.
    const auto root = test::scratchDirectory("native-announcement-absent-fallback");
    writeBank(root, "VOICE1", {"S_BADLY"});
    writeBank(root, "CLASS", {"NAME"});
    SoundSet character;
    REQUIRE(character.load(root / "audio/CLASS"));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    sounds.open(root, &output, nullptr);
    sounds.bindAnnouncements(
        [&](auto names, auto subject) { return announcements.resolve(names, subject); });
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&output, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play)
            events.push_back(event);
    });
    REQUIRE(sounds.announce(character, "NAME", false, lines, 0, AnnouncementSubject{0, 2}));
    REQUIRE(events.size() == 2);
    CHECK(events[1].after == events[0].handle);
    CHECK(
        std::ranges::none_of(events, [](const auto& event) { return event.bank == "N64/SUMNER"; }));
    std::array<f32, 1600> samples{};
    mixer.mix(samples);
    CHECK_FALSE(silent(samples));
    sounds.close();
}

TEST_CASE("Native narrator context preserves the original color and class tables",
          "[legends][audio][native-announcements][assets]") {
    requireNative();
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    // e007c730 substitutes the two signed placeholders from e007c850/e007c858.
    constexpr std::array<u16, 4> colors{163, 166, 156, 164};
    constexpr std::array classes{std::pair{0, 157},  std::pair{1, 158}, std::pair{2, 167},
                                 std::pair{3, 165},  std::pair{8, 159}, std::pair{9, 161},
                                 std::pair{10, 160}, std::pair{11, 162}};
    for (const auto [character, clip] : classes) {
        for (s32 color = 0; color < 4; ++color) {
            CAPTURE(character, color);
            const auto cue = speech.resolve(0x4056, AnnouncementSubject{character, color});
            REQUIRE(cue);
            CHECK(cue->clips == std::vector<u16>{colors[color], static_cast<u16>(clip), 187});
        }
    }
    CHECK_FALSE(speech.resolve(0x4056));
    for (s32 character : {-1, 4, 5, 6, 7, 12, 13, 14, 15, 16, 17}) {
        CAPTURE(character);
        CHECK_FALSE(speech.resolve(0x4056, AnnouncementSubject{character, 2}));
        CHECK_FALSE(speech.resolve(0x4010, AnnouncementSubject{character, 2}));
    }
    CHECK_FALSE(speech.resolve(0x4056, AnnouncementSubject{0, -1}));
    CHECK_FALSE(speech.resolve(0x4056, AnnouncementSubject{0, 4}));
    CHECK_FALSE(speech.resolve(0x4056, AnnouncementSubject{0, 2, true}));
    // Native rank substitutions are deliberately not guessed from GDL ranks.
    CHECK_FALSE(speech.resolve(0x4000, AnnouncementSubject{0, 2}));
}

TEST_CASE("Native complete announcements share restored words and retain complete fallback",
          "[legends][audio][native-announcements][assets]") {
    requireNative();
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    LegendsAnnouncements announcements;
    const auto began = std::chrono::steady_clock::now();
    REQUIRE(announcements.load(nativeRoot(), speech));
    const auto milliseconds =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - began).count();
    INFO("Complete announcement cache load milliseconds: " << milliseconds);
    WARN("Complete announcement cache load milliseconds: " << milliseconds);
    const std::array lines{std::string_view{"S_BADLY"}};
    std::set<const SoundClip*> clips;
    for (const s32 character : {0, 1, 2, 3, 8, 9, 10, 11}) {
        for (s32 color = 0; color < 4; ++color) {
            const auto sequence =
                announcements.resolve(lines, AnnouncementSubject{character, color});
            REQUIRE(sequence);
            REQUIRE(sequence->steps.size() == 3);
            CHECK(sequence->volume > 0);
            CHECK(sequence->volume <= 1);
            for (const auto& step : sequence->steps)
                clips.insert(step.clip);
        }
    }
    CHECK(clips.size() == 13); // Four colors, eight classes, one shared phrase tail.
    LegendsAnnouncements second;
    REQUIRE(second.load(nativeRoot(), speech));
    const auto first = announcements.resolve(lines, AnnouncementSubject{0, 2});
    const auto repeat = second.resolve(lines, AnnouncementSubject{0, 2});
    REQUIRE(first);
    REQUIRE(repeat);
    CHECK(first->steps.front().clip == repeat->steps.front().clip);
    const std::array hasFire{std::string_view{"S_HAS"}, std::string_view{"S_FIREAMVOX"}};
    REQUIRE(announcements.resolve(hasFire, AnnouncementSubject{0, 2}));
    CHECK_FALSE(announcements.resolve(hasFire, AnnouncementSubject{16, 2}));
    CHECK_FALSE(announcements.resolve(hasFire, AnnouncementSubject{0, 2, true}));
    const std::array unknown{std::string_view{"S_HAS"}, std::string_view{"S_LOSTLEVEL"}};
    CHECK_FALSE(announcements.resolve(unknown, AnnouncementSubject{0, 2}));
    CHECK_FALSE(announcements.resolve(std::array{std::string_view{"S_GAMEOVERVOX"}}));
    CHECK_FALSE(announcements.resolve(std::array{std::string_view{"S_COUNT3"}}));
}

TEST_CASE("Native announcements reach the shared mixer with one phrase gain and pause safely",
          "[legends][audio][native-announcements][native-announcement-mix][assets]") {
    requireNative();
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    LegendsAnnouncements announcements;
    REQUIRE(announcements.load(nativeRoot(), speech));
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    const std::array<u16, 3> words{156, 157, 187}; // Red Warrior + native low-health line.
    for (f32 slider : {128.f / 255.f, 1.f}) {
        CAPTURE(slider);
        AudioMixer mixer(48000), referenceMixer(48000);
        SoundPlayer player(mixer);
        player.setCategoryVolume(SoundCategory::Effects, slider);
        LevelSoundscape sounds;
        sounds.open(test::scratchDirectory("native-announcement-mix"), &player, nullptr);
        sounds.bindAnnouncements(
            [&](auto names, auto subject) { return announcements.resolve(names, subject); });
        SoundSet unusedCharacter;
        const std::array lines{std::string_view{"S_BADLY"}};
        std::vector<SoundPlayer::Event> events;
        const SoundPlayer::Capture capture(&player,
                                           [&](const auto& event) { events.push_back(event); });
        REQUIRE(sounds.announce(unusedCharacter, "MISSING", false, lines, 0,
                                AnnouncementSubject{0, 2}));
        REQUIRE(events.size() == 1);
        CHECK(events.front().bank == "N64/SUMNER");
        CHECK(events.front().category == SoundCategory::Effects);
        REQUIRE_FALSE(sounds.announce(unusedCharacter, "MISSING", false, lines, 0,
                                      AnnouncementSubject{0, 2}));
        CHECK(events.size() == 1);
        CHECK_THROWS_AS(sounds.bindAnnouncements({}), std::logic_error);
        auto source = bank.createRestored(words);
        REQUIRE(source);
        const auto calibration =
            legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Speech);
        REQUIRE(calibration);
        std::vector<f32> pcm;
        while (source->read(pcm, 4096)) {
        }
        REQUIRE_FALSE(pcm.empty());
        for (auto& sample : pcm)
            sample *= calibration->gain;
        auto reference = referenceMixer.createStream(source->desc());
        reference->setVolume(slider);
        reference->push(pcm);
        reference->finish();
        std::array<f32, 1600> actual{}, expected{};
        f64 square = 0;
        f32 peak = 0, maxError = 0;
        const usize ticks = static_cast<usize>(std::ceil(sounds.narrationBacklog() * 60)) + 4;
        for (usize tick = 0; tick < ticks; ++tick) {
            if (tick == 12) {
                const auto backlog = sounds.narrationBacklog();
                player.setCategoryPaused(SoundCategory::Effects, true);
                reference->setPaused(true);
                for (s32 paused = 0; paused < 15; ++paused) {
                    player.update();
                    mixer.mix(actual);
                    referenceMixer.mix(expected);
                    CHECK(silent(actual));
                    CHECK(silent(expected));
                    CHECK(sounds.narrationBacklog() == backlog);
                }
                player.setCategoryPaused(SoundCategory::Effects, false);
                reference->setPaused(false);
            }
            player.update();
            mixer.mix(actual);
            referenceMixer.mix(expected);
            sounds.updateNarration(1.f / 60);
            for (usize i = 0; i < actual.size(); ++i) {
                REQUIRE(std::isfinite(actual[i]));
                maxError = std::max(maxError, std::abs(actual[i] - expected[i]));
                square += static_cast<f64>(actual[i]) * actual[i];
                peak = std::max(peak, std::abs(actual[i]));
            }
        }
        // Shared words factor their one phrase gain into PCM and stream gain;
        // allow only the resulting float multiplication roundoff, not drift.
        CHECK(maxError < 0.000002f);
        CHECK(peak > .1f);
        CHECK(peak <= 1.f);
        const f64 rms = std::sqrt(square / (ticks * actual.size()));
        INFO("Native phrase slider=" << slider << " RMS=" << rms << " peak=" << peak);
        WARN("Native phrase slider=" << slider << " RMS=" << rms << " peak=" << peak);
        CHECK(rms > .035);
        CHECK(sounds.narrationBacklog() == 0);
        sounds.holdNarration(true);
        CHECK_FALSE(sounds.announce(unusedCharacter, "MISSING", false, lines, 0,
                                    AnnouncementSubject{0, 2}));
        sounds.holdNarration(false);
        REQUIRE(sounds.announce(unusedCharacter, "MISSING", false, lines, 0,
                                AnnouncementSubject{0, 2}));
        sounds.suspend();
        CHECK(sounds.narrationBacklog() == 0);
        for (s32 tick = 0; tick < 4; ++tick) {
            player.update();
            mixer.mix(actual);
        }
        CHECK(silent(actual));
        CHECK(player.voiceCount() == 0);
    }
}

TEST_CASE("Unsupported native narrator subjects fall back as complete Dark Legacy sentences",
          "[legends][audio][native-announcements][assets]") {
    requireNative();
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    LegendsAnnouncements announcements;
    REQUIRE(announcements.load(nativeRoot(), speech));
    const auto root = test::scratchDirectory("native-announcement-fallback");
    writeBank(root, "VOICE1", {"S_BADLY", "S_HAS", "S_LOSTLEVEL", "S_POJO2"});
    writeBank(root, "CLASS", {"NAME"});
    SoundSet character;
    REQUIRE(character.load(root / "audio/CLASS"));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    LevelSoundscape sounds;
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&player, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play)
            events.push_back(event);
    });
    const auto open = [&] {
        sounds.close();
        events.clear();
        sounds.open(root, &player, nullptr);
        sounds.bindAnnouncements(
            [&](auto names, auto subject) { return announcements.resolve(names, subject); });
    };
    for (s32 id : {4, 5, 6, 7, 12, 13, 14, 15, 16}) {
        CAPTURE(id);
        open();
        REQUIRE(sounds.announce(character, "NAME", false, std::array{std::string_view{"S_BADLY"}},
                                0, AnnouncementSubject{id, 2}));
        REQUIRE(events.size() == 2);
        CHECK(events[0].bank == character.sequence(*character.find("NAME")).bank);
        CHECK(events[1].bank != "N64/SUMNER");
        CHECK(events[1].after == events[0].handle);
    }
    open();
    const std::array unknown{std::string_view{"S_HAS"}, std::string_view{"S_LOSTLEVEL"}};
    REQUIRE(sounds.announce(character, "NAME", false, unknown, 0, AnnouncementSubject{0, 2}));
    REQUIRE(events.size() == 3);
    CHECK(
        std::ranges::none_of(events, [](const auto& event) { return event.bank == "N64/SUMNER"; }));
    open();
    REQUIRE(sounds.announce(character, "NAME", true, std::array{std::string_view{"S_BADLY"}}, 0,
                            AnnouncementSubject{0, 2, true}));
    REQUIRE(events.size() == 2);
    CHECK(
        std::ranges::none_of(events, [](const auto& event) { return event.bank == "N64/SUMNER"; }));
    CHECK(events[0].bank != character.sequence(*character.find("NAME")).bank);
    sounds.close();
}

TEST_CASE("Native announcement routes agree with authored powerup and advice tables",
          "[legends][audio][native-announcements][assets]") {
    requireNative();
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    LegendsAnnouncements announcements;
    REQUIRE(announcements.load(nativeRoot(), speech));
    const auto powerups = readFile(nativeRoot() / "items/generic/ppow.rom");
    struct Powerup {
        std::string_view name;
        u16 key;
        u32 token;
    };
    constexpr std::array powers{
        Powerup{"S_XSPEED", 0x200, 0x4007},       Powerup{"S_REFLECTVOX", 0x202, 0x4008},
        Powerup{"S_SUPERVOX", 0x203, 0x400a},     Powerup{"S_3WAYSHOTVOX", 0x204, 0x400c},
        Powerup{"S_INVISVOX", 0x205, 0x400b},     Powerup{"S_INVULVOX", 0x206, 0x400e},
        Powerup{"S_FIREAMVOX", 0x207, 0x4010},    Powerup{"S_LGHTNGAMVOX", 0x208, 0x4011},
        Powerup{"S_LIGHTAMVOX", 0x209, 0x4013},   Powerup{"S_ACIDAMVOX", 0x20a, 0x4012},
        Powerup{"S_STOPPEDVOX", 0x20c, 0x4014},   Powerup{"S_ANTIDEATHVOX", 0x20d, 0x4015},
        Powerup{"S_5WAYSHOTVOX", 0x20e, 0x400d},  Powerup{"S_FIREBRVOX", 0x210, 0x4016},
        Powerup{"S_LGHTNGBRVOX", 0x211, 0x4017},  Powerup{"S_ACIDBRVOX", 0x212, 0x4018},
        Powerup{"S_PHOENIXVOX", 0x213, 0x4019},   Powerup{"S_GROWTHVOX", 0x214, 0x401a},
        Powerup{"S_REFLECTSHVOX", 0x216, 0x4009}, Powerup{"S_FIREWALLSHVOX", 0x217, 0x401c},
        Powerup{"S_LGHTNGSHVOX", 0x218, 0x401d},  Powerup{"S_HAMMERVOX", 0x219, 0x401e},
        Powerup{"S_RAPIDFIREVOX", 0x21a, 0x401f}, Powerup{"S_LEVVOX", 0x21c, 0x4020},
        Powerup{"S_ICEAXEVOX", 0x2901, 0x4021},   Powerup{"S_SCIMITARVOX", 0x2903, 0x4024},
        Powerup{"S_JAVELINVOX", 0x2904, 0x4022},  Powerup{"S_SAVIORVOX", 0x2905, 0x4066}};
    for (const auto& power : powers) {
        CAPTURE(power.name);
        bool found = false;
        for (usize row = 0; row < readU32BE(powerups, 0); ++row) {
            const usize at = 4 + row * 28;
            if (readU16BE(powerups, at) == power.key) {
                REQUIRE(readU32BE(powerups, at + 24) == power.token);
                found = true;
                break;
            }
        }
        REQUIRE(found);
        const auto sequence =
            announcements.resolve(std::array{power.name}, AnnouncementSubject{0, 2});
        REQUIRE(sequence);
        CHECK((sequence->call & 0xffff) == power.token);
    }
    const auto program = readFile(nativeRoot() / "_program/game.bin");
    struct Advice {
        std::string_view name;
        usize row;
        u32 text;
        u32 token;
    };
    constexpr std::array advice{
        Advice{"S_AVOID", 0, 12, 0x1090},         Advice{"S_SAVEKEYS", 1, 13, 0x1008},
        Advice{"S_KEYFULL", 2, 8, 0x4025},        Advice{"S_MAGICFULL", 3, 11, 0x4026},
        Advice{"S_USEMAGIC2", 4, 14, 0x4027},     Advice{"S_THROWMAGIC", 5, 15, 0x1084},
        Advice{"S_SHIELDMAGIC", 6, 16, 0x1085},   Advice{"S_DESTROY", 7, 17, 0x105b},
        Advice{"S_USEMAGIC", 8, 18, 0x4028},      Advice{"S_COLLECTGOLD", 9, 19, 0x108f},
        Advice{"S_COLLECTPOT", 11, 20, 0x1083},   Advice{"S_TRANSPORTER", 12, 21, 0x108a},
        Advice{"S_SAMEEXIT", 13, 22, 0x1006},     Advice{"S_SHOOTINGMAGIC", 14, 23, 0x1086},
        Advice{"S_MEATGIVES", 15, 24, 0x108b},    Advice{"S_FRUITGIVES", 16, 25, 0x108c},
        Advice{"S_POISONEDFOOD", 17, 26, 0x108d}, Advice{"S_MULTIPLEHITS", 19, 28, 0x1089},
        Advice{"S_TRAPSMAKE", 23, 32, 0x1099},    Advice{"S_USEKEY", 36, 36, 0x1088},
        Advice{"S_USEKEY2", 37, 37, 0x1009}};
    for (const auto& hint : advice) {
        CAPTURE(hint.name);
        const usize at = 0xe00722d0U - 0xe0000400U + hint.row * 24;
        CHECK(readU32BE(program, at + 8) == hint.text);
        REQUIRE(readU32BE(program, at + 12) == hint.token);
        const auto sequence =
            announcements.resolve(std::array{hint.name}, AnnouncementSubject{0, 2});
        REQUIRE(sequence);
        CHECK((sequence->call & 0xffff) == hint.token);
    }
}

TEST_CASE("A silent required native word cannot replace a complete fallback sentence",
          "[legends][audio][native-announcements][assets]") {
    requireNative();
    const auto original = readFile(nativeRoot() / "audio/mort.mdir");
    const usize count = readU32BE(original, 0);
    REQUIRE(count == 279);
    std::vector<u8> changed(4 + count * 4);
    const auto put32 = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte)
            changed[at + byte] = static_cast<u8>(value >> ((3 - byte) * 8));
    };
    put32(0, static_cast<u32>(count));
    // Valid one-block, 8 kHz MORT silence. Only Warrior's word is replaced.
    constexpr std::array<u8, 16> silence{'M', 'O', 'R', 'T', 0, 1, 0x1f, 0x40,
                                         0,   0,   0,   4,   0, 0, 0,    1};
    for (usize id = 0; id < count; ++id) {
        put32(4 + id * 4, static_cast<u32>(changed.size()));
        if (id == 157) {
            changed.insert(changed.end(), silence.begin(), silence.end());
        } else {
            const usize begin = readU32BE(original, 4 + id * 4);
            const usize end = id + 1 == count ? original.size() : readU32BE(original, 8 + id * 4);
            changed.insert(changed.end(), original.begin() + begin, original.begin() + end);
        }
    }
    const auto root = test::scratchDirectory("native-announcement-silent-word");
    std::filesystem::create_directories(root / "audio");
    std::filesystem::create_directories(root / "_program");
    writeFile(root / "audio/mort.mdir", changed);
    writeFile(root / "_program/game.bin", readFile(nativeRoot() / "_program/game.bin"));
    LegendsSpeech speech;
    REQUIRE(speech.openN64(root));
    LegendsAnnouncements announcements;
    REQUIRE(announcements.load(root, speech));
    const std::array lines{std::string_view{"S_BADLY"}};
    CHECK_FALSE(announcements.resolve(lines, AnnouncementSubject{0, 2}));
    CHECK(announcements.resolve(lines, AnnouncementSubject{1, 2}));
}
