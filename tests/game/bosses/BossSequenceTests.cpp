#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "fixtures/NativeSoundBank.h"
#include "game/bosses/BossSequence.h"
#include "game/effects/EffectObservation.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<BossSequence>);
static_assert(!std::is_copy_constructible_v<BossSequence>);

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelCatalog levels;
    Bosses bosses;
    BossSequence sequence;
    std::array<PlayerRuntime, 2> players;
    MessageTable strings;
    Fixture() {
        players[0].actor.spawn(3, {}, nullptr, Vec3{1, 2, 3}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{4, 5, 6}, 0);
    }
    void bind() { sequence.bind({device, world, weapons, textures, effects, audio, &levels}); }
    void loadLevel(s32 bossType = 41, std::string_view items = "") {
        const auto root = test::scratchDirectory("boss-sequence");
        const auto level = root / "LEVELS/LEVELG5";
        std::filesystem::create_directories(level);
        std::filesystem::create_directories(root / "wdata");
        writeTextFile(root / "wdata/TOWN.json",
                      std::format(R"({{"realm":7,"prefix":"levelG","levels":[
            {{"name":"G5","bossType":{},"rune":1}},{{"name":"G1","rune":2}}]}})",
                                  bossType));
        writeTextFile(level / "world.json", std::format(R"({{"objects":[
            {{"name":"FLOOR","object":0,"parent":-1,"position":[0,0,0]}}]{}}})",
                                                        items));
        writeTextFile(level / "objects.json",
                      R"({"objects":[{"name":"FLOOR","file":"mesh.obj","meshTriangles":1}]})");
        writeTextFile(level / "mesh.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                      "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
        writeFile(level / "skin.png", test::kTinyPng);
        writeTextFile(level / "textures.json", R"({"bitmaps":[
            {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(level);
        REQUIRE(levels.load(root));
        const auto ref = levels.byName("G5");
        REQUIRE(ref.has_value());
        REQUIRE(world.load(device, root, *ref));
        REQUIRE(world.level() != nullptr);
        writeTextFile(level / "animations.json", R"({"trees":[
            {"name":"BOSSKEY","nodes":[{"name":"FLOOR","object":"FLOOR","parent":-1,"position":[0,0,0]}]},
            {"name":"WIZARD","nodes":[{"name":"FLOOR","object":"FLOOR","parent":-1,"position":[0,0,0]}]}]})");
        REQUIRE(world.items().load(level));
        bind();
    }
};

TEST_CASE("boss sequence bearers use player ids and tolerate missing or fallen figures",
          "[game][screens][boss-sequence]") {
    Fixture f;
    REQUIRE_FALSE(BossSequence::bearer(0, 34, f.players).has_value());
    const auto bearer = BossSequence::bearer(1, 34, f.players);
    REQUIRE(bearer.has_value());
    REQUIRE(bearer->player == 1);
    REQUIRE(bearer->position == Vec3{4, 5, 6});
    REQUIRE(bearer->holdPoint == Vec3{4, 5 + LegendShow::kHeldLift, 6});
    REQUIRE_FALSE(bearer->canGesture);
    REQUIRE_FALSE(bearer->casting);
    REQUIRE_FALSE(bearer->released);
    f.players[1].figure = std::make_unique<PlayerFigure>();
    REQUIRE(BossSequence::bearer(1, 34, f.players)->canGesture);
    f.players[1].life = PlayerLife::Dying;
    REQUIRE_FALSE(BossSequence::bearer(1, 34, f.players));
}

TEST_CASE("boss sequence is inert when closed or when no level record exists",
          "[game][screens][boss-sequence]") {
    Fixture f;
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE_FALSE(f.sequence.advanceVictory(300, 5, f.players, f.strings));
    f.bind();
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    f.sequence.advanceLegend(1, f.bosses, f.players);
    REQUIRE_FALSE(f.sequence.victory().state().running());
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    f.sequence.clear();
    f.sequence.clear();
    REQUIRE(f.sequence.frozenTexture() == nullptr);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("Skorne records victory without an ordinary boss key effect", "[skorne][boss-sequence]") {
    for (const s32 kind : {41, 42, 43, 44}) {
        Fixture f;
        f.loadLevel(kind);
        f.sequence.fallen({2, 3, 4}, f.bosses, f.players);
        REQUIRE(f.players[0].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
        REQUIRE(f.sequence.victory().state().running());
        REQUIRE(f.effects.count() == (kind < 42 ? 1 : 0));
        f.sequence.fallen({2, 3, 4}, f.bosses, f.players);
        REQUIRE(f.effects.count() == (kind < 42 ? 1 : 0));
    }
}

TEST_CASE("every boss code below the temple's leaves the floating key and its sound",
          "[game][screens][boss-sequence][boss-kinds]") {
    const auto root = test::scratchDirectory("boss-key-sound");
    const auto bank = root / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {pcm.begin(), pcm.end()}}}};
    test::writeNativeSoundBank(
        bank, R"({"sounds":[{"index":0,"name":"S_BOSSKEYG","sequence":[{"sample":0}]}]})",
        bankSamples);
    struct Row {
        s32 code;
        bool key; ///< the BOSSKEY tree rises and S_BOSSKEY of the realm's letter sounds
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, true},
        {33, true},
        {34, true},
        {35, true},
        {36, true},
        {37, true},
        {38, true},
        {39, true},
        {40, true},
        {41, true},
        {42, false},
        {43, false},
        {44, false},
        {45, false},
        {std::numeric_limits<s32>::min(), true},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        AudioMixer mixer(48000);
        SoundPlayer output(mixer);
        Fixture f;
        f.loadLevel(row.code);
        f.audio.open(root, &output, nullptr);
        f.sequence.fallen({2, 3, 4}, f.bosses, f.players);
        REQUIRE(f.sequence.victory().state().running());
        CHECK(f.effects.count() == (row.key ? 1U : 0U));
        CHECK(output.voiceCount() == (row.key ? 1U : 0U));
        f.sequence.clear();
        f.audio.close();
    }
}

TEST_CASE("the boss's fall keeps the arena's camera subject to the level's end",
          "[game][screens][boss-sequence][boss-camera]") {
    // BossCameraUpdate's active flag clears only with the next level: after the wizard
    // has gone it looks at the fallen boss's base with the key's radius.
    Fixture f;
    f.loadLevel();
    REQUIRE_FALSE(f.sequence.fallen());
    f.sequence.fallen({2, 3, 4}, f.bosses, f.players);
    REQUIRE(f.sequence.fallen());
    CHECK(f.sequence.shardPosition() == Vec3{2, 3, 4} + f.bosses.rewardOffset());
    for (s32 tick = 0; tick < 2000 && !f.sequence.victory().state().finished(); ++tick) {
        f.sequence.advanceVictory(2, 1.0f / 30.0f, f.players, f.strings);
    }
    REQUIRE(f.sequence.victory().state().finished());
    REQUIRE(f.sequence.fallen());
    const BossCameraSubject rest = f.sequence.restSubject();
    CHECK(rest.position == Vec3{2, 3, 4});
    CHECK(rest.baseAttention == std::optional{Vec3{2, 3, 4}});
    CHECK(rest.focus == BossCameraSubject::Focus::Combat);
    CHECK(rest.awake);
    CHECK(rest.radius == 5.0f);
    f.bind();
    CHECK_FALSE(f.sequence.fallen());
}

TEST_CASE("boss victory rewards survivors once and counts the fallen party's runes",
          "[game][screens][boss-sequence]") {
    Fixture f;
    f.loadLevel();
    f.players[1].life = PlayerLife::InTower;
    // Level records number runes from one; save bits are zero-based.
    f.players[0].actor.save().progress().relics.addRune(0);
    f.players[1].actor.save().progress().relics.addRune(1);
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Waiting);
    REQUIRE(f.sequence.victory().state().runeQuality() == 3);
    REQUIRE(f.players[0].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
    REQUIRE_FALSE(f.players[1].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
    // A duplicate fall notification must not reset the visit or grant another reward.
    f.sequence.advanceVictory(BossVictory::kWaitTicks, 0, f.players, f.strings);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Appearing);
    f.players[0].actor.save().progress().relics.shards = 0;
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Appearing);
    s32 completions = 0;
    for (s32 tick = 0; tick < 2000; ++tick) {
        completions += f.sequence.advanceVictory(2, 1.0f / 30.0f, f.players, f.strings) ? 1 : 0;
    }
    REQUIRE(completions == 1);
    REQUIRE(f.sequence.victory().state().finished());
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    f.bind();
    REQUIRE_FALSE(f.sequence.victory().state().running());
    REQUIRE_FALSE(f.sequence.victory().state().finished());
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
}

TEST_CASE("departed players neither receive boss rewards nor inflate the rune announcement",
          "[game][screens][boss-sequence]") {
    Fixture f;
    f.loadLevel();
    f.players[0].actor.save().progress().relics.addRune(0);
    f.players[1].actor.save().progress().relics.addRune(1);
    f.players[1].departed = true;
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    CHECK(f.sequence.victory().state().runeQuality() == 1);
    CHECK(f.players[0].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
    CHECK_FALSE(f.players[1].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
}

TEST_CASE("boss coins count waiting party members but not departed controller slots",
          "[game][screens][boss-sequence][multiplayer][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    REQUIRE(f.levels.load(root));
    REQUIRE(f.world.load(f.device, root, *f.levels.byName("G5")));
    f.bind();
    std::array<PlayerRuntime, 4> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].actor.spawn(static_cast<s32>(i), {}, nullptr, Vec3{0}, 0);
    }
    players[0].life = PlayerLife::InTower;
    players[1].life = PlayerLife::InTower;
    players[1].departed = true;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    CombatSpew spew;
    spew.origin = Vec3{0, 10, 0};
    spew.velocity = Vec3{0, 10, 10};
    spew.halfAngle = 0.5f;
    const usize before = f.world.placedItems().size();
    f.sequence.spewCoins(spew, opponents, fixtures, players);
    // SetPlayerVars counts nonzero states, including the waiting dead but not quit slots.
    // G5's per-participant table is four bronze and one silver coin.
    REQUIRE(f.world.placedItems().size() == before + 15);
    usize bronze = 0;
    usize silver = 0;
    for (usize i = before; i < f.world.placedItems().size(); ++i) {
        const auto& item = f.world.placedItems().item(i);
        bronze += static_cast<usize>(item.name == "COIN_BRONZE");
        silver += static_cast<usize>(item.name == "COIN_SILVER");
        // BossSpewCoins writes each coin's worth over its record's tenth of it.
        CHECK(item.value == (item.name == "COIN_BRONZE" ? 500 : 1000));
    }
    CHECK(bronze == 12);
    CHECK(silver == 3);
}

TEST_CASE("the boss's death blast breaks the arena's cover before its coins fly",
          "[game][screens][boss-sequence][safe-rocks]") {
    Fixture f;
    // Two of the lair's rocks, one within the blast's thousand units and one beyond.
    f.loadLevel(34, R"(,"itemInfos":[{"type":10,"subtype":41,"name":"ROCK","radius":2,
        "height":5,"collisionType":1,"hitPoints":100,"armor":10}],
      "itemInstances":[
        {"info":0,"minPlayers":1,"position":[30,0,-16],"rotation":[0,0,0],
         "params":[41,0,1,0,0,0,0,0,0,0,0,0]},
        {"info":0,"minPlayers":1,"position":[1500,0,0],"rotation":[0,0,0],
         "params":[41,0,1,0,0,0,0,0,0,0,0,0]}])");
    LevelFixtures fixtures;
    fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    REQUIRE(fixtures.safeRocks().size() == 2);
    REQUIRE(fixtures.safeRocks().standing(0));
    REQUIRE(fixtures.safeRocks().standing(1));
    LevelOpponents opponents;
    CombatSpew spew;
    spew.origin = Vec3{0, 10, 0};
    spew.velocity = Vec3{0, 10, 10};
    spew.halfAngle = 0.5f;
    f.sequence.spewCoins(spew, opponents, fixtures, f.players);
    // BossSpewCoins' unseen effect of a thousand damage over a thousand units breaks the
    // near rock, so the coins can reach the party; the far one is out of its reach.
    CHECK_FALSE(fixtures.safeRocks().standing(0));
    CHECK(fixtures.safeRocks().rock(0).health == 0);
    CHECK(fixtures.safeRocks().standing(1));
    CHECK(fixtures.safeRocks().obstacles().size() == 1);
}

TEST_CASE("boss victory queues rune speech and honors authored speech loops",
          "[game][screens][boss-sequence][victory]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    Fixture f;
    f.loadLevel();
    const auto root = test::scratchDirectory("boss-victory-speech");
    const auto bank = root / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {pcm.begin(), pcm.end()}}}};
    // An authored loop has no logical completion. The second line remains
    // serialized, even if a device independently stops or loses its first voice.
    test::writeNativeSoundBank(bank, R"({"sounds":[
        {"index":0,"name":"S_DEFEATVOXG","sequence":[
            {"sample":0,"loopStart":true,"loopBack":true}]},
        {"index":1,"name":"S_RUNEVOX0G","sequence":[
            {"sample":0,"loopStart":true,"loopBack":true}]}]})",
                               bankSamples);
    f.audio.open(root, &output, nullptr);
    f.audio.holdNarration(true); // gameplay announcements must not block Sumner himself
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    f.sequence.advanceVictory(300, 5, f.players, f.strings);
    f.sequence.advanceVictory(64, 64.0f / 60, f.players, f.strings);
    REQUIRE(output.voiceCount() == 1);
    f.sequence.advanceVictory(30, 0.5f, f.players, f.strings);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Runes);
    REQUIRE(output.voiceCount() == 1); // rune line is pending, not a simultaneous voice
    f.sequence.advanceVictory(60, 1, f.players, f.strings);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Leaving);
    REQUIRE_FALSE(f.sequence.advanceVictory(600, 10, f.players, f.strings));
    REQUIRE_FALSE(f.sequence.victory().state().sparkling());

    SECTION("clearing stops both active and queued victory lines") {
        f.sequence.clear();
        std::array<f32, 2048> silence{};
        mixer.mix(silence);
        output.update();
        REQUIRE(output.voiceCount() == 0);
    }
    SECTION("device stop cannot release the departure countdown") {
        output.stopAll();
        f.sequence.advanceVictory(85, 85.0f / 60, f.players, f.strings);
        REQUIRE_FALSE(f.sequence.victory().state().sparkling());
        REQUIRE_FALSE(f.sequence.advanceVictory(35, 35.0f / 60, f.players, f.strings));
        CHECK(f.sequence.speechTiming().looping);
        f.sequence.clear();
        CHECK_FALSE(f.sequence.speechTiming().playing());
    }
}

TEST_CASE("native boss victory completes on clip time with stalled stopped or absent audio",
          "[game][screens][boss-sequence][speech-timing][determinism]") {
    struct Observation {
        BossVictory::Stage stage;
        SpeechTiming speech;
        bool sparkling;
        bool finished;
        bool operator==(const Observation&) const = default;
    };
    for (const s32 rate : {30, 60}) {
        CAPTURE(rate);
        std::vector<Observation> reference;
        for (const s32 deviceMode : {0, 1, 2}) {
            CAPTURE(deviceMode);
            AudioMixer mixer(48000);
            SoundPlayer output(mixer);
            auto fixture = std::make_unique<Fixture>();
            fixture->loadLevel();
            const auto root = test::scratchDirectory("boss-victory-logical-speech");
            const std::array<test::NativeSoundSample, 1> samples{
                {{8000, std::vector<s16>(40000, 8192)}}};
            test::writeNativeSoundBank(root / "audio/COMMON", R"({"sounds":[
                {"index":0,"name":"S_DEFEATVOXG","sequence":[{"sample":0}]},
                {"index":1,"name":"S_RUNEVOX0G","sequence":[{"sample":0}]}]})",
                                       samples);
            fixture->audio.open(root, deviceMode == 2 ? nullptr : &output, nullptr);
            fixture->sequence.fallen(Vec3{0}, fixture->bosses, fixture->players);
            std::vector<Observation> observed;
            bool waitedForSpeech = false;
            bool complete = false;
            for (s32 tick = 0; tick < 2400 && !complete; ++tick) {
                const f32 seconds = 1.0f / static_cast<f32>(rate);
                fixture->sequence.advanceAudio(seconds);
                complete = fixture->sequence.advanceVictory(1, seconds, fixture->players,
                                                            fixture->strings);
                const auto& state = fixture->sequence.victory().state();
                observed.push_back(
                    {state.stage(), fixture->sequence.speechTiming(), state.sparkling(), complete});
                waitedForSpeech |= state.stage() == BossVictory::Stage::Leaving &&
                                   fixture->sequence.speechTiming().playing() && !state.sparkling();
                if (deviceMode == 1) {
                    output.stopAll();
                    output.update();
                }
            }
            REQUIRE(complete);
            REQUIRE(waitedForSpeech);
            REQUIRE(observed.back().sparkling);
            REQUIRE_FALSE(observed.back().speech.playing());
            if (deviceMode == 0) {
                reference = observed;
            } else {
                REQUIRE((observed == reference));
            }
            fixture->sequence.clear();
            CHECK_FALSE(fixture->sequence.speechTiming().playing());
            fixture->audio.close();
        }
    }
}
TEST_CASE("native boss sequence rewind restores death birth speech and confirmed visit timing",
          "[local-rewind][boss-sequence-local-rewind]") {
    Fixture actual;
    Fixture expected;
    actual.loadLevel();
    expected.loadLevel();
    const auto root = test::scratchDirectory("boss-sequence-local-speech");
    const std::array<test::NativeSoundSample, 1> samples{{{8000, std::vector<s16>(16000, 8192)}}};
    test::writeNativeSoundBank(root / "audio/COMMON", R"({"sounds":[
        {"index":0,"name":"S_DEFEATVOXG","sequence":[{"sample":0}]},
        {"index":1,"name":"S_RUNEVOX0G","sequence":[{"sample":0}]}]})",
                               samples);
    actual.audio.open(root, nullptr, nullptr);
    expected.audio.open(root, nullptr, nullptr);
    BossSequence::LocalSnapshot saved;
    EffectTrees::LocalSnapshot effectsSaved;
    CHECK_FALSE(actual.sequence.restoreLocal(saved));
    const auto compare = [&] {
        const auto& a = actual.sequence.victory().state();
        const auto& b = expected.sequence.victory().state();
        CHECK(a.stage() == b.stage());
        CHECK(a.wizardAlpha() == b.wizardAlpha());
        CHECK(a.sparkling() == b.sparkling());
        CHECK(actual.sequence.speechTiming() == expected.sequence.speechTiming());
        CHECK(actual.sequence.victorySubject().position ==
              expected.sequence.victorySubject().position);
        REQUIRE(actual.effects.count() == expected.effects.count());
        for (usize i = 0; i < expected.effects.count(); ++i) {
            CHECK(test::observeEffect(actual.effects, actual.effects.effect(i).id) ==
                  test::observeEffect(expected.effects, expected.effects.effect(i).id));
        }
        actual.device.draws.clear();
        expected.device.draws.clear();
        actual.sequence.victory().drawWizard(actual.device, Mat4{1}, {}, nullptr, 0.37f);
        expected.sequence.victory().drawWizard(expected.device, Mat4{1}, {}, nullptr, 0.37f);
        REQUIRE(actual.device.draws.size() == expected.device.draws.size());
        for (usize i = 0; i < expected.device.draws.size(); ++i) {
            test::checkRecordedDraw(actual.device.draws[i], expected.device.draws[i]);
        }
    };
    s32 frame = 0;
    s32 completions = 0;
    for (s32 window = 0; window < 192; ++window) {
        CAPTURE(window, frame);
        const s32 distance = window % 8 + 1;
        actual.sequence.saveLocal(saved);
        actual.effects.saveLocal(effectsSaved);
        CHECK_FALSE(expected.sequence.restoreLocal(saved));
        const std::array saves{actual.players[0].actor.save(), actual.players[1].actor.save()};
        actual.sequence.fallen({90, 10, 20}, actual.bosses, actual.players);
        for (s32 tick = 0; tick < distance; ++tick) {
            actual.sequence.capturePresentation();
            actual.sequence.advanceAudio(0.7f);
            actual.sequence.advanceVictory(100, 0.7f, actual.players, actual.strings);
            actual.effects.update(0.7f);
        }
        REQUIRE(actual.effects.restoreLocal(effectsSaved));
        REQUIRE(actual.sequence.restoreLocal(saved));
        for (usize i = 0; i < saves.size(); ++i) {
            actual.players[i].actor.save() = saves[i];
        }
        compare();
        for (s32 tick = 0; tick < distance; ++tick, ++frame) {
            if (frame == 2) {
                actual.sequence.fallen({2, 3, 4}, actual.bosses, actual.players);
                expected.sequence.fallen({2, 3, 4}, expected.bosses, expected.players);
            }
            actual.sequence.capturePresentation();
            expected.sequence.capturePresentation();
            actual.sequence.advanceAudio(1.0f / 30);
            expected.sequence.advanceAudio(1.0f / 30);
            const bool a =
                actual.sequence.advanceVictory(2, 1.0f / 30, actual.players, actual.strings);
            const bool b =
                expected.sequence.advanceVictory(2, 1.0f / 30, expected.players, expected.strings);
            CHECK(a == b);
            completions += static_cast<s32>(a);
            actual.effects.update(1.0f / 30);
            expected.effects.update(1.0f / 30);
        }
        compare();
    }
    CHECK(completions == 1);
    CHECK(actual.sequence.victory().state().finished());
    CHECK(actual.device.textureUpdates == 0);
    actual.sequence.clear();
    CHECK_FALSE(actual.sequence.restoreLocal(saved));
    actual.sequence.saveLocal(saved);
    actual.bind();
    CHECK_FALSE(actual.sequence.restoreLocal(saved));
    actual.sequence.clear();
    expected.sequence.clear();
    actual.audio.close();
    expected.audio.close();
}

TEST_CASE("boss sequence digest bytes stay pinned through its legend after the victory script",
          "[game][screens][boss-sequence][digest-golden]") {
    // Bound, the sequence holds its legend presentation, so its digest takes the legend's branch.
    Fixture f;
    f.loadLevel();
    const auto step = [&f](s32 frame) {
        if (frame == 2) {
            f.sequence.fallen({2, 3, 4}, f.bosses, f.players);
        }
        f.sequence.capturePresentation();
        f.sequence.advanceAudio(1.0f / 30);
        f.sequence.advanceVictory(2, 1.0f / 30, f.players, f.strings);
        f.effects.update(1.0f / 30);
    };
    for (s32 frame = 0; frame < 4; ++frame) {
        step(frame);
    }
    const BossSequence& sequence = f.sequence;
    BossSequence::LocalSnapshot saved;
    BossVictoryPresentation::LocalSnapshot victory;
    sequence.saveLocal(saved);
    sequence.victory().saveLocal(victory);
    for (s32 frame = 4; frame < 12; ++frame) {
        step(frame);
    }
    NativeStateResources resources;
    test::registerStage(resources, f.world, f.device);
    const auto view = resources.resources();
    // SHA-256 of the sequence's native digest twelve frames in and of what it saved four frames
    // in, two after the boss fell, which netplay peers must hash alike: a literal moves only when
    // what is visited changes on purpose. Its victory follows it, so that a moved sequence names
    // the child that moved it.
    test::checkPinnedDigests("BossSequence", test::liveDigest(sequence, view),
                             test::savedDigest(sequence, saved, view),
                             "597a67d637298661982771bca634135efb862cf9951d16f1a4545be12a7574aa",
                             "24babc80425244a38b0ff4ffa1bcc57e5179e75190379445ea5ca4189601a1c0");
    test::checkPinnedDigests("BossVictoryPresentation", test::liveDigest(sequence.victory(), view),
                             test::savedDigest(sequence.victory(), victory, view),
                             "0f9d36da8271f5c706ac827e63c5565246cbe4532c13b45ee2098be3fb671512",
                             "e556fc620c55d2a30daca6e810977309a9af7b6ccd85e4597ada86678c4d5024");
    f.sequence.clear();
}

TEST_CASE("native boss coin rewind restores RNG and item flights after discarded sprays",
          "[local-rewind][boss-sequence-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture fixture;
    REQUIRE(fixture.levels.load(root));
    REQUIRE(fixture.world.load(fixture.device, root, *fixture.levels.byName("G5")));
    fixture.bind();
    LevelOpponents opponents;
    LevelFixtures fixtures;
    CombatSpew spew;
    spew.origin = Vec3{0, 10, 0};
    spew.velocity = Vec3{0, 10, 10};
    spew.halfAngle = 0.5f;
    BossSequence::LocalSnapshot saved;
    LevelWorld::LocalSnapshot worldSaved;
    fixture.sequence.saveLocal(saved);
    fixture.world.saveLocal(worldSaved);
    const usize before = fixture.world.placedItems().size();
    fixture.sequence.spewCoins(spew, opponents, fixtures, fixture.players);
    std::vector<Vec3> velocities;
    for (usize i = before; i < fixture.world.placedItems().size(); ++i) {
        velocities.push_back(fixture.world.placedItems().item(i).velocity);
    }
    REQUIRE_FALSE(velocities.empty());
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        for (s32 tick = 0; tick < distance; ++tick) {
            fixture.sequence.spewCoins(spew, opponents, fixtures, fixture.players);
        }
        REQUIRE(fixture.world.restoreLocal(worldSaved));
        REQUIRE(fixture.sequence.restoreLocal(saved));
        fixture.sequence.spewCoins(spew, opponents, fixtures, fixture.players);
        REQUIRE(fixture.world.placedItems().size() == before + velocities.size());
        for (usize i = 0; i < velocities.size(); ++i) {
            CHECK(fixture.world.placedItems().item(before + i).velocity == velocities[i]);
        }
    }
}
} // namespace
