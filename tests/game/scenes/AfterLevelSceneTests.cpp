#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <stdexcept>
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
#include "game/audio/ShopMusic.h"
#include "game/effects/ShopFrameLight.h"
#include "game/hud/ScriptedControlLabels.h"
#include "game/players/PickupVoices.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/shop/ShopLayout.h"
#include "game/widgets/MenuInput.h"
namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("expanded shop option changes only the enabled native shop catalog", "[shop][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    std::array<PartyMember, 1> party{{{0, CharacterSave{}}}};
    party[0].save.gold = 99999;
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "L1", ShopVisit::Shop));
    CHECK(scene.session().catalog().items().size() == 34);
    config.game.expandedShop = true;
    REQUIRE(scene.open(device, context, party, {}, {}, "L1", ShopVisit::Shop));
    const auto& offers = scene.session().catalog().items();
    REQUIRE(offers.size() == 39);
    const auto halo = std::ranges::find(offers, 35, &ShopItem::type);
    REQUIRE(halo != offers.end());
    const auto start = static_cast<usize>(halo - offers.begin());
    const std::array<s32, 7> order{35, 16, 40, 34, 29, 39, 36};
    for (usize index = 0; index < order.size(); ++index) {
        CHECK(offers[start + index].type == order[index]);
    }
    TextureSet select;
    TextureSet inventory;
    REQUIRE(select.load(root / "SELECT"));
    REQUIRE(inventory.load(root / "INVENTORY"));
    for (usize index = 1; index <= 5; ++index) {
        CAPTURE(offers[start + index].texture);
        // SELECT owns shop art when a name is also present in INVENTORY.
        auto& bank = select.find(offers[start + index].texture) ? select : inventory;
        const auto icon = bank.find(offers[start + index].texture);
        REQUIRE(icon);
        const auto& expected = dynamic_cast<const test::FakeTexture&>(bank.texture(device, *icon));
        REQUIRE(scene.focusOnline(0, start + index));
        scene.update(60, {}); // settle the full retail scroll to this newly available row
        device.draws.clear();
        scene.render(device, makeScreenProjection(512, 384), 512, 384);
        CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
            const auto* actual = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return actual != nullptr && actual->width() == expected.width() &&
                   actual->height() == expected.height() && actual->pixels == expected.pixels;
        }));
    }
    config.game.expandedShop = false;
    REQUIRE(scene.open(device, context, party, {}, {}, "L1", ShopVisit::Shop));
    CHECK(scene.session().catalog().items().size() == 34);
}
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
    AfterLevelScene scene;
    context.unpackedRoot = test::scratchDirectory("shop-screen-empty");
    REQUIRE_FALSE(scene.open(device, context, {}, {}, {}, "G1"));
    REQUIRE_FALSE(scene.isOpen());
    scene.close();
    scene.close();
}

TEST_CASE("shop completion follows native phrase durations without a draining audio device",
          "[shop][screens][audio][speech-timing][determinism][assets]") {
    const s32 rate = GENERATE(30, 60);
    const bool throwOutput = GENERATE(false, true);
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("AUDIO/COMMON.VBK");
    test::assetOrSkip("AUDIO/VOICE1.VBK");
    test::assetOrSkip("AUDIO/VOICE2.VBK");
    test::assetOrSkip("AUDIO/WAR.VBK");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    GameServices context;
    context.unpackedRoot = root;
    GameServices audibleContext = context;
    audibleContext.sounds = &output;
    CharacterSave save;
    save.name = "SHOP";
    save.progress().health = 100;
    save.progress().experience = levelExperience(2);
    const std::array<PartyMember, 1> party{{{0, save}}};
    const std::array<LevelResults, 1> results{{{0, {0, 0, levelExperience(2)}}}};
    AfterLevelScene silent;
    AfterLevelScene audible;
    REQUIRE(silent.open(device, context, party, results, {1, 1, levelExperience(2)}, "G1"));
    REQUIRE(audible.open(device, audibleContext, party, results, {1, 1, levelExperience(2)}, "G1"));

    SoundSet common;
    SoundSet character;
    SoundSet narrator;
    SoundSet secondNarrator;
    REQUIRE(common.load(root / "audio/COMMON"));
    REQUIRE(character.load(root / "audio/WAR"));
    REQUIRE(narrator.load(root / "audio/VOICE1"));
    REQUIRE(secondNarrator.load(root / "audio/VOICE2"));
    const auto duration = [](SoundSet& bank, std::string_view name) {
        const auto found = bank.find(name);
        REQUIRE(found);
        const auto timing = SpeechTiming::of(bank.sequence(*found));
        REQUIRE_FALSE(timing.looping);
        REQUIRE(timing.remainingSeconds > 0);
        return timing.remainingSeconds;
    };
    f64 phraseSeconds = duration(character, PickupVoices::nameOf(save.character, save.color));
    for (const auto* const name : {"S_HAS", "S_GAINEDLEVEL"}) {
        phraseSeconds += duration(secondNarrator.find(name) ? secondNarrator : narrator, name);
    }
    const f64 longestEffect = std::max(
        {phraseSeconds, duration(common, "S_OPTMENUSEL"), duration(common, "S_STNDGLASS")});
    REQUIRE(longestEffect < 30); // The fixture's exact authored completion bound, not hardware.
    REQUIRE_FALSE(silent.update(10, {}));
    REQUIRE_FALSE(audible.update(10, {}));
    REQUIRE(silent.session().lanes()[0].tally.finished());
    ShopSession::Inputs accept{};
    accept[0].select = true;
    std::vector<SoundHandle> attempted;
    {
        const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play) {
                attempted.push_back(event.handle);
                if (throwOutput) {
                    throw std::runtime_error("shop output unavailable");
                }
            }
        });
        REQUIRE_FALSE(silent.update(0, accept));
        REQUIRE_FALSE(audible.update(0, accept));
        REQUIRE(audible.session().lanes()[0].phase == ShopPhase::BeforeStats);
        REQUIRE(audible.lastSounds() == silent.lastSounds());
        REQUIRE(audible.lastSounds().size() == 4); // Select, name, has, gained-level.
        // Native confirmation can skip stat reveals and the inventory entrance.
        // Keep every cue at this boundary so the full chained duration is observable.
        for (s32 press = 0; press < 16 && silent.session().lanes()[0].inventory.phase() !=
                                              InventoryPanel::Phase::Leaving;
             ++press) {
            REQUIRE_FALSE(silent.update(0, accept));
            REQUIRE_FALSE(audible.update(0, accept));
            REQUIRE(audible.session().lanes()[0].phase == silent.session().lanes()[0].phase);
            REQUIRE(audible.lastSounds() == silent.lastSounds());
        }
        REQUIRE(silent.session().lanes()[0].inventory.phase() == InventoryPanel::Phase::Leaving);
        REQUIRE_FALSE(attempted.empty());
        SpeechTiming remaining{longestEffect, false};
        bool completed = false;
        const f64 seconds = 1.0 / rate;
        const auto bound = static_cast<s32>(std::ceil(std::max(longestEffect, 0.25) * rate)) + 2;
        for (s32 tick = 1; tick <= bound; ++tick) {
            CAPTURE(rate, throwOutput, tick, longestEffect);
            remaining.advance(seconds);
            const bool expected = silent.update(seconds, {});
            const bool actual = audible.update(seconds, {});
            REQUIRE(actual == expected);
            CHECK(actual == (silent.session().finished() && !remaining.playing()));
            // Deliberately never call mixer.mix: the non-null output cannot drain.
            output.update();
            if (actual) {
                completed = true;
                break;
            }
        }
        REQUIRE(completed);
        CHECK(std::ranges::any_of(attempted,
                                  [&](SoundHandle handle) { return output.isPlaying(handle); }));
    }
    audible.close();
    if (!throwOutput) {
        CHECK(std::ranges::none_of(attempted,
                                   [&](SoundHandle handle) { return output.isPlaying(handle); }));
    }
    // A throwing observer can fail after starting a voice but before returning
    // its output-only handle; explicitly clean that injected fault up as well.
    output.stopAll();
    silent.close();
}
TEST_CASE("unheard shop cues keep their timing, and level-ups are still said",
          "[shop][screens][audio][speech-timing][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("AUDIO/COMMON.VBK");
    test::assetOrSkip("AUDIO/VOICE1.VBK");
    test::assetOrSkip("AUDIO/VOICE2.VBK");
    test::assetOrSkip("AUDIO/WAR.VBK");
    test::FakeRenderDevice device;
    AudioMixer heardMixer(48000);
    AudioMixer mutedMixer(48000);
    SoundPlayer heardOutput(heardMixer);
    SoundPlayer mutedOutput(mutedMixer);
    GameServices context;
    context.unpackedRoot = root;
    GameServices heardContext = context;
    heardContext.sounds = &heardOutput;
    GameServices mutedContext = context;
    mutedContext.sounds = &mutedOutput;
    CharacterSave save;
    save.name = "SHOP";
    save.progress().health = 100;
    save.progress().experience = levelExperience(2);
    const std::array<PartyMember, 1> party{{{0, save}}};
    const std::array<LevelResults, 1> results{{{0, {0, 0, levelExperience(2)}}}};
    AfterLevelScene heard;
    AfterLevelScene muted;
    REQUIRE(heard.open(device, heardContext, party, results, {1, 1, levelExperience(2)}, "G1"));
    REQUIRE(muted.open(device, mutedContext, party, results, {1, 1, levelExperience(2)}, "G1"));
    muted.setCuesHeard(false);
    SoundSet common;
    REQUIRE(common.load(root / "audio/COMMON"));
    const auto select = common.find("S_OPTMENUSEL");
    REQUIRE(select);
    const std::string menuBank = common.sequence(*select).bank;
    REQUIRE_FALSE(heard.update(10, {}));
    REQUIRE_FALSE(muted.update(10, {}));
    std::vector<std::string> heardBanks;
    std::vector<std::string> mutedBanks;
    const auto played = [](std::vector<std::string>& banks) {
        return [&banks](const SoundPlayer::Event& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play) {
                banks.push_back(event.bank);
            }
        };
    };
    const SoundPlayer::Capture heardCapture(&heardOutput, played(heardBanks));
    const SoundPlayer::Capture mutedCapture(&mutedOutput, played(mutedBanks));
    ShopSession::Inputs accept{};
    accept[0].select = true;
    for (s32 press = 0; press < 16 && heard.session().lanes()[0].inventory.phase() !=
                                          InventoryPanel::Phase::Leaving;
         ++press) {
        REQUIRE_FALSE(heard.update(0, accept));
        REQUIRE_FALSE(muted.update(0, accept));
        REQUIRE(muted.session().lanes()[0].phase == heard.session().lanes()[0].phase);
        REQUIRE(muted.lastSounds() == heard.lastSounds());
    }
    bool finished = false;
    for (s32 tick = 0; tick < 60 * 60 && !finished; ++tick) {
        CAPTURE(tick);
        finished = heard.update(1.0 / 60, {});
        REQUIRE(muted.update(1.0 / 60, {}) == finished); // the same timing, heard or not
    }
    REQUIRE(finished);
    CHECK(std::ranges::count(heardBanks, menuBank) > 0);
    CHECK(std::ranges::count(mutedBanks, menuBank) == 0);
    CHECK_FALSE(mutedBanks.empty()); // the level-up is still said
    heardOutput.stopAll();
    mutedOutput.stopAll();
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
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
    GameServices context;
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
    SECTION("online pointer capture waits for authoritative focus and transaction") {
        auto input = pointer({kLaneX + 60, rowY(0, 2) - 3}, true)[3];
        const auto row = scene.captureOnlinePointer(3, input);
        REQUIRE(row == 2);
        CHECK(input.select);
        CHECK(scene.session().lanes()[0].cursor == 0);
        CHECK(scene.session().party()[0].save.gold == 5000);
        CHECK_FALSE(scene.focusOnline(0, *row));
        CHECK_FALSE(scene.focusOnline(3, scene.session().catalog().items().size()));
        REQUIRE(scene.focusOnline(3, *row));
        ShopSession::Inputs committed{};
        committed[3].select = input.select;
        scene.update(0, committed, nullptr, nullptr, true);
        CHECK(scene.session().party()[0].save.gold == 4900);
        CHECK(scene.session().party()[0].save.progress().inventory.keys == 2);
        CHECK(scene.session().party()[1].save.gold == 5000);
    }
    SECTION("online pointer capture does not steer a different seat") {
        auto input = pointer({kLaneX + 60, rowY(0, 2) - 3}, true)[1];
        CHECK_FALSE(scene.captureOnlinePointer(1, input));
        CHECK_FALSE(input.select);
        CHECK(scene.session().lanes()[1].cursor == 0);
        auto wheel = pointer({kLaneX + 80, 180}, false, false, -1)[3];
        CHECK_FALSE(scene.captureOnlinePointer(3, wheel));
        CHECK(wheel.down);
        CHECK(scene.session().lanes()[0].cursor == 0);
    }
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
    SECTION("a bound mouse press buys the pointed lane instead of the keyboard lane") {
        GameConfig config;
        config.controls[1].device = "keyboard";
        config.controls[1].customized = true;
        config.controls[1].menu.select = {Key::MouseLeft};
        config.controls[1].menu.shopSell = {Key::MouseLeft};
        config.controls[1].menu.shopExit = {Key::MouseLeft};
        context.config = &config;
        REQUIRE(scene.open(device, context, party, {}, {}, "G1", ShopVisit::Shop));
        render();
        const Vec4 clip = transform * Vec4{kLaneX + 60, rowY(0, 2) - 3, 0.5f, 1};
        Input devices;
        devices.setKey(Key::MouseLeft, true);
        devices.setPointer({(clip.x / clip.w + 1) / 2, (clip.y / clip.w + 1) / 2, true, true});
        ShopSession::Inputs inputs;
        for (s32 seat = 0; seat < 4; ++seat) {
            inputs[static_cast<usize>(seat)] = readPlayerMenuInput(devices, config, seat);
        }
        scene.update(0, inputs);
        CHECK(scene.session().party()[0].save.gold == 4900);
        CHECK(scene.session().party()[0].save.progress().inventory.keys == 2);
        CHECK(scene.session().party()[1].save.gold == 5000);
        CHECK(scene.session().party()[1].save.progress().inventory.keys == 1);
        CHECK(scene.session().lanes()[1].phase == ShopPhase::Shopping);
        CHECK_FALSE(scene.session().lanes()[1].transacted);
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
    GameServices context;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array<PartyMember, 2> party{{{2, CharacterSave{}}, {0, CharacterSave{}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {}, "G1"));
    BitmapFont font;
    REQUIRE(font.load(root / "FONTS/font32.fnt", 16));
    TextureSet art;
    REQUIRE(art.load(root / "STATIC"));
    const auto fontId = art.find("FONT32");
    REQUIRE(fontId);
    const auto& pixels = art.image(*fontId).pixels;
    const auto centeredContinue = [&](s32 y) {
        test::FakeRenderDevice expected;
        Canvas canvas;
        TextPainter painter;
        painter.setFont(&font, &art.texture(expected, *fontId));
        canvas.begin(expected, Mat4{1});
        painter.draw(canvas, -(2 * 128 + 64), y, "Continue", TextStyle{0.5f, Color::white()});
        canvas.end();
        REQUIRE_FALSE(expected.draws.empty());
        CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels == pixels &&
                   !std::ranges::search(draw.vertices, expected.draws.front().vertices).empty();
        }));
    };
    const auto render = [&] {
        device.draws.clear();
        scene.render(device, Mat4{1}, 512, 384);
    };
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
    centeredContinue(92);
    // Under the pointer the word keeps its white ink; only its glow holds steady.
    {
        ShopSession::Inputs hover{};
        for (auto& input : hover) {
            input.pointer = Vec2{2 * 128 + 48, 96};
        }
        scene.update(0, hover);
        render();
        centeredContinue(92);
        const auto purple = Color::rgba(130, 0, 234);
        CHECK(std::ranges::none_of(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels == pixels &&
                   std::ranges::any_of(draw.vertices, [&](const ImmediateVertex& vertex) {
                       return vertex.color.r == purple.r && vertex.color.g == purple.g &&
                              vertex.color.b == purple.b && vertex.position.y >= 92 &&
                              vertex.position.y <= 92 + 16;
                   });
        }));
        CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
            const auto* texture = dynamic_cast<const test::FakeTexture*>(draw.texture);
            return texture != nullptr && texture->pixels != pixels &&
                   std::ranges::any_of(draw.vertices, [&](const ImmediateVertex& vertex) {
                       return vertex.color.r == purple.r && vertex.color.g == purple.g &&
                              vertex.color.b == purple.b && vertex.color.a == 255 &&
                              vertex.position.y >= 92 && vertex.position.y <= 92 + 16;
                   });
        }));
    }
    click(96);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Shopping);
    REQUIRE(scene.session().lanes()[1].phase == ShopPhase::Tally);
    render();
    click(88); // Exit is initially at y=72; its label starts at 84.
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::AfterStats);
    scene.update(1, {});
    render();
    centeredContinue(280);
    click(285);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::Inventory);
    render();
    centeredContinue(InventoryPanel::kPromptY);
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
    centeredContinue(280);
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
    GameServices context;
    context.unpackedRoot = root;
    context.strings = &strings;
    bool prompted = false;
    test::ScriptedControlLabels labels;
    labels.answer = [&](s32 player, std::string_view action) {
        CHECK(player == 2);
        CHECK(action == "menuSelect");
        prompted = true;
        return std::string("RB");
    };
    context.controlLabels = &labels;
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
    SoundSet confirmation;
    REQUIRE(confirmation.load(root / "audio/COMMON"));
    const auto confirm = confirmation.find("S_OPTMENUSEL");
    REQUIRE(confirm);
    const auto confirmTiming = SpeechTiming::of(confirmation.sequence(*confirm));
    REQUIRE_FALSE(confirmTiming.looping);
    const auto glass = confirmation.find("S_STNDGLASS");
    REQUIRE(glass);
    auto glassTiming = SpeechTiming::of(confirmation.sequence(*glass));
    REQUIRE_FALSE(glassTiming.looping);
    // The inventory cue began two simulation seconds before this confirmation.
    // Its authored 3.42-second sound outlasts the shorter confirmation and the
    // 15-tick panel exit, so both must remain part of the completion oracle.
    glassTiming.advance(2);
    const f64 confirmationWait = std::max(0.25, confirmTiming.remainingSeconds);
    CAPTURE(confirmTiming.remainingSeconds, glassTiming.remainingSeconds);
    REQUIRE(glassTiming.remainingSeconds > confirmationWait);
    REQUIRE_FALSE(scene.update(confirmationWait, {}));
    REQUIRE(scene.session().finished());
    glassTiming.advance(confirmationWait);
    REQUIRE(scene.update(glassTiming.remainingSeconds, {}));
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
    GameServices context;
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

TEST_CASE("a pile taller than its art repeats the art from its top, and none shows above the frame",
          "[shop][screens][piles]") {
    using Piece = AfterLevelScene::PilePiece;
    const auto check = [](f32 height, const std::vector<Piece>& expected) {
        CAPTURE(height);
        const auto pieces = AfterLevelScene::pilePieces(128, height);
        REQUIRE(pieces.size() == expected.size());
        for (usize i = 0; i < pieces.size(); ++i) {
            CAPTURE(i);
            CHECK(pieces[i].area == expected[i].area);
            CHECK(pieces[i].uv == expected[i].uv);
        }
    };
    // Up to its art's height a pile is one piece, its art cropped from the top.
    check(20, {{.area = {128, 300, 128, 20}, .uv = {0, 0, 1, 20.0f / 256}}});
    check(256, {{.area = {128, 64, 128, 256}, .uv = {0, 0, 1, 1}}});
    // Past it the art starts over below, as the GameCube's repeating texture does.
    check(300, {{.area = {128, 20, 128, 256}, .uv = {0, 0, 1, 1}},
                {.area = {128, 276, 128, 44}, .uv = {0, 0, 1, 44.0f / 256}}});
    // The reported sale: 1300 gold over 626 at the shop's start lifts the pile to 410 rows,
    // whose top 90 are above the frame.
    check(410, {{.area = {128, 0, 128, 166}, .uv = {0, 90.0f / 256, 1, 166.0f / 256}},
                {.area = {128, 166, 128, 154}, .uv = {0, 0, 1, 154.0f / 256}}});
    // A repeat may begin right at the frame's top; one wholly above it is not drawn.
    check(576, {{.area = {128, 0, 128, 256}, .uv = {0, 0, 1, 1}},
                {.area = {128, 256, 128, 64}, .uv = {0, 0, 1, 0.25f}}});
    // 3000 gold sold for with none brought in: 563700 rows above the frame, 244 into a repeat.
    check(20 + 3000 * 188, {{.area = {128, 0, 128, 12}, .uv = {0, 244.0f / 256, 1, 12.0f / 256}},
                            {.area = {128, 12, 128, 256}, .uv = {0, 0, 1, 1}},
                            {.area = {128, 268, 128, 52}, .uv = {0, 0, 1, 52.0f / 256}}});
    check(0, {});
}

TEST_CASE("a pile a sale lifts past its art repeats it as the GameCube does, not smeared",
          "[shop][screens][piles][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("PDATA/WAR.WAD");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameServices context;
    context.unpackedRoot = root;
    context.strings = &strings;
    CharacterSave save;
    save.gold = 10;
    save.progress().health = 100;
    save.progress().inventory.keys = 9;
    const std::array<PartyMember, 1> party{{{0, save}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, {}, {1000, 100, 1000}, "G1", ShopVisit::Shop));
    const auto& lane = scene.session().lanes()[0];
    REQUIRE(lane.phase == ShopPhase::Shopping);
    const auto& items = scene.session().catalog().items();
    ShopSession::Inputs input;
    input[0].down = true;
    for (usize step = 0; step < items.size() && items[lane.cursor].type != ShopItem::kKey; ++step) {
        REQUIRE_FALSE(scene.update(0, input));
        REQUIRE_FALSE(scene.update(0, {}));
    }
    REQUIRE(items[lane.cursor].type == ShopItem::kKey);
    REQUIRE_FALSE(scene.update(2, {})); // the list settles before anything is traded
    input = {};
    input[0].back = true;
    REQUIRE_FALSE(scene.update(0, input));
    REQUIRE(lane.member.save.gold > save.gold);
    REQUIRE_FALSE(scene.update(1.0 / 60, {}));
    // The pile stands for the gold now held against the gold brought in: far past its art.
    REQUIRE(lane.goldHeight > LevelTally::kBaseline);
    device.draws.clear();
    scene.render(device, Mat4{1}, 512, 384);
    TextureSet art;
    REQUIRE(art.load(root / "SELECT"));
    const auto gold = art.find("SHP_GOLD");
    REQUIRE(gold);
    const auto& pixels = art.image(*gold).pixels;
    std::vector<ImmediateVertex> pile;
    for (const auto& draw : device.draws) {
        const auto* image = dynamic_cast<const test::FakeTexture*>(draw.texture);
        if (image != nullptr && image->pixels == pixels) {
            pile.insert(pile.end(), draw.vertices.begin(), draw.vertices.end());
        }
    }
    REQUIRE_FALSE(pile.empty());
    // Sampled within the art, a clamped edge never stretches its last row down the column.
    CHECK(std::ranges::all_of(
        pile, [](const auto& vertex) { return vertex.uv.y >= 0 && vertex.uv.y <= 1; }));
    // The art starts over inside the column, and the column ends at the frame's top.
    CHECK(std::ranges::any_of(
        pile, [](const auto& vertex) { return vertex.uv.y == 0 && vertex.position.y > 0; }));
    CHECK(std::ranges::min(pile, {}, [](const auto& vertex) {
              return vertex.position.y;
          }).position.y == 0);
    CHECK(std::ranges::max(pile, {}, [](const auto& vertex) {
              return vertex.position.y;
          }).position.y == LevelTally::kBaseline);
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
    GameServices context;
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

TEST_CASE("Sumner announces one gained level after another, never two at once",
          "[shop][screens][audio][speech-timing][assets]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("AUDIO/COMMON.VBK");
    test::assetOrSkip("AUDIO/VOICE1.VBK");
    test::assetOrSkip("AUDIO/VOICE2.VBK");
    test::assetOrSkip("AUDIO/WAR.VBK");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    GameServices context;
    context.unpackedRoot = root;
    context.sounds = &output;
    CharacterSave save;
    save.name = "SHOP";
    save.progress().health = 100;
    save.progress().experience = levelExperience(2);
    const std::array<PartyMember, 2> party{{{0, save}, {1, save}}};
    const std::array<LevelResults, 2> results{
        {{0, {0, 0, levelExperience(2)}}, {1, {0, 0, levelExperience(2)}}}};
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, results, {1, 1, levelExperience(2)}, "G1"));
    REQUIRE_FALSE(scene.update(10, {}));
    struct Spoken {
        SoundHandle handle;
        SoundHandle after;
        std::string bank;
    };
    std::vector<Spoken> spoken;
    {
        const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
            if (event.kind == SoundPlayer::Event::Kind::Play && event.bank != "COMMON") {
                spoken.push_back({event.handle, event.after, event.bank});
            }
        });
        // Both players reach their level panels in the same moment.
        ShopSession::Inputs accept{};
        accept[0].select = true;
        accept[1].select = true;
        REQUIRE_FALSE(scene.update(0, accept));
    }
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::BeforeStats);
    REQUIRE(scene.session().lanes()[1].phase == ShopPhase::BeforeStats);
    // Name, "has", "gained a level" for each: the second name waits for the first phrase.
    REQUIRE(spoken.size() == 6);
    CHECK(spoken[0].after == kNoSound);
    CHECK(spoken[1].after == spoken[0].handle);
    CHECK(spoken[2].after == spoken[1].handle);
    CHECK(spoken[3].after == spoken[2].handle);
    CHECK(spoken[4].after == spoken[3].handle);
    CHECK(spoken[5].after == spoken[4].handle);
    scene.close();
}

} // namespace
