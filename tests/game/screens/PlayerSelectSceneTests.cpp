#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/menu/MenuInput.h"
#include "game/players/Party.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayerSelectScene.h"
#include "game/screens/TitleScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("SELECT/textures.ngc").parent_path().parent_path();
}

struct Fixture {
    GameConfig config;
    StringTable strings;

    explicit Fixture(std::string_view scratch) {
        config.save.directory = test::scratchDirectory(scratch).string();
        config.save.slots = 4;
        strings.load(test::dataDirectory() / "text", config.text.language);
    }

    GameContext context(std::filesystem::path root = unpackedRoot(),
                        SoundPlayer* sounds = nullptr) const {
        GameContext out;
        out.config = &config;
        out.strings = &strings;
        out.sounds = sounds;
        out.unpackedRoot = std::move(root);
        return out;
    }
};

PlayerSelectScene::Inputs nobody() {
    return {};
}

PlayerSelectScene::Inputs player(s32 index, bool select, bool back = false, bool start = false,
                                 bool right = false) {
    PlayerSelectScene::Inputs inputs{};
    inputs[static_cast<usize>(index)].select = select;
    inputs[static_cast<usize>(index)].back = back;
    inputs[static_cast<usize>(index)].start = start;
    inputs[static_cast<usize>(index)].right = right;
    return inputs;
}

TEST_CASE("letterboxed mouse loads a character and creates another in only its hovered lane",
          "[select][mouse][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-mouse");
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    CharacterSave saved;
    saved.name = "MOUSE";
    saved.gold = 321;
    REQUIRE(slots.write(0, saved));
    PlayerSelectScene scene;
    const auto context = f.context(); // Missing retail assets must skip outside REQUIRE.
    REQUIRE(scene.open(device, context, 0));
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    const auto transform = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    scene.render(device, projection, 640, 448);
    const auto pointerAt = [&](const Rect& area, bool click = true, bool back = false) {
        const auto clip =
            transform * Vec4{area.x + area.width / 2, area.y + area.height / 2, 0.5f, 1};
        MenuInput input;
        input.pointer = (Vec2{clip} + Vec2{1}) / 2.0f;
        input.pointerNormalized = true;
        input.pointerPressed = click;
        input.pointerBack = back;
        PlayerSelectScene::Inputs inputs;
        inputs.fill(input); // every device read carries the same physical pointer
        return scene.step(1, inputs);
    };
    pointerAt(scene.lane(0).menu().itemArea(1));
    REQUIRE(scene.lane(0).state() == SelectLane::State::LoadPick);
    CHECK_FALSE(scene.lane(1).active());
    CHECK_FALSE(scene.lane(2).active());
    CHECK_FALSE(scene.lane(3).active());
    pointerAt(scene.lane(0).menu().itemArea(0));
    REQUIRE(scene.lane(0).state() == SelectLane::State::Loading);
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    scene.step(SelectLane::kNoticeTicks, nobody());
    REQUIRE(scene.lane(0).state() == SelectLane::State::ClassPick);
    CHECK(scene.lane(0).save().name == "MOUSE");
    CHECK(scene.lane(0).save().gold == 321);
    pointerAt({270, 90, 20, 20});
    REQUIRE(scene.lane(2).state() == SelectLane::State::TopMenu);
    CHECK_FALSE(scene.lane(1).active());
    CHECK_FALSE(scene.lane(3).active());
    pointerAt(scene.lane(2).menu().itemArea(0));
    REQUIRE(scene.lane(2).state() == SelectLane::State::NameEntry);
    CHECK(scene.lane(2).nameEntry().name().empty());
    const auto clickControl = [&](SelectLane::PointerAction action, char letter = 0) {
        const auto targets = scene.lane(2).pointerTargets();
        const auto it = std::ranges::find_if(targets, [&](const auto& target) {
            return target.action == action && (letter == 0 || target.letter == letter);
        });
        REQUIRE(it != targets.end());
        pointerAt(it->area);
    };
    clickControl(SelectLane::PointerAction::Letter, 'C');
    clickControl(SelectLane::PointerAction::Accept);
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    REQUIRE(scene.lane(2).state() == SelectLane::State::ClassPick);
    CHECK(scene.lane(2).save().name == "C");
    CHECK(scene.lane(0).save().name == "MOUSE");
    pointerAt({270, 90, 20, 20}, false, true);
    CHECK(scene.lane(2).state() == SelectLane::State::TopMenu);
    CHECK(scene.lane(0).state() == SelectLane::State::ClassPick);
    CHECK(scene.lane(0).slotInUse() == 0);
    pointerAt({-40, 90, 20, 20}, false, true);
    CHECK(scene.lane(0).state() == SelectLane::State::ClassPick);
}

TEST_CASE("the select screen refuses to open without unpacked data", "[game][select]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-empty");
    PlayerSelectScene scene;
    REQUIRE_FALSE(scene.open(device, f.context(test::scratchDirectory("select-none")), 0));
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("character selection continues the title music at its existing playback position",
          "[select][title][music-handoff][assets]") {
    test::assetOrSkip("audio/SELECT.vbk");
    test::assetOrSkip("TITLE/textures.ngc");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const Fixture f("select-title-music");
    const auto context = f.context(unpackedRoot(), &sounds);
    TitleScene title;
    PlayerSelectScene select;
    REQUIRE(title.open(device, context));
    const auto music = title.musicHandle();
    REQUIRE(music != kNoSound);
    SoundSet bank;
    REQUIRE(bank.load(context.unpackedRoot / "audio/SELECT"));
    const auto cue = bank.find("S_SELECTMUS");
    REQUIRE(cue);
    AudioMixer expectedMixer(48000);
    SoundPlayer expectedPlayer(expectedMixer);
    expectedPlayer.play(bank.sequence(*cue), 1, SoundCategory::Music);
    std::vector<f32> actual(9600);
    std::vector<f32> expected(9600);
    mixer.mix(actual);
    expectedMixer.mix(expected);
    REQUIRE(actual == expected);
    REQUIRE(select.open(device, context, -1, {}, false, music));
    title.releaseMusic();
    title.close();
    CHECK(sounds.isPlaying(music));
    CHECK(sounds.voiceCount() == 1);
    mixer.mix(actual);
    expectedMixer.mix(expected);
    CHECK(actual == expected);
    select.close();
    CHECK_FALSE(sounds.isPlaying(music));
}

TEST_CASE("the starting player joins and others join on Start", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-join");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 2));
    REQUIRE(scene.lane(2).active());
    REQUIRE_FALSE(scene.lane(0).active());
    REQUIRE(scene.step(1, nobody()) == SelectOutcome::Running);
    REQUIRE(scene.step(1, player(0, false, false, true)) == SelectOutcome::Running);
    REQUIRE(scene.lane(0).active());
    REQUIRE(scene.lane(0).state() == SelectLane::State::TopMenu);
    REQUIRE(scene.inputSource(0).keyboard);
    REQUIRE_FALSE(scene.inputSource(0).text);
    REQUIRE(scene.step(1, player(0, true)) == SelectOutcome::Running); // New
    REQUIRE(scene.lane(0).state() == SelectLane::State::NameEntry);
    REQUIRE(scene.inputSource(0).text);
    REQUIRE_FALSE(scene.inputSource(2).text);

    const Mat4 projection = makeLetterboxProjection(640, 448, 1920, 1080);
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 8);
    const auto& margins = device.draws.back();
    CHECK(margins.transform == Mat4{1});
    CHECK(test::minCorner(margins) == Vec2{-1, -1});
    CHECK(test::maxCorner(margins) == Vec2{1, 1});
    CHECK(margins.vertices.front().color == Color::black());
    CHECK_FALSE(margins.state.depthTest);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("Manage Character opens its player's save menu with the rest of the party retained",
          "[game][select][pause][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-manage");
    PlayerSelectScene scene;
    CharacterSave first;
    first.name = "FIRST";
    first.gold = 123;
    CharacterSave second;
    second.name = "SECOND";
    second.gold = 456;
    const std::array party{PartyMember{1, first, 0}, PartyMember{3, second, 2}};
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 3, party, true));
    REQUIRE(scene.lane(1).lockedIn());
    REQUIRE(scene.lane(1).save().toJson() == first.toJson());
    REQUIRE(scene.lane(1).slotInUse() == 0);
    REQUIRE(scene.lane(3).state() == SelectLane::State::SaveMenu);
    REQUIRE(scene.lane(3).save().toJson() == second.toJson());
    REQUIRE(scene.lane(3).slotInUse() == 2);
    REQUIRE_FALSE(scene.lane(0).active());
    REQUIRE(scene.step(1, nobody()) == SelectOutcome::Running);
    scene.close();
}

TEST_CASE("backing out of the last lane cancels the screen", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-cancel");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0));
    REQUIRE(scene.step(1, player(0, false, true)) == SelectOutcome::Cancelled);
}

TEST_CASE("Manage Character Game Over removes only that player from the returned party",
          "[game][select][manage-game-over][assets]") {
    const bool saved = GENERATE(false, true);
    const bool withTeammate = GENERATE(false, true);
    CAPTURE(saved, withTeammate);
    test::FakeRenderDevice device;
    const Fixture f("select-scene-game-over");
    PlayerSelectScene scene;
    CharacterSave leaving;
    leaving.name = "LEAVING";
    const auto slot = saved ? std::optional<usize>{0} : std::nullopt;
    std::vector party{PartyMember{0, leaving, slot}};
    CharacterSave staying;
    staying.name = "STAYING";
    staying.gold = 456;
    if (withTeammate) {
        party.push_back(PartyMember{2, staying});
    }
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0, party, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::SaveMenu);
    auto up = nobody();
    up[0].up = true;
    scene.step(1, up); // Game Over, immediately above Done.
    auto outcome = scene.step(1, player(0, true));
    if (!saved) {
        REQUIRE(scene.lane(0).state() == SelectLane::State::QuitConfirm);
        CHECK(scene.party().size() == party.size());
        scene.step(1, up); // Yes; the default is No for an unsaved character.
        outcome = scene.step(1, player(0, true));
    }
    CHECK_FALSE(scene.lane(0).active());
    CHECK_FALSE(scene.lane(0).hasCharacter());
    if (withTeammate) {
        for (s32 frame = 0; frame < 200 && outcome == SelectOutcome::Running; ++frame) {
            outcome = scene.step(1, nobody());
        }
        CHECK(outcome == SelectOutcome::Done);
        const auto result = scene.party();
        REQUIRE(result.size() == 1);
        CHECK(result[0].player == 2);
        CHECK(result[0].save.toJson() == staying.toJson());
    } else {
        CHECK(outcome == SelectOutcome::Cancelled);
        CHECK(scene.party().empty());
    }
}

TEST_CASE("the screen finishes once every player is locked in", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-done");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0));
    scene.step(1, player(0, true)); // New
    REQUIRE(scene.lane(0).state() == SelectLane::State::NameEntry);
    scene.step(1, player(0, true)); // accept the end mark: a random name
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    REQUIRE(scene.lane(0).state() == SelectLane::State::ClassPick);
    scene.step(1, player(0, false, false, false, true));
    REQUIRE(scene.lane(0).pickedClass() == 1);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).lockedIn());
    SelectOutcome outcome = SelectOutcome::Running;
    s32 frames = 0;
    while (outcome == SelectOutcome::Running && frames < 200) {
        outcome = scene.step(1, nobody());
        ++frames;
    }
    REQUIRE(outcome == SelectOutcome::Done);
    REQUIRE(frames > PlayerSelectScene::kIdleFrames);
}

TEST_CASE("a player joining a game in progress finds the party locked in beside them",
          "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-join");
    PlayerSelectScene scene;
    const auto context = f.context();
    CharacterSave playing;
    playing.name = "ALREADY";
    playing.character = 1;
    const std::array party{PartyMember{1, playing, std::optional<usize>{0}}};
    REQUIRE(scene.open(device, context, 0, party));
    CHECK(scene.lane(1).lockedIn());
    CHECK(scene.lane(1).save().name == "ALREADY");
    CHECK(scene.lane(1).slotInUse() == std::optional<usize>{0});
    CHECK(scene.lane(0).state() == SelectLane::State::TopMenu);
    // The joiner thinking better of it leaves the party as it was.
    scene.step(1, player(0, false, true));
    CHECK_FALSE(scene.lane(0).active());
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 frame = 0; frame < 200 && outcome == SelectOutcome::Running; ++frame) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
    CHECK(scene.lane(1).lockedIn());
}

TEST_CASE("selection entry keeps every simultaneous joining Start without advancing existing lanes",
          "[game][select][multiplayer][assets]") {
    // check_active_players (0x8008FED4) collects every inactive controller before
    // init_player_select(1); the screen transition must not lose the later presses.
    test::FakeRenderDevice device;
    const Fixture f("select-simultaneous-join");
    CharacterSave existing;
    existing.name = "HERE";
    PlayerSelectScene scene;
    bool inTower = false;
    const auto context = f.context();
    SECTION("from the tower") {
        const std::array party{PartyMember{2, existing, 1, false, 0, {14}}};
        REQUIRE(scene.open(device, context, -1, party));
        inTower = true;
    }
    SECTION("from the title") {
        REQUIRE(scene.open(device, context, 2));
    }
    PlayerSelectScene::Inputs joining{};
    for (auto& input : joining) {
        input.start = true;
        input.select = true;
    }
    scene.join(joining);
    CHECK(scene.lane(0).state() == SelectLane::State::TopMenu);
    CHECK(scene.lane(1).state() == SelectLane::State::TopMenu);
    CHECK(scene.lane(3).state() == SelectLane::State::TopMenu);
    if (inTower) {
        CHECK(scene.lane(2).lockedIn());
        REQUIRE(scene.party().size() == 1);
        CHECK(scene.party()[0].slot == 1);
        CHECK(scene.party()[0].helpHeard == std::vector<s32>{14});
    } else {
        CHECK(scene.lane(2).state() == SelectLane::State::TopMenu);
        CHECK(scene.party().empty());
    }
    CHECK(scene.time() == 0);
    scene.close();
    scene.join(joining);
    CHECK_FALSE(scene.lane(0).active());
}

TEST_CASE("Sumner greets a locked-in character by costume and class", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-greeting");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    PlayerSelectScene scene;
    const auto context = f.context(unpackedRoot(), &sounds);
    REQUIRE(scene.open(device, context, 0));
    REQUIRE_FALSE(scene.speaking());
    const usize before = sounds.voiceCount(); // the music, when the bank is there
    scene.step(1, player(0, true));           // New
    scene.step(1, player(0, true));           // a random name
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    scene.step(1, player(0, true)); // lock in the warrior
    REQUIRE(scene.lane(0).lockedIn());
    REQUIRE(scene.speaking());
    REQUIRE(sounds.voiceCount() > before); // the welcome (and the select click)

    // The name line follows the welcome, so Sumner is still speaking once the welcome alone
    // would have ended, and quiet some seconds later.
    SoundSet bank;
    REQUIRE(bank.load(unpackedRoot() / "audio" / "SELECT"));
    const SoundSequence welcome = bank.sequence(bank.find("S_WELCOME").value());
    f64 welcomeSeconds = 0.0;
    for (const SoundSequenceStep& step : welcome.steps) {
        welcomeSeconds += step.clip->seconds();
    }
    std::vector<f32> out(usize{4800} * 2); // a tenth of a second of stereo
    const auto drain = [&](f64 seconds) {
        const auto chunks = static_cast<s32>(std::ceil(seconds * 10.0));
        for (s32 i = 0; i < chunks; ++i) {
            mixer.mix(out);
            sounds.update();
        }
    };
    drain(welcomeSeconds + 0.5);
    REQUIRE(scene.speaking());
    drain(10.0);
    REQUIRE_FALSE(scene.speaking());
}

TEST_CASE("selection waits for the complete queued greetings before entering play",
          "[select][select-welcome][assets][multiplayer]") {
    // BGMusicStart (800A1144), called before round start, drains sndFxUpdate's
    // narration queue before replacing the select bank with level audio.
    test::FakeRenderDevice device;
    const Fixture f("select-welcome-completion");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    PlayerSelectScene scene;
    const auto context = f.context(unpackedRoot(), &sounds);
    REQUIRE(scene.open(device, context, 0));
    scene.step(1, player(2, false, false, true));
    PlayerSelectScene::Inputs choose{};
    choose[0].select = choose[2].select = true;
    scene.step(1, choose); // New, in both nonadjacent lanes.
    scene.step(1, choose); // Random names.
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    scene.step(1, choose);
    REQUIRE(scene.lane(0).lockedIn());
    REQUIRE(scene.lane(2).lockedIn());
    REQUIRE(scene.speaking());
    scene.step(200, nobody()); // Finish the portraits without advancing the audio device.
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 frame = 0; frame <= PlayerSelectScene::kIdleFrames; ++frame) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Running);

    SoundSet bank;
    REQUIRE(bank.load(context.unpackedRoot / "audio/SELECT"));
    const auto duration = [&](std::string_view name) {
        const auto index = bank.find(name);
        REQUIRE(index);
        f64 seconds = 0;
        for (const auto& part : bank.sequence(*index).steps) {
            seconds += part.clip->seconds();
        }
        return seconds;
    };
    std::array<f64, 2> lengths{};
    for (usize i = 0; i < lengths.size(); ++i) {
        const auto& save = scene.lane(static_cast<s32>(i * 2)).save();
        lengths[i] = duration("S_WELCOME") + duration(std::format("S_{}{}1S", colorCode(save.color),
                                                                  classCode(save.character)));
    }
    std::array<f32, 9600> audio{};
    const auto drain = [&](f64 seconds) {
        const auto chunks = static_cast<s32>(std::ceil(seconds * 10));
        for (s32 chunk = 0; chunk < chunks; ++chunk) {
            mixer.mix(audio);
            sounds.update();
        }
    };
    drain(std::max(lengths[0], lengths[1]) + 0.3);
    // Simultaneous welcomes must not overlap, or the last handle ends too early.
    CHECK(scene.speaking());
    CHECK(scene.step(1, nobody()) == SelectOutcome::Running);
    drain(lengths[0] + lengths[1] + 1);
    CHECK_FALSE(scene.speaking());
    for (s32 frame = 0; frame <= PlayerSelectScene::kIdleFrames; ++frame) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
}

TEST_CASE("closing selection cancels its queued welcome without stopping unrelated audio",
          "[select][select-welcome][select-audio-lifetime][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-welcome-close");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const SoundClip unrelatedClip{48000, 1, std::vector<f32>(480, 0.01f)};
    SoundSequence unrelatedSequence;
    unrelatedSequence.steps.push_back({&unrelatedClip, true, true});
    const auto unrelated = sounds.play(unrelatedSequence);
    const auto context = f.context(unpackedRoot(), &sounds);
    std::array<f32, 960> audio{};
    const auto audioStep = [&] {
        mixer.mix(audio);
        sounds.update();
    };
    {
        PlayerSelectScene scene;
        REQUIRE(scene.open(device, context, 0));
        scene.step(1, player(0, true));
        scene.step(1, player(0, true));
        scene.step(NameEntry::kFlashTicks + 1, nobody());
        scene.step(1, player(0, true));
        REQUIRE(scene.speaking());
        scene.close();
        // Stop here on the unfixed implementation, before feeding any stale borrowers.
        REQUIRE_FALSE(scene.speaking());
        audioStep();
        CHECK(sounds.voiceCount() == 1);
        CHECK(sounds.isPlaying(unrelated));
        REQUIRE(scene.open(device, context, 0));
        scene.step(1, player(0, true)); // A live menu cue and music when destroyed.
    }
    audioStep();
    CHECK(sounds.voiceCount() == 1);
    for (s32 step = 0; step < 200; ++step) {
        audioStep();
    }
    CHECK(sounds.isPlaying(unrelated));
    CHECK(sounds.voiceCount() == 1);
    CHECK(std::ranges::all_of(audio, [](f32 value) { return value == 0.01f; }));
}

TEST_CASE("a missing class announcement finishes after the welcome without holding selection",
          "[select][select-welcome][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-welcome-no-name");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const auto context = f.context(unpackedRoot(), &sounds);
    SoundSet bank;
    REQUIRE(bank.load(context.unpackedRoot / "audio/SELECT"));
    REQUIRE_FALSE(bank.find("S_REDSUM1S"));
    CharacterSave save;
    save.name = "SUMNER";
    save.character = kSumnerClass;
    save.classUnlock = 0xFFFF;
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    REQUIRE(slots.write(0, save));
    PlayerSelectScene scene;
    REQUIRE(scene.open(device, context, 0));
    scene.step(1, player(0, true)); // Load, initially highlighted when a save exists.
    scene.step(1, player(0, true));
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    scene.step(SelectLane::kNoticeTicks, nobody());
    REQUIRE(scene.lane(0).pickedClass() == kSumnerClass);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::SaveMenu);
    scene.step(1, player(0, true)); // Loaded characters confirm Done after choosing their class.
    REQUIRE(scene.lane(0).lockedIn());
    REQUIRE(scene.speaking());
    scene.step(200, nobody());
    CHECK(scene.step(1, nobody()) == SelectOutcome::Running);
    std::array<f32, 9600> audio{};
    for (s32 chunk = 0; chunk < 100; ++chunk) {
        mixer.mix(audio);
        sounds.update();
    }
    REQUIRE_FALSE(scene.speaking());
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 frame = 0; frame <= PlayerSelectScene::kIdleFrames; ++frame) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
}

TEST_CASE("the native character preview panel follows highlighted costume colors",
          "[select][select-banner][assets]") {
    // setup_player_display uses player_rgb on S4_<class>; the border and
    // BK_RUNE_STONE_02 strip are separate artwork, not additional tint recipients.
    test::FakeRenderDevice device;
    const Fixture f("select-banner");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 2));
    scene.step(1, player(2, true));
    scene.step(1, player(2, true));
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    REQUIRE(scene.lane(2).state() == SelectLane::State::ClassPick);
    TextureSet reference;
    REQUIRE(reference.load(context.unpackedRoot / "SELECT"));
    const auto projection = makeScreenProjection(640, 448);
    for (s32 character = 0; character < kStartingClassCount; ++character) {
        REQUIRE(scene.lane(2).pickedClass() == character);
        const auto panel = reference.find(std::format("S4_{}", classCode(character)));
        REQUIRE(panel);
        for (s32 color = 0; color < kColorCount; ++color) {
            const auto picked = scene.lane(2).pickedColor();
            CAPTURE(character, picked);
            device.draws.clear();
            scene.render(device, projection, 640, 448);
            const auto found = std::ranges::find_if(device.draws, [&](const auto& draw) {
                const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
                return texture != nullptr && texture->pixels == reference.image(*panel).pixels &&
                       test::minCorner(draw) == Vec2{256, 320} &&
                       test::maxCorner(draw) == Vec2{384, 384};
            });
            REQUIRE(found != device.draws.end());
            CHECK(std::ranges::all_of(found->vertices, [&](const auto& vertex) {
                return vertex.color == boxTint(picked, true);
            }));
            PlayerSelectScene::Inputs up{};
            up[2].up = true;
            scene.step(1, up);
        }
        scene.step(1, player(2, false, false, false, true));
    }
}

TEST_CASE("post-shop prompts surviving lanes and retains fallen character checkpoints",
          "[game][select][post-shop][assets][multiplayer]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop");
    PlayerSelectScene scene;
    CharacterSave alive;
    alive.name = "ALIVE";
    alive.gold = 4321;
    CharacterSave fallen;
    fallen.name = "FALLEN";
    const std::array party{PartyMember{0, alive, std::nullopt, false, 0, {14, 27}},
                           PartyMember{2, alive, 1}, PartyMember{3, fallen, 2, true}};
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    REQUIRE(slots.write(2, fallen));
    const auto context = f.context();
    REQUIRE(scene.openAfterLevel(device, context, party));
    CHECK(scene.lane(0).state() == SelectLane::State::SaveMenu);
    CHECK(scene.lane(2).state() == SelectLane::State::SaveMenu);
    CHECK(scene.lane(3).lockedIn());
    CHECK_FALSE(scene.lane(1).active());
    scene.step(1, player(3, true, true, true, true));
    CHECK(scene.lane(3).lockedIn());
    CHECK(scene.step(60, player(0, false, true)) == SelectOutcome::Running);
    CHECK(scene.lane(0).state() == SelectLane::State::SaveMenu);
    scene.step(1, player(0, true)); // Done; the other survivor still owns its menu.
    CHECK(scene.lane(0).lockedIn());
    auto restart = player(0, false, false, true);
    restart[3].start = true;
    scene.step(1, restart);
    CHECK(scene.lane(0).state() == SelectLane::State::SaveMenu);
    CHECK(scene.lane(3).lockedIn());
    CHECK(scene.step(60, nobody()) == SelectOutcome::Running);
    scene.step(1, player(0, true));
    scene.step(1, player(2, true));
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 i = 0; i <= PlayerSelectScene::kIdleFrames; ++i) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
    const auto result = scene.party();
    REQUIRE(result.size() == 3);
    CHECK(result[0].save.toJson() == alive.toJson());
    CHECK(result[0].helpHeard == std::vector<s32>{14, 27});
    CHECK(result[1].slot == 1);
    CHECK(result[2].fallen);
    CHECK(result[2].slot == 2);
    CHECK(result[2].save.toJson() == fallen.toJson());
    CharacterSave checkpoint;
    REQUIRE(scene.saves().load(2, checkpoint));
    CHECK(checkpoint.toJson() == fallen.toJson());
}

TEST_CASE("post-shop lanes cannot race for another lane's pending save slot",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-reservations");
    PlayerSelectScene scene;
    CharacterSave first;
    first.name = "FIRST";
    CharacterSave second;
    second.name = "SECOND";
    const std::array party{PartyMember{0, first, std::nullopt, false, 0, {14}},
                           PartyMember{1, second}};
    const auto context = f.context();
    REQUIRE(scene.openAfterLevel(device, context, party));
    PlayerSelectScene::Inputs both{};
    both[0].up = both[1].up = true;
    for (s32 i = 0; i < 3; ++i) {
        scene.step(1, both); // Done -> Quit -> Change -> Save; no files to load.
    }
    both = {};
    both[0].select = both[1].select = true;
    scene.step(1, both);
    REQUIRE(scene.lane(0).state() == SelectLane::State::SavePick);
    REQUIRE(scene.lane(1).state() == SelectLane::State::SavePick);
    scene.step(1, both);
    CHECK(scene.lane(0).state() == SelectLane::State::Saving);
    CHECK(scene.lane(1).state() == SelectLane::State::SavePick);
    CHECK(scene.lane(0).reservedSlot() == 0);
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    CharacterSave saved;
    REQUIRE(scene.saves().load(0, saved));
    CHECK(saved.name == "FIRST");
    CHECK(scene.party()[0].slot == 0);
    CHECK(scene.party()[0].helpHeard == std::vector<s32>{14});
    CHECK_FALSE(scene.party()[1].slot.has_value());
    scene.step(1, player(1, true));
    CHECK(scene.lane(1).state() == SelectLane::State::SavePick);
}

TEST_CASE("post-shop keeps Done selected and protects an existing save when overwrite is refused",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-focus");
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    CharacterSave save;
    save.name = "EXIST";
    REQUIRE(slots.write(0, save));
    save.gold = 4567;
    const std::array party{PartyMember{0, save, 0}};
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.openAfterLevel(device, context, party));
    // The GameCube card-directory probe returns at most one, selecting Done.
    PlayerSelectScene::Inputs up{};
    up[0].up = true;
    for (s32 i = 0; i < 4; ++i) {
        scene.step(1, up);
    }
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::SavePick);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::OverwriteConfirm);
    scene.step(1, player(0, true)); // No is the default; leave the old file alone.
    CharacterSave before;
    REQUIRE(slots.load(0, before));
    CHECK(before.gold != save.gold);
    CHECK(scene.party()[0].save.gold == save.gold);
}

TEST_CASE("post-shop snapshots follow a loaded character rather than the pending journey",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-load");
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    CharacterSave saved;
    saved.name = "SAME";
    saved.gold = 250;
    REQUIRE(slots.write(0, saved));
    CharacterSave live = saved;
    live.gold = 600;
    const std::array party{PartyMember{0, live, 0, false, 0, {14}}};
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.openAfterLevel(device, context, party));
    PlayerSelectScene::Inputs up{};
    up[0].up = true;
    scene.step(1, up); // Quit
    scene.step(1, up); // Load
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::LoadPick);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::Loading);
    CHECK(scene.party()[0].save.gold == 600); // No speculative replacement.
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    REQUIRE(scene.party().size() == 1);
    CHECK(scene.party()[0].save.gold == 250);
    CHECK(scene.party()[0].helpHeard.empty()); // Loading resets even an identical name/class.
    scene.step(SelectLane::kNoticeTicks, nobody());
    REQUIRE(scene.lane(0).state() == SelectLane::State::ClassPick);
    scene.step(1, player(0, false, false, false, true));
    CHECK(scene.lane(0).pickedClass() == 1);
    CHECK(scene.party()[0].save.character == 0); // Browsing has not committed the choice.
}

} // namespace
