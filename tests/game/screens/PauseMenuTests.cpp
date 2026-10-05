#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PauseMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("resuming waits for the native acid scroll and blocks all menu input during it",
          "[pause][pause-dismiss][assets]") {
    const bool useStart = GENERATE(false, true);
    test::FakeRenderDevice device;
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    TextureSet reference;
    REQUIRE(reference.load(context.unpackedRoot / "STATIC"));
    const auto ring = reference.find("GREENCIRCTRANS");
    REQUIRE(ring);
    REQUIRE(*ring + 21 < reference.size());
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    const Rect area = menu.menu().backdropArea();
    MenuInput leave;
    leave.start = useStart;
    leave.back = !useStart;
    // OptionsDone (80070BDC) holds play until ServeFireScroll's 21 frames, two ticks each.
    REQUIRE(menu.update(0, leave) == PauseOutcome::Running);
    CHECK(menu.isOpen());
    CHECK_FALSE(menu.musicAudible());
    CHECK(menu.menu().backdropReleased());
    CHECK(menu.menu().closing());
    MenuInput ignored;
    ignored.select = true;
    ignored.down = true;
    ignored.back = true;
    ignored.start = true;
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    for (u32 tick = 0; tick < 42; ++tick) {
        CAPTURE(tick);
        device.draws.clear();
        menu.render(device, projection, 640, 448);
        const auto& expected = reference.image(*ring + 1 + tick / 2);
        const auto found = std::ranges::find_if(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels == expected.pixels &&
                   test::minCorner(draw) == Vec2{area.x, area.y} &&
                   test::maxCorner(draw) == Vec2{area.right(), area.bottom()};
        });
        REQUIRE(found != device.draws.end());
        CHECK(menu.menu().selection() == 0);
        const auto outcome = menu.update(1.0 / 60, ignored);
        CHECK(outcome == (tick == 41 ? PauseOutcome::Resume : PauseOutcome::Running));
    }
    CHECK(device.textureUpdates == 21);
    menu.close();
    REQUIRE(menu.open(device, context, party, 0));
    CHECK_FALSE(menu.menu().backdropReleased());
    CHECK_FALSE(menu.menu().closing());
    CHECK(menu.update(0, {}) == PauseOutcome::Running);
    // Teardown is also legal before the wipe ends; reopened menus must not retain its borrows.
    REQUIRE(menu.update(0, leave) == PauseOutcome::Running);
    menu.render(device, projection, 640, 448);
    const auto uploads = device.textureUpdates;
    menu.close();
    REQUIRE(menu.open(device, context, party, 0));
    MenuInput down;
    down.down = true;
    menu.update(0, down);
    CHECK(menu.menu().selection() == 1);
    menu.render(device, projection, 640, 448);
    CHECK(device.textureUpdates == uploads);
}

TEST_CASE("pause dismissal starts its native scroll sound once despite further input",
          "[pause][pause-dismiss][assets]") {
    test::assetOrSkip("audio/COMMON.vbk");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    CHECK(sounds.voiceCount() == 1);
    MenuInput back;
    back.back = true;
    CHECK(menu.update(0, back) == PauseOutcome::Running);
    // AudioMenuExit and StartFireScroll's handle19 (S_OPTMENUSCROLL), once each.
    CHECK(sounds.voiceCount() == 3);
    CHECK(menu.update(0.5, back) == PauseOutcome::Running);
    CHECK(sounds.voiceCount() == 3);
    CHECK(menu.update(0.2, back) == PauseOutcome::Resume);
    CHECK(sounds.voiceCount() == 3);
}

TEST_CASE("pause acid dismissal retains fractional ticks at different update rates",
          "[pause][pause-dismiss][assets]") {
    const s32 rate = GENERATE(30, 60, 144, 240);
    test::FakeRenderDevice device;
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    MenuInput back;
    back.back = true;
    REQUIRE(menu.update(0, back) == PauseOutcome::Running);
    const s32 updates = rate * 7 / 10;
    for (s32 step = 1; step < updates; ++step) {
        CHECK(menu.update(1.0 / rate, {}) == PauseOutcome::Running);
    }
    CHECK(menu.update(0.7 - static_cast<f64>(updates - 1) / rate, {}) == PauseOutcome::Resume);
}

TEST_CASE("pause reopening or destruction stops its borrowed bank voices but not other audio",
          "[pause][pause-dismiss][pause-audio-lifetime][assets]") {
    test::assetOrSkip("audio/COMMON.vbk");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const SoundClip unrelatedClip{48000, 1, std::vector<f32>(480, 0.01f)};
    SoundSequence unrelatedSequence;
    unrelatedSequence.steps.push_back({&unrelatedClip, true, true});
    const auto unrelated = sounds.play(unrelatedSequence);
    REQUIRE(sounds.isPlaying(unrelated));
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const std::array party{PartyMember{}};
    std::array<f32, 960> audio{};
    const auto audioStep = [&] {
        mixer.mix(audio); // Ten milliseconds drains the owned voices' stop ramps.
        sounds.update();
    };
    {
        PauseMenu menu;
        REQUIRE(menu.open(device, context, party, 0));
        MenuInput back;
        back.back = true;
        REQUIRE(menu.update(0, back) == PauseOutcome::Running);
        REQUIRE(sounds.voiceCount() == 4); // External loop, open, exit and acid-scroll cues.
        REQUIRE(menu.open(device, context, party, 0)); // Reloads COMMON while the cues are live.
        audioStep();
        CHECK(sounds.isPlaying(unrelated));
        CHECK(sounds.voiceCount() == 2); // Only the new opening cue and unrelated loop survive.
        REQUIRE(menu.update(0, back) == PauseOutcome::Running);
        SECTION("explicit close releases borrowed voices before another bank reload") {
            menu.close();
            audioStep();
            CHECK(sounds.voiceCount() == 1);
            REQUIRE(menu.open(device, context, party, 0));
        }
        SECTION("destruction may also interrupt an active dismissal") {}
    }
    audioStep();
    CHECK(sounds.voiceCount() == 1);
    CHECK(sounds.isPlaying(unrelated));
    // Keep feeding beyond the sound player's lookahead, after PauseMenu's bank is destroyed.
    for (s32 step = 0; step < 200; ++step) {
        audioStep();
    }
    CHECK(sounds.voiceCount() == 1);
    CHECK(sounds.isPlaying(unrelated));
    CHECK(std::ranges::all_of(audio, [](f32 value) { return value == 0.01f; }));
}

TEST_CASE("pause draws the five native rune seal frames on the menus that request them",
          "[pause][pause-seal][assets]") {
    test::FakeRenderDevice device;
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    TextureSet reference;
    REQUIRE(reference.load(context.unpackedRoot / "STATIC"));
    const auto seal = reference.find("LOGO_BURN1");
    REQUIRE(seal);
    REQUIRE(*seal + 5 <= reference.size());
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    // OPTMENU_TOWER/INGAME (8011E218/8011E4E4) share the native seal rectangle.
    // show_optmenu advances its five consecutive texture entries every eight menu ticks.
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    const auto canvasProjection = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    const auto isSeal = [](const test::RecordedDraw& draw) {
        return test::minCorner(draw) == Vec2{290, 142} && test::maxCorner(draw) == Vec2{514, 314};
    };
    for (u32 tick = 0; tick <= 40; ++tick) {
        CAPTURE(tick);
        device.draws.clear();
        menu.render(device, projection, 640, 448);
        const auto found = std::ranges::find_if(device.draws, isSeal);
        REQUIRE(found != device.draws.end());
        REQUIRE(found->texture);
        const auto& expected = reference.image(*seal + (tick / 8) % 5);
        const auto* actual = dynamic_cast<const test::FakeTexture*>(found->texture);
        REQUIRE(actual);
        CHECK(actual->pixels == expected.pixels);
        CHECK(found->transform == canvasProjection);
        if (tick != 40) {
            menu.update(1.0 / 60, {});
        }
    }
    MenuInput select;
    select.select = true;
    MenuInput back;
    back.back = true;
    MenuInput down;
    down.down = true;
    menu.update(1.0 / 60, select); // Settings records 8011E390/8011E638 also carry the seal.
    device.draws.clear();
    menu.render(device, projection, 640, 448);
    CHECK(std::ranges::any_of(device.draws, isSeal));
    menu.update(1.0 / 60, select); // Audio's record 8011E9C8 has no seal.
    device.draws.clear();
    menu.render(device, projection, 640, 448);
    CHECK(std::ranges::none_of(device.draws, isSeal));
    menu.update(1.0 / 60, back);
    menu.update(1.0 / 60, down); // Compass, native record 8011EF60, retains the seal.
    menu.update(1.0 / 60, select);
    device.draws.clear();
    menu.render(device, projection, 640, 448);
    CHECK(std::ranges::any_of(device.draws, isSeal));
    menu.update(1.0 / 60, back);
    menu.update(1.0 / 60, back);
    CHECK_FALSE(menu.menu().backdropReleased());
    CHECK_FALSE(menu.menu().closing());
    for (s32 row = 0; row < 4; ++row) {
        menu.update(1.0 / 60, down);
    }
    menu.update(1.0 / 60, select); // The smaller Quit Game dialog has no seal.
    device.draws.clear();
    menu.render(device, projection, 640, 448);
    CHECK(std::ranges::none_of(device.draws, isSeal));
}

TEST_CASE("pause loads the native axe and plays navigation and timed volume samples",
          "[pause][audio-samples][assets]") {
    test::assetOrSkip("audio/COMMON.vbk");
    test::assetOrSkip("POWERUPS/objects.ngc");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.sounds = &sounds;
    context.saveSettings = [](const GameConfig&) { return true; };
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    REQUIRE(menu.arrowBound());
    CHECK(sounds.voiceCount() == 1); // opening select
    MenuInput choose;
    choose.select = true;
    menu.update(1.0 / 60, choose); // Settings
    menu.update(1.0 / 60, choose); // Audio
    CHECK(sounds.voiceCount() == 3);
    MenuInput down;
    down.down = true;
    menu.update(1.0 / 60, down); // Effects
    CHECK(sounds.voiceCount() == 4);
    menu.update(1.0, {});
    CHECK(sounds.voiceCount() == 5); // heartbeat preview
    MenuInput right;
    right.rightHeld = true;
    menu.update(0.1, right);
    CHECK(sounds.voiceCount() == 5); // changing volume does not click every frame
    MenuInput back;
    back.back = true;
    menu.update(1.0 / 60, back);
    CHECK(sounds.voiceCount() == 6);
    menu.close();
    CHECK_FALSE(menu.arrowBound());
}

TEST_CASE("pause mouse targets work after letterboxing for controller-owned pauses",
          "[pause][mouse][assets]") {
    test::FakeRenderDevice device;
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    std::array party{PartyMember{}};
    party[0].player = 2;
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 2));
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    menu.render(device, projection, 640, 448);
    const auto area = menu.menu().itemArea(2); // Shop, a real tower action.
    const auto transform = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    const auto clip = transform * Vec4{area.x + 1, area.y + 1, 0.5f, 1};
    MenuInput click;
    click.pointer = (Vec2{clip} + Vec2{1}) / 2.0f;
    click.pointerNormalized = true;
    click.pointerPressed = true;
    CHECK(menu.update(1.0 / 60, click) == PauseOutcome::Shop);
    MenuInput back;
    back.pointerBack = true;
    CHECK(menu.update(1.0 / 60, back) == PauseOutcome::Running);
    CHECK(menu.update(0.7, {}) == PauseOutcome::Resume);
}

TEST_CASE("pause menu refuses absent artwork or a player outside the party", "[pause]") {
    test::FakeRenderDevice device;
    PauseMenu menu;
    GameContext context;
    context.unpackedRoot = test::scratchDirectory("pause-empty");
    const std::array party{PartyMember{}};
    CHECK_FALSE(menu.open(device, context, party, 3));
    CHECK_FALSE(menu.open(device, context, party, 0));
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("pause menu time retains fractions across thirty sixty and uncapped update rates",
          "[pause][graphics][assets]") {
    test::FakeRenderDevice device;
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const std::array party{PartyMember{}};
    PauseMenu menu;
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        REQUIRE(menu.open(device, context, party, 0));
        for (s32 frame = 0; frame < rate; ++frame) {
            menu.update(1.0 / rate, {});
        }
        CHECK(menu.menu().time() == 60);
        menu.update(0.0, {});
        CHECK(menu.menu().time() == 60);
    }
    menu.close();
}
TEST_CASE("pause menus route character management and preserve the live party", "[pause][assets]") {
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("pause-files").string();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    bool settingsSaved = false;
    context.saveSettings = [&](const GameConfig& next) {
        config = next;
        settingsSaved = true;
        return true;
    };
    std::array party{PartyMember{}};
    party[0].player = 2;
    party[0].save.name = "PAUSE";
    party[0].save.gold = 123;
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 2));
    CHECK(menu.player() == 2);
    CHECK(menu.menu().definition().parchmentFont);
    CHECK_FALSE(menu.menu().definition().prompts);
    CHECK(menu.menu().definition().backLabel.empty());
    CHECK(menu.menu().definition().selectLabel.empty());
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    select.start = true; // Enter is both menu-select and menu-start by default.
    MenuInput back;
    back.back = true;
    const auto step = [&](const MenuInput& input) { return menu.update(1.0 / 60, input); };
    menu.render(device, makeLetterboxProjection(640, 448, 2560, 1080), 640, 448);
    CHECK_FALSE(device.draws.empty());
    REQUIRE(device.draws.front().transform == Mat4{1});
    CHECK(test::minCorner(device.draws.front()) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws.front()) == Vec2{1, 1});
    CHECK(device.draws.front().vertices.front().color == Color::rgba(0, 0, 0, 150));
    SECTION("resume waits for dismissal") {
        CHECK(step(back) == PauseOutcome::Running);
        CHECK(menu.update(0.7, {}) == PauseOutcome::Resume);
    }
    SECTION("quit confirmation") {
        for (s32 i = 0; i < 4; ++i) {
            step(down);
        }
        step(select);
        // The retail dialog (0x8011EB1C) is the abort dialog's parchment with its own
        // title: No first, Yes, and nothing else on it.
        const auto& dialog = menu.menu().definition();
        CHECK(dialog.title == "Quit Game?");
        REQUIRE(dialog.items.size() == 2);
        CHECK(dialog.items[0].text == "No");
        CHECK(dialog.items[1].text == "Yes");
        CHECK(dialog.body.empty());
        CHECK(dialog.playerLabel.empty());
        CHECK_FALSE(dialog.prompts);
        CHECK(dialog.parchmentFont);
        CHECK(dialog.fades);
        CHECK(dialog.x == -256);
        CHECK(dialog.backdropY == 64);
        CHECK(dialog.backdropWidth == 320);
        CHECK(dialog.backdropHeight == 220);
        menu.update(1, {});
        CHECK(step(select) == PauseOutcome::Running); // default No
        for (s32 i = 0; i < 4; ++i) {
            step(down);
        }
        step(select);
        menu.update(1, {});
        step(down);
        CHECK(step(select) == PauseOutcome::Running);
        CHECK(menu.update(0.7, {}) == PauseOutcome::Title);
    }
    SECTION("shop and inventory") {
        // OPTMENU_TOWER: OPT_SHOP opens init_shop(1), OPT_INVENTORY init_shop(2).
        step(down);
        step(down);
        CHECK(step(select) == PauseOutcome::Shop);
        CHECK_FALSE(menu.menu().backdropReleased());
        step(down);
        CHECK(step(select) == PauseOutcome::Inventory);
        CHECK_FALSE(menu.menu().backdropReleased());
    }
    SECTION("music under the menu") {
        // options.c 813 ducks the music every frame the menu is up; 984 lets it play on
        // the Audio page, where its slider is set.
        CHECK_FALSE(menu.musicAudible());
        step(select);
        CHECK_FALSE(menu.musicAudible());
        step(select); // settings -> audio
        CHECK(menu.musicAudible());
        step(back);
        CHECK_FALSE(menu.musicAudible());
        step(back);
        CHECK(step(back) == PauseOutcome::Running);
        CHECK(menu.update(0.7, {}) == PauseOutcome::Resume);
    }
    SECTION("shared settings") {
        step(select);
        step(select); // settings -> audio
        MenuInput left;
        left.left = true;
        step(left);
        step({});
        REQUIRE(settingsSaved);
        CHECK(AudioSlider::value(config.audio.musicVolume) == 127);
        step(back);
        step(back);
        CHECK(step(back) == PauseOutcome::Running);
        CHECK(menu.update(0.7, {}) == PauseOutcome::Resume);
    }
    SECTION("character management returns to party selection") {
        step(down);
        CHECK(step(select) == PauseOutcome::Manage);
        CHECK_FALSE(menu.menu().backdropReleased());
        REQUIRE_FALSE(menu.party()[0].slot.has_value());
        CHECK_FALSE(party[0].slot.has_value());
        CHECK(menu.party()[0].save.gold == 123);
    }
    menu.close();
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("level abort uses the retail parchment dialog without character-file warnings",
          "[pause][assets]") {
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameContext context;
    context.tower = &world;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    menu.update(1.0 / 60, down);
    menu.update(1.0 / 60, select);
    const auto& definition = menu.menu().definition();
    CHECK(definition.title == "Abort Level?");
    CHECK(definition.items[0].text == "No");
    CHECK(definition.items[1].text == "Yes");
    CHECK(definition.body.empty());
    CHECK(definition.playerLabel.empty());
    CHECK_FALSE(definition.prompts);
    CHECK(definition.parchmentFont);
    CHECK(definition.colors.off.r == 92);
    CHECK(definition.colors.off.g == 26);
    CHECK(definition.colors.off.b == 3);
    CHECK(definition.backdropY == 64);
    CHECK(definition.backdropWidth == 320);
    CHECK(definition.backdropHeight == 220);
    menu.update(1, {});
    menu.update(1.0 / 60, down);
    const auto outcome = menu.update(1.0 / 60, select);
    CHECK(
        (outcome == PauseOutcome::ReturnTower || menu.update(1, {}) == PauseOutcome::ReturnTower));
}

TEST_CASE("the secret world cannot be quit from its menu", "[pause][assets]") {
    // options.c 1462: OPT_QUITLEVEL's value is -1 (greyed) while sMusicTrackHi is 12.
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    // A level of the secret realm that is not unpacked: the world keeps the reference.
    LevelRef secret;
    secret.realm = "SECRET";
    secret.realmId = LevelRef::kSecretRealm;
    secret.name = "S1";
    secret.directory = "LEVELS/LEVELS1-absent";
    LevelWorld world;
    REQUIRE_FALSE(world.load(device, root, secret));
    REQUIRE(world.ref().isSecret());
    GameContext context;
    context.tower = &world;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    const auto& items = menu.menu().definition().items;
    REQUIRE(items.size() == 2);
    CHECK(items[0].text == strings.get("pause.settings"));
    CHECK(items[0].enabled);
    CHECK(items[1].text == strings.get("pause.quitLevel"));
    CHECK_FALSE(items[1].enabled);
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    menu.update(1.0 / 60, down);
    CHECK(menu.update(1.0 / 60, select) == PauseOutcome::Running);
    CHECK(menu.menu().definition().items.size() == 2); // still the main page
}
} // namespace
