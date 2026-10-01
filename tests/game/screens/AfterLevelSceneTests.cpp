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

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/ShopLayout.h"
#include "game/screens/ShopMusic.h"
namespace {
using namespace gdl;
using namespace gdl::game;
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

TEST_CASE("shop plays the authored music through realm changes without retaining the old theme",
          "[shop][screens][audio][unpacked]") {
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    for (char realm = 'A'; realm <= 'K'; ++realm) {
        test::unpackedOrSkip(std::format("audio/SHOP_{}/sounds.json", realm));
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
          "[shop][screens][unpacked]") {
    // init_shop 8009A504/8009A52C sets 0x80808080; DrawBlit 800B47D0 doubles
    // alpha and clamps it to 255. The TEV color scale at 80067D98 is GX_CS_SCALE_2.
    // Copying those raw bytes into our normalized vertex colors fades/darkens twice.
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    test::unpackedOrSkip("SELECT/textures.json");
    test::unpackedOrSkip("INVENTORY/textures.json");
    test::unpackedOrSkip("pdata/WAR.json");
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
                REQUIRE(vertex.color == Color::white());
            }
        }
        REQUIRE(std::ranges::all_of(columns, [](bool drawn) { return drawn; }));
    }
}

TEST_CASE("after-level screen renders every phase with retail assets",
          "[shop][screens][unpacked]") {
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    test::unpackedOrSkip("SELECT/textures.json");
    test::unpackedOrSkip("INVENTORY/textures.json");
    test::unpackedOrSkip("pdata/WAR.json");
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.unpackedRoot = root;
    context.strings = &strings;
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
            REQUIRE(test::minCorner(draw) == Vec2{0, 0});
            REQUIRE(test::maxCorner(draw) == Vec2{512, 384});
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
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the level panel names a level gained, shows the magic perks' line at 25, and a "
          "traded price flashes red",
          "[shop][screens][unpacked]") {
    // GUNE5D shop_show_lv (8009A2C8): AudioExp on entry (S_HAS, S_GAINEDLEVEL after the
    // name); string 184 (MAGIC_ATT1) at level 25, page char_type, at (xcol, 224) in
    // 0xFF80C0; do_shopping's 30-tick price timer draws the row's price in 0xFF0000.
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    test::unpackedOrSkip("SELECT/textures.json");
    test::unpackedOrSkip("pdata/WAR.json");
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
    REQUIRE(strings.get("shop.continue") == "Continue");
    REQUIRE(strings.get("shop.experience") == "Exp.");
    REQUIRE(strings.get("shop.buy") == "B:");
    REQUIRE(strings.get("shop.sell") == "S:");
}

TEST_CASE("retail realm tally loop is shared, audible, and stopped when piles settle",
          "[shop][screens][audio][unpacked]") {
    // 8009FCA8 selects 80123454[world], starts only if absent, kills when statsFlag is zero.
    const char realm = GENERATE('G', 'J');
    const std::string bank = std::format("SHOP_{}", realm);
    const auto root = test::unpackedOrSkip("shop/catalog.json").parent_path().parent_path();
    test::unpackedOrSkip(std::format("audio/{}/sounds.json", bank));
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
