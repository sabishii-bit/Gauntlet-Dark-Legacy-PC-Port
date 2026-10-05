#include <algorithm>
#include <array>
#include <format>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/MenuInput.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/ShopFrameLight.h"
#include "game/screens/ShopLayout.h"
#include "game/screens/ShopMusic.h"
namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("retail frame sweep travels across corners every 150 ticks", "[shop][render]") {
    CHECK(shopFrameLight(0, 0, 0) == 128);
    CHECK(shopFrameLight(0, 0, 2.5 / 7) == 255);
    CHECK(shopFrameLight(448, 0, 2.5 * 3 / 7) == 255);
    CHECK(shopFrameLight(512, 384, 2.5 * 6 / 7) == 255);
    for (s32 tick = 0; tick < 150; ++tick) {
        for (s32 x = 0; x <= 512; x += 128) {
            CAPTURE(tick, x);
            CHECK(shopFrameLight(x, 384, tick / 60.0) >= 128);
            CHECK(shopFrameLight(x, 384, tick / 60.0) == shopFrameLight(x, 384, tick / 60.0 + 2.5));
        }
    }
    CHECK(DrawState{}.colorScale == 1);
}

TEST_CASE("only the end of H4 selects Final Stats in the production screen", "[shop][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    const std::array<PartyMember, 1> party{{{0, CharacterSave{}}}};
    AfterLevelScene scene;
    for (const auto* level : {"G5", "E2", "F2", "H3", "H4"}) {
        REQUIRE(scene.open(device, context, party, {}, {}, level));
        CHECK(scene.session().visit() ==
              (std::string_view(level) == "H4" ? ShopVisit::Completion : ShopVisit::Level));
    }
    REQUIRE(scene.open(device, context, party, {}, {}, "H4", ShopVisit::Shop));
    CHECK(scene.session().visit() == ShopVisit::Shop);
    scene.render(device, makeLetterboxProjection(640, 448, 1920, 1080), 640, 448);
    REQUIRE_FALSE(device.draws.empty());
    const auto& background = device.draws.front();
    CHECK(background.transform == Mat4{1});
    CHECK(test::minCorner(background) == Vec2{-1, -1});
    CHECK(test::maxCorner(background) == Vec2{1, 1});
    CHECK(background.vertices.front().color == Color::black());
}
TEST_CASE("shop music follows the departed realm including non-gameplay fallbacks",
          "[shop][screens][audio]") {
    // GUNE5D ShopMusicStart 800a0da8 and LevelLetter 80057a6c.
    for (char realm = 'A'; realm <= 'K'; ++realm) {
        CHECK(shopMusicRealm(std::format("{}1", realm)) == realm);
    }
    CHECK(shopMusicRealm("L1") == 'A');
    CHECK(shopMusicRealm("L2") == 'A');
    CHECK(shopMusicRealm("S1") == 'A');
    CHECK(shopMusicRealm("T1") == 'G');
    CHECK(shopMusicRealm("") == 'A');
    CHECK(shopMusicRealm("?") == 'A');
}

TEST_CASE("Final Stats renders retail captions and staggered lifetime totals",
          "[shop][final-stats][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    CharacterSave save;
    save.progress().lifetime = {12345, 678, 54321, 93780};
    const std::array<PartyMember, 1> party{{{0, save}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "H4", ShopVisit::FinalStats));
    BitmapFont font;
    REQUIRE(font.load(root / "FONTS/font32.fnt", 16));
    TextureSet art;
    REQUIRE(art.load(root / "STATIC"));
    const auto fontId = art.find("FONT32");
    REQUIRE(fontId);
    const auto& pixels = art.image(*fontId).pixels;
    const auto hasText = [&](s32 y, std::string_view value, f32 scale = 0.48f) {
        test::FakeRenderDevice expected;
        Canvas canvas;
        TextPainter painter;
        painter.setFont(&font, &art.texture(expected, *fontId));
        canvas.begin(expected, Mat4{1});
        painter.draw(canvas, -64, y, value, TextStyle{scale, Color::white()});
        canvas.end();
        const auto& glyphs = expected.draws.front().vertices;
        return std::ranges::any_of(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels == pixels &&
                   !std::ranges::search(draw.vertices, glyphs).empty();
        });
    };
    const auto render = [&] {
        device.draws.clear();
        scene.render(device, Mat4{1}, 512, 384);
    };
    render();
    CHECK(hasText(32, "Final Stats", 0.56f));
    CHECK(hasText(60, "Enemies Killed"));
    CHECK(hasText(98, "Generators"));
    CHECK(hasText(116, "Destroyed"));
    CHECK(hasText(154, "Gold Found"));
    CHECK(hasText(192, "Total Playtime"));
    CHECK_FALSE(hasText(78, "12345"));
    scene.update(1.5, {});
    render();
    CHECK_FALSE(hasText(78, "12345"));
    scene.update(1.0 / 60, {});
    render();
    CHECK(hasText(78, "12345"));
    CHECK_FALSE(hasText(134, "678"));
    scene.update(1, {});
    render();
    CHECK(hasText(134, "678"));
    CHECK_FALSE(hasText(172, "54321"));
    scene.update(1, {});
    render();
    CHECK(hasText(172, "54321"));
    CHECK_FALSE(hasText(210, "1 Days"));
    scene.update(1, {});
    render();
    CHECK(hasText(210, "1 Days")); // authored format does not singularize
    CHECK(hasText(228, "2 Hours"));
    CHECK(hasText(246, "3 Minutes"));
    CHECK_FALSE(scene.session().lanes()[0].finalStatsReady());
    scene.update(1, {});
    CHECK(scene.session().lanes()[0].finalStatsReady());
}

TEST_CASE("frame lighting varies at corners without fading alpha or illuminating other UI",
          "[shop][render][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    const std::array<PartyMember, 1> party{{{0, CharacterSave{}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
    scene.update(1, {});
    scene.render(device, Mat4{1}, 512, 384);
    usize frames = 0;
    bool varied = false;
    for (const auto& draw : device.draws) {
        if (draw.state.colorScale != 2) {
            CHECK(draw.state.colorScale == 1);
            continue;
        }
        ++frames;
        for (const auto& vertex : draw.vertices) {
            CHECK(vertex.color.a == 255);
            CHECK(vertex.position.z == 0.5f);
            CHECK(vertex.color.r == shopFrameLight(static_cast<s32>(vertex.position.x),
                                                   384 - static_cast<s32>(vertex.position.y), 1));
            varied |= vertex.color != draw.vertices.front().color;
        }
    }
    CHECK(frames == 8);
    CHECK(varied);
}

TEST_CASE("shop plays the authored music through realm changes without retaining the old theme",
          "[shop][screens][audio][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    for (char realm = 'A'; realm <= 'K'; ++realm) {
        test::assetOrSkip(std::format("audio/SHOP_{}.vbk", realm));
    }
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.unpackedRoot = root;
    context.sounds = &sounds;
    const std::array<PartyMember, 1> party{{{0, CharacterSave{}}}};
    AfterLevelScene scene;
    // E2 and F2 are Skorne's two arenas. Returning to G or opening the tower
    // shop must not latch the last boss realm's theme for the rest of the save.
    constexpr std::array<std::pair<std::string_view, char>, 17> kCases{{
        {"A1", 'A'},
        {"B1", 'B'},
        {"C1", 'C'},
        {"D1", 'D'},
        {"E2", 'E'},
        {"G1", 'G'},
        {"F2", 'F'},
        {"G1", 'G'},
        {"H1", 'H'},
        {"I1", 'I'},
        {"J1", 'J'},
        {"K1", 'K'},
        {"L1", 'A'},
        {"L2", 'A'},
        {"S1", 'A'},
        {"T1", 'G'},
        {"", 'A'},
    }};
    for (const auto& [level, realm] : kCases) {
        CAPTURE(level, realm);
        REQUIRE(scene.open(device, context, party, {}, {}, level,
                           level.starts_with('L') ? ShopVisit::Shop : ShopVisit::Level));
        SoundSet bank;
        REQUIRE(bank.load(root / "audio" / std::format("SHOP_{}", realm)));
        const auto cue = bank.find(std::format("S_SHOP_{}", realm));
        REQUIRE(cue.has_value());
        AudioMixer expectedMixer(48000);
        SoundPlayer expectedSound(expectedMixer);
        expectedSound.play(bank.sequence(*cue), 1, SoundCategory::Music);
        std::vector<f32> actual(9600);
        std::vector<f32> expected(9600);
        // Allow the previous voice's short stop ramp to drain before comparing.
        mixer.mix(actual);
        expectedMixer.mix(expected);
        sounds.update();
        REQUIRE(sounds.voiceCount() == 1);
        mixer.mix(actual);
        expectedMixer.mix(expected);
        REQUIRE(actual == expected);
        REQUIRE(std::ranges::any_of(actual, [](f32 value) { return value != 0; }));
    }
    scene.close();
    std::vector<f32> tail(9600);
    mixer.mix(tail);
    sounds.update();
    CHECK(sounds.voiceCount() == 0);
}

TEST_CASE("after-level screen fails safely without its portable catalog", "[shop][screens]") {
    test::FakeRenderDevice device;
    AfterLevelScene scene;
    GameContext context;
    context.unpackedRoot = test::scratchDirectory("shop-screen-empty");
    REQUIRE_FALSE(scene.open(device, context, {}, {}, {}, "G1"));
    REQUIRE_FALSE(scene.isOpen());
    scene.close();
    scene.close();
}
TEST_CASE("shop lane borders retain retail brightness and opacity in every column",
          "[shop][screens][assets]") {
    // init_shop 8009A504/8009A52C sets 0x80808080; DrawBlit 800B47D0 doubles
    // alpha and clamps it to 255. The TEV color scale at 80067D98 is GX_CS_SCALE_2.
    // Copying those raw bytes into our normalized vertex colors fades/darkens twice.
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("INVENTORY/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    const auto visit = GENERATE(ShopVisit::Level, ShopVisit::Shop, ShopVisit::Inventory);
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    const std::array<PartyMember, 1> party{{{2, CharacterSave{}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", visit));
    scene.render(device, Mat4{1}, 512, 384);
    TextureSet art;
    REQUIRE(art.load(root / "SELECT"));
    for (const auto* name : {"S1_BORDER", "S2_BORDER"}) {
        CAPTURE(name, visit);
        const auto id = art.find(name);
        REQUIRE(id.has_value());
        const auto& pixels = art.image(*id).pixels;
        std::array<bool, 4> columns{};
        for (const auto& draw : device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            if (texture == nullptr || texture->pixels != pixels) {
                continue;
            }
            const usize column = static_cast<usize>(test::minCorner(draw).x / 128);
            REQUIRE(column < columns.size());
            REQUIRE_FALSE(columns[column]);
            columns[column] = true;
            REQUIRE_FALSE(draw.vertices.empty());
            for (const auto& vertex : draw.vertices) {
                REQUIRE(vertex.color == Color::rgba(128, 128, 128, 255));
                REQUIRE(vertex.position.z == 0.5f);
            }
            REQUIRE(draw.state.colorScale == 2);
        }
        REQUIRE(std::ranges::all_of(columns, [](bool drawn) { return drawn; }));
    }
}

TEST_CASE("shop header uses the retail caption scale and native marquee extent",
          "[shop][screens][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    std::array<PartyMember, 4> party;
    for (usize i = 0; i < party.size(); ++i) {
        party[i].player = static_cast<s32>(i);
        party[i].save.color = static_cast<s32>(i);
    }
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
    scene.render(device, Mat4{1}, 512, 384);
    TextureSet art;
    REQUIRE(art.load(root / "SELECT"));
    BitmapFont font;
    REQUIRE(font.load(root / "FONTS/font32.fnt", 16));
    TextureSet fontArt;
    REQUIRE(fontArt.load(root / "STATIC"));
    const auto fontId = fontArt.find("FONT32");
    REQUIRE(fontId.has_value());
    const auto& fontPixels = fontArt.image(*fontId).pixels;
    for (s32 player = 0; player < 4; ++player) {
        CAPTURE(player);
        const auto capId = art.find(std::format("SHOP_TOP_{}", colorCode(player)));
        REQUIRE(capId.has_value());
        REQUIRE_FALSE(art.entry(*capId).halfResolution);
        usize capCount = 0;
        for (const auto& draw : device.draws) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            if (texture != nullptr && texture->pixels == art.image(*capId).pixels) {
                ++capCount;
                REQUIRE(test::minCorner(draw) == Vec2{player * 128 + 32, 0});
                REQUIRE(test::maxCorner(draw) == Vec2{player * 128 + 96, 32});
            }
        }
        REQUIRE(capCount == 1);
        test::FakeRenderDevice expected;
        Canvas canvas;
        TextPainter painter;
        painter.setFont(&font, &fontArt.texture(expected, *fontId));
        canvas.begin(expected, Mat4{1});
        painter.draw(canvas, -(player * 128 + 64), 8, "Shop", TextStyle{0.45f, Color::black()});
        canvas.end();
        REQUIRE(expected.draws.size() == 1);
        const auto& glyphs = expected.draws.front().vertices;
        REQUIRE(glyphs.size() == 24); // four glyph quads, not just an unrelated 'S'
        REQUIRE(std::ranges::any_of(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels == fontPixels &&
                   !std::ranges::search(draw.vertices, glyphs).empty();
        }));
    }
}

TEST_CASE("shop ignores transaction presses during scrolling without queuing a later purchase",
          "[shop][screens][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    CharacterSave save;
    save.gold = 5000;
    save.progress().inventory.addKeys(1);
    save.progress().health = 100; // keep both food rows available while navigating
    const std::array<PartyMember, 1> party{{{0, save}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
    ShopSession::Inputs input;
    input[0].down = true;
    for (s32 i = 0; i < 7; ++i) {
        scene.update(0, input); // fire amulet: its target requires a real scroll
    }
    REQUIRE(scene.session().lanes()[0].cursor == 7);
    input = {};
    input[0].select = true;
    scene.update(0, input);
    REQUIRE_FALSE(scene.session().lanes()[0].transacted);
    REQUIRE(scene.session().party()[0].save.gold == 5000);
    scene.update(10, {});
    REQUIRE(scene.session().party()[0].save.gold == 5000);
    scene.update(0, input);
    REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Bought);
    REQUIRE(scene.session().party()[0].save.gold == 4650);
    input = {};
    input[0].up = true;
    for (s32 i = 0; i < 5; ++i) {
        scene.update(0, input); // owned key, but the list has not moved back yet
    }
    REQUIRE(scene.session().lanes()[0].cursor == 2);
    input = {};
    input[0].back = true;
    scene.update(0, input);
    REQUIRE_FALSE(scene.session().lanes()[0].transacted);
    REQUIRE(scene.session().party()[0].save.progress().inventory.keys == 1);
    scene.update(10, {});
    scene.update(0, input);
    REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Sold);
    REQUIRE(scene.session().party()[0].save.gold == 4725);
}

TEST_CASE("shop commands use separate sell and exit bindings and retain the retail scroll lock",
          "[shop][shop-commands][input][assets][multiplayer]") {
    const bool keyboard = GENERATE(false, true);
    const bool remapped = GENERATE(false, true);
    CAPTURE(keyboard, remapped);
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    if (remapped) {
        config.mergeJson(R"({"controls":{"players":[{},{},{"bindings":{
            "menuSelect":{"keys":["P"],"buttons":["RightBumper"]},
            "shopSell":{"keys":["K"],"buttons":["LeftBumper"]},
            "shopExit":{"keys":["L"],"buttons":["RightTrigger"]}
        }},{}]}})");
    }
    if (keyboard) {
        config.controls[2].device = "keyboard";
    }
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    CharacterSave save;
    save.gold = 5000;
    save.progress().health = 100;
    const std::array party{PartyMember{2, save}, PartyMember{3, save}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
    const auto press = [&](Key key, PadButton button) {
        Input raw;
        if (keyboard) {
            raw.setKey(key, true);
        } else {
            PadSnapshot pad;
            pad.connected = true;
            pad.buttons[static_cast<usize>(button)] = true;
            if (button == PadButton::RightTrigger) {
                // Input derives virtual trigger presses from their analog axes.
                pad.axes[static_cast<usize>(PadAxis::RightTrigger)] = 1.0f;
            }
            raw.setPad(2, pad);
            REQUIRE(raw.wasPadButtonPressed(2, button));
        }
        ShopSession::Inputs inputs{};
        for (s32 player = 0; player < 4; ++player) {
            inputs[static_cast<usize>(player)] = readPlayerMenuInput(raw, config, player);
        }
        return scene.update(0, inputs);
    };
    press(Key::Down, PadButton::DpadDown);
    press(Key::Down, PadButton::DpadDown);
    REQUIRE(scene.session().lanes()[0].cursor == 2);
    REQUIRE(scene.session().catalog().items()[2].type == 1); // Native key row.
    const auto buy = [&] {
        press(remapped ? Key::P : Key::Enter, remapped ? PadButton::RightBumper : PadButton::A);
    };
    buy();
    CHECK_FALSE(scene.session().lanes()[0].transacted);
    CHECK(scene.session().lanes()[0].cursor == 2);
    CHECK(scene.session().party()[0].save.gold == 5000);
    scene.update(2, {});
    // Enter is both Select and Start by default; Select owns a transaction.
    buy();
    REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Bought);
    CHECK(scene.session().party()[0].save.progress().inventory.keys == 1);
    CHECK(scene.session().party()[0].save.gold == 4900);
    press(remapped ? Key::K : Key::Backspace, remapped ? PadButton::LeftBumper : PadButton::X);
    REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Sold);
    CHECK(scene.session().party()[0].save.progress().inventory.keys == 0);
    CHECK(scene.session().party()[0].save.gold == 4975);
    press(remapped ? Key::L : Key::Escape, remapped ? PadButton::RightTrigger : PadButton::B);
    CHECK(scene.session().lanes()[0].cursor == 0);
    CHECK(scene.session().lanes()[0].scrollJump);
    CHECK(scene.session().lanes()[0].phase == ShopPhase::Shopping);
    CHECK(scene.session().party()[1].save.gold == 5000);
    CHECK(scene.session().party()[1].save.progress().inventory.keys == 0);
    CHECK(scene.session().lanes()[1].cursor == 0);
}

TEST_CASE("shop mouse targets rendered rows prices arrows and wheel in only the hovered lane",
          "[shop][mouse][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    CharacterSave save;
    save.gold = 5000;
    save.progress().health = 100;
    save.progress().inventory.addKeys(1);
    const std::array<PartyMember, 2> party{{{3, save}, {1, save}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    const auto transform = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    const auto render = [&] {
        device.draws.clear();
        scene.render(device, projection, 640, 448);
    };
    const auto pointer = [&](Vec2 point, bool press = false, bool back = false, f32 wheel = 0) {
        const Vec4 clip = transform * Vec4{point, 0.5f, 1};
        ShopSession::Inputs input{};
        for (auto& lane : input) {
            lane.pointer = (Vec2{clip} / clip.w + Vec2{1}) * 0.5f;
            lane.pointerNormalized = true;
            lane.pointerPressed = press;
            lane.pointerBack = back;
            lane.pointerScroll = wheel;
        }
        return input;
    };
    const auto rowY = [&](usize cursor, usize row) {
        const auto layout = ShopLayout::make(scene.session().catalog().items(), cursor, 32);
        return static_cast<f32>(layout.rows[row]) + layout.target;
    };
    constexpr f32 kLaneX = 3 * 128;
    render();
    SECTION("one click buys the pointed row and the sell price performs a sale") {
        scene.update(0, pointer({kLaneX + 60, rowY(0, 2) - 3}, true));
        REQUIRE(scene.session().lanes()[0].cursor == 2);
        REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Bought);
        REQUIRE(scene.session().party()[0].save.progress().inventory.keys == 2);
        REQUIRE(scene.session().party()[0].save.gold == 4900);
        REQUIRE(scene.session().party()[1].save.gold == 5000);
        scene.update(1, {}); // finish the newly focused row's scroll
        render();
        scene.update(0, pointer({kLaneX + 60, rowY(2, 2) + 16}, true));
        REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Sold);
        REQUIRE(scene.session().party()[0].save.progress().inventory.keys == 1);
        REQUIRE(scene.session().party()[0].save.gold == 4975);
        scene.update(0, pointer({kLaneX + 60, rowY(2, 2) + 16})); // held, not a fresh click
        REQUIRE_FALSE(scene.session().lanes()[0].transacted);
    }
    SECTION("right click sells only a hit row and never an unpointed player's selection") {
        scene.update(0, pointer({kLaneX + 24, rowY(0, 2) + 3}, false, true));
        REQUIRE(scene.session().lanes()[0].feedback == ShopResult::Sold);
        REQUIRE(scene.session().party()[0].save.progress().inventory.keys == 0);
        REQUIRE(scene.session().party()[1].save.progress().inventory.keys == 1);
        scene.update(1, {});
        render();
        scene.update(0, pointer({kLaneX + 3, 310}, false, true));
        REQUIRE_FALSE(scene.session().lanes()[0].transacted);
        REQUIRE_FALSE(scene.session().lanes()[1].transacted);
    }
    SECTION("hover highlights the row but a stationary cursor cannot undo controller navigation") {
        const Vec2 point{kLaneX + 24, rowY(0, 2) + 3};
        scene.update(0, pointer(point));
        REQUIRE(scene.session().lanes()[0].cursor == 2);
        render();
        const f32 buyY = rowY(0, 2) - 6 + 0.25f;
        REQUIRE(std::ranges::any_of(device.draws, [&](const auto& draw) {
            return std::ranges::any_of(draw.vertices, [&](const auto& vertex) {
                return vertex.position.x == kLaneX + 58.25f && vertex.position.y == buyY &&
                       vertex.color == Color::white();
            });
        }));
        auto input = pointer(point);
        input[3].down = true;
        scene.update(1, input);
        REQUIRE(scene.session().lanes()[0].cursor == 3);
        render();
        scene.update(1, pointer(point));
        REQUIRE(scene.session().lanes()[0].cursor == 3);
        REQUIRE(scene.session().lanes()[1].cursor == 0);
        REQUIRE(scene.session().party()[0].save.gold == 5000);
    }
    SECTION("arrows and wheel navigate only their lane and ignore the HUD") {
        scene.update(0, pointer({kLaneX + 34, 282}, true));
        REQUIRE(scene.session().lanes()[0].cursor == 1);
        render();
        scene.update(0, pointer({kLaneX + 80, 180}, false, false, -1));
        REQUIRE(scene.session().lanes()[0].cursor == 2);
        scene.update(1, {});
        render();
        scene.update(0, pointer({kLaneX + 80, 180}, false, false, 1));
        REQUIRE(scene.session().lanes()[0].cursor == 1);
        scene.update(0, pointer({kLaneX + 80, 340}, false, false, -1));
        REQUIRE(scene.session().lanes()[0].cursor == 1);
        REQUIRE(scene.session().lanes()[1].cursor == 0);
    }
    SECTION("faded rows and transaction clicks during scrolling are not actionable") {
        ShopSession::Inputs keys{};
        keys[3].down = true;
        for (s32 i = 0; i < 7; ++i) {
            scene.update(0, keys);
        }
        scene.update(0, pointer({kLaneX + 24, rowY(0, 2) + 3}, true));
        REQUIRE_FALSE(scene.session().lanes()[0].transacted);
        scene.update(10, {});
        render();
        const auto cursor = scene.session().lanes()[0].cursor;
        const auto layout = ShopLayout::make(scene.session().catalog().items(), cursor, 32);
        bool faded = false;
        for (usize row = 0; row < layout.rows.size(); ++row) {
            const f32 y = static_cast<f32>(layout.rows[row]) + layout.target;
            if (ShopLayout::opacity(y) > 0 && ShopLayout::opacity(y) < 255) {
                scene.update(0, pointer({kLaneX + 24, y + 1}, true));
                REQUIRE_FALSE(scene.session().lanes()[0].transacted);
                REQUIRE(scene.session().lanes()[0].cursor == cursor);
                faded = true;
                break;
            }
        }
        REQUIRE(faded);
        REQUIRE(scene.session().party()[0].save.gold == 5000);
    }
    SECTION("disabled rows and unjoined columns cannot focus or transact") {
        auto empty = save;
        empty.gold = 0;
        empty.progress().inventory.keys = 0;
        const std::array<PartyMember, 1> broke{{{3, empty}}};
        REQUIRE(scene.open(device, context, broke, {}, {}, "G1", ShopVisit::Shop));
        render();
        scene.update(0, pointer({kLaneX + 24, rowY(0, 2) + 3}, true));
        REQUIRE(scene.session().lanes()[0].cursor == 0);
        REQUIRE_FALSE(scene.session().lanes()[0].transacted);
        scene.update(0, pointer({24, rowY(0, 2) + 3}, true));
        REQUIRE_FALSE(scene.session().lanes()[0].transacted);
    }
}

TEST_CASE("mouse continues through tally exit stats inventory and final stats without lane bleed",
          "[shop][mouse][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array<PartyMember, 2> party{{{2, CharacterSave{}}, {0, CharacterSave{}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1"));
    const auto render = [&] { scene.render(device, Mat4{1}, 512, 384); };
    const auto click = [&](f32 y) {
        ShopSession::Inputs inputs{};
        for (auto& input : inputs) {
            input.pointer = Vec2{2 * 128 + 48, y};
            input.pointerPressed = true;
        }
        return scene.update(0, inputs);
    };
    scene.update(10, {});
    render();
    click(96);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Shopping);
    REQUIRE(scene.session().lanes()[1].phase == ShopPhase::Tally);
    render();
    click(88); // Exit is initially at y=72; its label starts at 84.
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::AfterStats);
    scene.update(1, {});
    render();
    click(285);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Inventory);
    render();
    click(285);
    REQUIRE(scene.session().lanes()[0].inventory.phase() == InventoryPanel::Phase::Leaving);
    scene.update(1, {});
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Done);
    REQUIRE(scene.session().lanes()[1].phase == ShopPhase::Tally);

    REQUIRE(scene.open(device, context, party, {}, {}, "H4", ShopVisit::FinalStats));
    render();
    click(285);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::FinalStats);
    scene.update(6, {});
    render();
    click(285);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Done);
    REQUIRE(scene.session().lanes()[1].phase == ShopPhase::FinalStats);
}

TEST_CASE("after-level screen renders every phase with retail assets", "[shop][screens][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("INVENTORY/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    bool prompted = false;
    context.controlLabels = [&](s32 player, std::string_view action) {
        CHECK(player == 2);
        CHECK(action == "menuSelect");
        prompted = true;
        return "RB";
    };
    AfterLevelScene scene;
    CharacterSave save;
    save.gold = 5000;
    save.name = "SHOP";
    save.progress().health = 100;
    const std::array<PartyMember, 1> party{{{2, save}}};
    const std::array<LevelResults, 1> results{{{2, {500, 40, 900}}}};
    REQUIRE(scene.open(device, context, party, results, {1000, 100, 1000}, "G1"));
    REQUIRE(scene.isOpen());
    const Mat4 projection{1};
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(device.draws.empty());
    TextureSet art;
    REQUIRE(art.load(root / "SELECT"));
    const auto artworkDraws = [&](std::string_view name) {
        const auto id = art.find(name);
        REQUIRE(id.has_value());
        const auto& pixels = art.image(*id).pixels;
        std::vector<const test::RecordedDraw*> found;
        for (const auto& draw : device.draws) {
            const auto* image = dynamic_cast<const test::FakeTexture*>(draw.texture);
            if (image != nullptr && image->pixels == pixels) {
                found.push_back(&draw);
            }
        }
        return found;
    };
    // GUNE5D show_gold (80099E5C), table 80122ED0, height globals 80343E0C.
    // All four background columns exist, but only player 2 has a tally. Experience
    // is the tallest pile for this fixture; it starts as a 20-pixel TOP crop.
    REQUIRE(artworkDraws("S1_PLYR1").size() == 1);
    REQUIRE(artworkDraws("S1_PLYR4").size() == 1);
    REQUIRE(artworkDraws("SHOP_SCROLL_1").empty());
    REQUIRE(artworkDraws("SHP_GOLD").empty());
    REQUIRE(artworkDraws("SHP_BONES").empty());
    const auto piles = artworkDraws("SHP_EXP");
    REQUIRE(piles.size() == 1);
    REQUIRE(test::minCorner(*piles[0]) == Vec2{256, 300});
    REQUIRE(test::maxCorner(*piles[0]) == Vec2{384, 320});
    REQUIRE(std::ranges::max(piles[0]->vertices, {}, [](const auto& vertex) {
                return vertex.uv.y;
            }).uv.y == 20.0f / 256);
    REQUIRE_FALSE(scene.update(10, {}));
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    // shop_setup creates GOLD, BONES, EXP blits at identical depths; MBDrawBlits
    // traverses that append order. Height ranking must not reorder the sprites.
    const auto gold = artworkDraws("SHP_GOLD");
    const auto bones = artworkDraws("SHP_BONES");
    const auto experience = artworkDraws("SHP_EXP");
    REQUIRE(gold.size() == 1);
    REQUIRE(bones.size() == 1);
    REQUIRE(experience.size() == 1);
    REQUIRE(gold.front() < bones.front());
    REQUIRE(bones.front() < experience.front());
    // Piles at 63990 must be behind both frame halves at 63900, including
    // the neighboring column's overlapping edge. Canvas order replaces GX depth.
    for (const auto* name : {"S1_BORDER", "S2_BORDER"}) {
        const auto borders = artworkDraws(name);
        REQUIRE(borders.size() == 4);
        for (const auto* border : borders) {
            REQUIRE(experience.front() < border);
        }
    }
    ShopSession::Inputs input;
    input[2].select = true;
    REQUIRE_FALSE(scene.update(0, input));
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Shopping);
    const auto scrolls = artworkDraws("SHOP_SCROLL_1");
    REQUIRE(scrolls.size() == 1);
    REQUIRE(test::minCorner(*scrolls[0]) == Vec2{256, 0});
    REQUIRE(test::maxCorner(*scrolls[0]) == Vec2{384, 256});
    const auto shopGold = artworkDraws("SHP_GOLD");
    REQUIRE(shopGold.size() == 1);
    REQUIRE(shopGold.front() < scrolls.front());
    const auto lowerScrolls = artworkDraws("SHOP_SCROLL_2");
    REQUIRE(lowerScrolls.size() == 1);
    for (const auto* name : {"S1_BORDER", "S2_BORDER"}) {
        const auto borders = artworkDraws(name);
        REQUIRE(borders.size() == 4);
        for (const auto* border : borders) {
            REQUIRE(scrolls.front() < border);
            REQUIRE(lowerScrolls.front() < border);
        }
    }
    // Crossing Exit to the last row uses write_shop_menu's negative-speed snap;
    // no elapsed time should be needed to see the wrapped selection.
    const auto& items = scene.session().catalog().items();
    REQUIRE_FALSE(items.back().texture.empty());
    input = {};
    input[2].up = true;
    scene.update(0, input);
    REQUIRE(scene.session().lanes()[0].cursor == items.size() - 1);
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    const auto lastIcon = artworkDraws(items.back().texture);
    REQUIRE_FALSE(lastIcon.empty());
    REQUIRE(std::ranges::any_of(lastIcon, [](const auto* draw) {
        return test::minCorner(*draw).y == ShopLayout::kBottom;
    }));
    input = {};
    input[2].down = true;
    scene.update(0, input);
    REQUIRE(scene.session().lanes()[0].cursor == 0);
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    const auto firstIcon = artworkDraws(items[1].texture);
    REQUIRE_FALSE(firstIcon.empty());
    const auto firstRow = ShopLayout::make(items, 0, 32);
    REQUIRE(std::ranges::any_of(firstIcon, [&](const auto* draw) {
        return test::minCorner(*draw).y == firstRow.target + firstRow.rows[1];
    }));
    // No opaque rectangle may hide the parchment. Only the full-screen clear is untextured.
    for (const auto& draw : device.draws) {
        if (draw.texture == &device.whiteTexture()) {
            REQUIRE(draw.transform == Mat4{1});
            REQUIRE(test::minCorner(draw) == Vec2{-1, -1});
            REQUIRE(test::maxCorner(draw) == Vec2{1, 1});
        }
    }
    // Walk every catalog entry: this also exercises all optional artwork and text wrapping.
    input = {};
    input[2].down = true;
    for (usize i = 0; i < 34; ++i) {
        REQUIRE_FALSE(scene.update(1, input));
        scene.render(device, projection, 512, 384);
    }
    input = {};
    input[2].select = true;
    REQUIRE_FALSE(scene.update(0, input));
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE_FALSE(scene.update(0.5, input));
    // The stats confirmed, the inventory panel flies in over the column (shop.c 498-522).
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Inventory);
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_OPTMENUSEL", "S_STNDGLASS"});
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(scene.update(2, {}));
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    const auto windows = artworkDraws("WINDOW_EMPTY");
    REQUIRE(windows.size() == 1);
    REQUIRE(test::minCorner(*windows[0]) == Vec2{256, 0});
    REQUIRE(test::maxCorner(*windows[0]) == Vec2{320, 256});
    const auto scimitars = artworkDraws("SCIMITAR_EMPTY");
    REQUIRE(scimitars.size() == 1);
    REQUIRE(test::minCorner(*scimitars[0]) == Vec2{352, 32});
    REQUIRE(artworkDraws("SCIMITAR").empty());
    REQUIRE(artworkDraws("LITCH_PIECE").empty());
    REQUIRE(artworkDraws("FANGS").size() == 1);
    REQUIRE(test::minCorner(*artworkDraws("FANGS")[0]) == Vec2{312, 116});
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_OPTMENUSEL"});
    REQUIRE(scene.update(0.25, {}));
    scene.render(device, projection, 512, 384);
    REQUIRE(scene.session().party()[0].save.gold == 5000);
    CHECK_FALSE(prompted);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the level panel names a level gained, shows the magic perks' line at 25, and a "
          "traded price flashes red",
          "[shop][screens][assets]") {
    // GUNE5D shop_show_lv (8009A2C8): AudioExp on entry (S_HAS, S_GAINEDLEVEL after the
    // name); string 184 (MAGIC_ATT1) at level 25, page char_type, at (xcol, 224) in
    // 0xFF80C0; do_shopping's 30-tick price timer draws the row's price in 0xFF0000.
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
    CharacterSave save;
    save.gold = 5000;
    save.color = 1;
    save.progress().health = 100;
    save.progress().experience = levelExperience(25);
    const std::array<PartyMember, 1> party{{{1, save}}};
    const std::array<LevelResults, 1> results{{{1, {0, 0, levelExperience(25)}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, results, {1000, 100, 1000}, "G1"));
    REQUIRE(scene.session().lanes()[0].entryLevel == 1);
    REQUIRE_FALSE(scene.update(10, {}));
    ShopSession::Inputs input;
    input[1].select = true;
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::BeforeStats);
    REQUIRE(scene.lastSounds() ==
            std::vector<std::string>{"S_OPTMENUSEL", "S_BLUWAR2", "S_HAS", "S_GAINEDLEVEL"});
    const Mat4 projection{1};
    const auto drawnIn = [&](const Color& color) {
        return std::ranges::any_of(device.draws, [&](const test::RecordedDraw& draw) {
            return std::ranges::any_of(draw.vertices, [&](const ImmediateVertex& vertex) {
                return vertex.color.r == color.r && vertex.color.g == color.g &&
                       vertex.color.b == color.b;
            });
        });
    };
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE(drawnIn(Color::rgba(255, 128, 192)));
    REQUIRE_FALSE(drawnIn(Color::rgba(255, 0, 0)));
    REQUIRE_FALSE(scene.update(10, input));
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Shopping);
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_OPTMENUSEL"});
    input = {};
    input[1].down = true;
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_SECRETCLOCK2"});
    REQUIRE_FALSE(scene.update(0, input));
    input = {};
    input[1].select = true;
    REQUIRE_FALSE(scene.update(2, {}));    // retail locks trading while the list scrolls
    REQUIRE_FALSE(scene.update(0, input)); // a key
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_PICKUPMAGIC"});
    REQUIRE(scene.session().lanes()[0].flashRow == 2);
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(drawnIn(Color::rgba(255, 0, 0))); // the selected row is white and glowing
    input = {};
    input[1].up = true;
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_SECRETCLOCK1"});
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE(drawnIn(Color::rgba(255, 0, 0)));
    REQUIRE_FALSE(scene.update(0.5, {}));
    device.draws.clear();
    scene.render(device, projection, 512, 384);
    REQUIRE_FALSE(drawnIn(Color::rgba(255, 0, 0)));
    input = {};
    input[1].back = true;
    REQUIRE_FALSE(scene.update(0, input)); // nothing to sell on the cherry row
    REQUIRE(scene.lastSounds() == std::vector<std::string>{"S_NO"});
    scene.close();
}

TEST_CASE("shop layout uses catalog scale and line breaks rather than fixed rows",
          "[shop][screens]") {
    // 8009BE24 lays out icon rows with 24 pixels, scaled FONT32 lines, then a 16-pixel gap.
    const std::array<ShopItem, 4> items{{{"", "EXIT", 1},
                                         {"KEY", "Key", 1},
                                         {"AMULET", "Fire\nAmulet", 0.95f},
                                         {"POTION", "Potion", 1}}};
    const auto layout = ShopLayout::make(items, 2, 32);
    REQUIRE(layout.rows == std::vector<s32>{0, 32, 88, 158});
    REQUIRE(layout.target == 66);
    REQUIRE(ShopLayout::opacity(72) == 255);
    REQUIRE(ShopLayout::opacity(224) == 255);
    REQUIRE(ShopLayout::opacity(40) == 0);
    REQUIRE(ShopLayout::opacity(256) == 0);
}

TEST_CASE("shop captions are the retail strings, not invented instructions", "[shop][screens]") {
    // GUNE5D string pools 80114918, 80348310, 80348368 and 80348414.
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    REQUIRE(strings.get("shop.stats") == "Stats");
    REQUIRE(strings.get("shop.title") == "Shop"); // retail 8034840C
    REQUIRE(strings.get("shop.continue") == "Continue");
    REQUIRE(strings.get("shop.experience") == "Exp.");
    REQUIRE(strings.get("shop.buy") == "B:");
    REQUIRE(strings.get("shop.sell") == "S:");
}

TEST_CASE("retail realm tally loop is shared, audible, and stopped when piles settle",
          "[shop][screens][audio][assets]") {
    // 8009FCA8 selects 80123454[world], starts only if absent, kills when statsFlag is zero.
    const char realm = GENERATE('G', 'J');
    const std::string bank = std::format("SHOP_{}", realm);
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip(std::format("audio/{}.vbk", bank));
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    sounds.setCategoryVolume(SoundCategory::Music, 0);
    GameContext context;
    context.unpackedRoot = root;
    context.sounds = &sounds;
    CharacterSave save;
    const std::array<PartyMember, 2> party{{{0, save}, {3, save}}};
    const std::array<LevelResults, 2> results{{{0, {}}, {3, {1000, 1000, 1000}}}};
    AfterLevelScene scene;
    REQUIRE(
        scene.open(device, context, party, results, {999, 999, 999}, std::format("{}1", realm)));
    REQUIRE(sounds.voiceCount() == 1); // music only, before piles start moving
    scene.update(0.1, {});
    REQUIRE(sounds.voiceCount() == 2);

    // Compare mixed PCM with the actual named loop, not merely a nonzero voice count.
    SoundSet expectedBank;
    REQUIRE(expectedBank.load(root / "audio" / bank));
    const auto cue = expectedBank.find(std::format("S_TALLYSFX{}", realm));
    REQUIRE(cue.has_value());
    AudioMixer expectedMixer(48000);
    SoundPlayer expectedSound(expectedMixer);
    expectedSound.play(expectedBank.sequence(*cue));
    std::vector<f32> actual(9600);
    std::vector<f32> expected(9600);
    mixer.mix(actual);
    expectedMixer.mix(expected);
    REQUIRE(actual == expected);
    REQUIRE(std::ranges::any_of(actual, [](f32 sample) { return sample != 0; }));

    scene.update(2.1, {});
    REQUIRE(scene.session().lanes()[0].tally.finished());
    REQUIRE_FALSE(scene.session().lanes()[1].tally.finished());
    REQUIRE(sounds.voiceCount() == 2); // neither one loop per player nor restarted each update
    scene.update(10, {});
    mixer.mix(actual);
    sounds.update();
    REQUIRE(sounds.voiceCount() == 1);
    scene.close();
    mixer.mix(actual);
    sounds.update();
    REQUIRE(sounds.voiceCount() == 0);

    SECTION("closing mid-tally kills the loop") {
        REQUIRE(scene.open(device, context, party, results, {}, std::format("{}1", realm)));
        scene.update(0.1, {});
        REQUIRE(sounds.voiceCount() == 2);
        scene.close();
        mixer.mix(actual);
        sounds.update();
        REQUIRE(sounds.voiceCount() == 0);
    }
    SECTION("tower shop does not play tally audio") {
        REQUIRE(
            scene.open(device, context, party, {}, {}, std::format("{}1", realm), ShopVisit::Shop));
        scene.update(0.1, {});
        REQUIRE(sounds.voiceCount() == 1);
        scene.close();
    }
}
} // namespace
