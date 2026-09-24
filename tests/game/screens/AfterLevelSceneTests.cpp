#include <algorithm>
#include <array>
#include <format>

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
namespace {
using namespace gdl;
using namespace gdl::game;
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
    REQUIRE(scene.update(0.5, input));
    scene.render(device, projection, 512, 384);
    REQUIRE(scene.session().party()[0].save.gold == 5000);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
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
        REQUIRE(scene.open(device, context, party, {}, {}, std::format("{}1", realm), true));
        scene.update(0.1, {});
        REQUIRE(sounds.voiceCount() == 1);
        scene.close();
    }
}
} // namespace
