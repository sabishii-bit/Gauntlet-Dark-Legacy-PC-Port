#include <algorithm>
#include <array>
#include <format>
#include <span>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/scenes/LevelLoadingScreen.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("encounter movies play on every entry, including characters with legacy seen flags",
          "[level-loading][save]") {
    std::array<PartyMember, 2> party;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].save.moviesSeen.emplace_back("movieG1");
    party[1].save.moviesSeen.emplace_back("movieG1");
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].save = CharacterSave::fromJson(party[0].save.toJson());
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    CHECK(LevelLoadingScreen::movieWanted("movieG4", party));
    party[1].save = {};
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[1].fallen = true;
    CHECK(LevelLoadingScreen::movieWanted("movieG1", party));
    party[0].fallen = true;
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", party));
    CHECK_FALSE(LevelLoadingScreen::movieWanted("movieG1", {}));
}

TEST_CASE("travel keeps a map before the stage preview and skips both for tower returns",
          "[level-loading][assets]") {
    const auto root =
        test::assetOrSkip("MAPS/LEVELG1/textures.ngc").parent_path().parent_path().parent_path();
    test::assetOrSkip("WDATA/TOWN.WAD");
    test::FakeRenderDevice device;
    GameServices context;
    context.unpackedRoot = root;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelLoadingScreen screen;
    screen.open(device, context, *level);
    CHECK(screen.active());
    CHECK(screen.movie() == "movieG1");
    CHECK(screen.previewAlpha() == 0);
    Canvas canvas;
    canvas.begin(device, makeLetterboxProjection(512, 384, 1920, 1080));
    screen.draw(canvas, device);
    canvas.end();
    REQUIRE(device.draws.size() >= 5);
    CHECK(device.draws.front().transform == Mat4{1});
    CHECK(test::minCorner(device.draws.front()) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws.front()) == Vec2{1, 1});
    CHECK(device.draws.front().vertices.front().color == Color::black());
    std::array<bool, 4> panels{};
    for (const auto& draw : device.draws) {
        if (test::minCorner(draw).y == StatusBoxPainter::kY &&
            test::maxCorner(draw).y == StatusBoxPainter::kY + StatusBoxPainter::kHeight &&
            test::maxCorner(draw).x - test::minCorner(draw).x == StatusBoxPainter::kWidth) {
            const auto slot =
                static_cast<usize>(test::minCorner(draw).x / StatusBoxPainter::kWidth);
            REQUIRE(slot < panels.size());
            panels[slot] = true;
        }
    }
    CHECK(panels == std::array{true, true, true, true});
    CHECK_FALSE(screen.update(3.5f));
    CHECK(screen.previewAlpha() == 0);
    CHECK_FALSE(screen.update(1.5f));
    CHECK(screen.previewAlpha() == 1);
    CHECK(screen.update(2));
    screen.close();
    CHECK_FALSE(screen.active());
    screen.open(device, context, LevelRef::tower());
    CHECK_FALSE(screen.active());
    CHECK(screen.update(0));
    CHECK(screen.movie().empty());
}

TEST_CASE("loading pictures retain complete native HUD tops through crossfades and resizing",
          "[level-loading][assets]") {
    const auto root =
        test::assetOrSkip("MAPS/LEVELG1/textures.ngc").parent_path().parent_path().parent_path();
    test::assetOrSkip("STATIC/textures.ngc");
    test::assetOrSkip("SELECT/textures.ngc");
    test::FakeRenderDevice device;
    GameServices context;
    context.unpackedRoot = root;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    std::array<PartyMember, 2> party;
    party[0].player = 2;
    party[0].save.name = "HERO";
    party[1].player = 1;
    party[1].fallen = true;
    LevelLoadingScreen screen;
    screen.open(device, context, *level, party);

    TextureSet artwork;
    REQUIRE(artwork.load(root / "STATIC"));
    TextureSet maps;
    REQUIRE(maps.load(root / "MAPS" / "LEVELG1"));
    const auto texture = [&](TextureSet& set, std::string_view name) -> const test::FakeTexture& {
        const auto index = set.find(name);
        REQUIRE(index);
        return dynamic_cast<const test::FakeTexture&>(set.texture(device, *index));
    };
    const auto& strip = texture(artwork, "S3");
    const auto& activeStrip = texture(artwork, "BK_RUNE_STONE_02");
    const auto& glint = texture(artwork, "TRBO_GLINT");
    const auto& meter = texture(artwork, "TRBO_FULL_NEW");
    std::array<const test::FakeTexture*, 4> previewTiles{};
    for (usize i = 0; i < previewTiles.size(); ++i) {
        previewTiles[i] = &texture(maps, std::format("LDMAP_G1_{:02}", i));
    }
    const auto matches = [](const test::RecordedDraw& draw, const test::FakeTexture& expected) {
        return draw.texture->width() == expected.width() &&
               draw.texture->height() == expected.height() &&
               dynamic_cast<const test::FakeTexture&>(*draw.texture).pixels == expected.pixels;
    };
    // The preview is one picture joined from its four tiles at their places (0,0) (256,0)
    // (0,256) (256,256) and mended along the joins, so no seam shows between them: well
    // inside each tile its texels sit where the tile was placed.
    const auto joinedPreview = [&](const test::RecordedDraw& draw) {
        if (draw.texture->width() != 512 || draw.texture->height() != 384) {
            return false;
        }
        const auto& pixels = dynamic_cast<const test::FakeTexture&>(*draw.texture).pixels;
        const auto texel = [&](u32 x, u32 y) {
            return std::span(pixels).subspan((usize{y} * 512 + x) * 4, 4);
        };
        constexpr u32 kInside = 32;
        const auto inside = [](const test::FakeTexture& tileTexture) {
            return std::span(tileTexture.pixels)
                .subspan((usize{kInside} * tileTexture.width() + kInside) * 4, 4);
        };
        return std::ranges::equal(texel(kInside, kInside), inside(*previewTiles[0])) &&
               std::ranges::equal(texel(256 + kInside, kInside), inside(*previewTiles[1])) &&
               std::ranges::equal(texel(kInside, 256 + kInside), inside(*previewTiles[2])) &&
               std::ranges::equal(texel(256 + kInside, 256 + kInside), inside(*previewTiles[3]));
    };
    const std::array<Vec2, 4> extents{{{640, 448}, {1024, 768}, {1920, 1080}, {900, 1200}}};
    // Sample the map, a partially crossfaded preview and the complete preview.
    for (const f32 seconds : std::array{0.0f, 4.0f, 1.0f}) {
        screen.update(seconds);
        for (const Vec2& extent : extents) {
            CAPTURE(seconds, extent.x, extent.y);
            const Mat4 transform = makeVirtualScreenTransform(
                makeLetterboxProjection(640, 448, extent.x, extent.y), 512, 384, 640, 448);
            device.draws.clear();
            Canvas canvas;
            canvas.begin(device, transform);
            screen.draw(canvas, device);
            canvas.end();
            REQUIRE_FALSE(device.draws.empty());
            CHECK(device.draws.front().transform == Mat4{1});
            CHECK(test::minCorner(device.draws.front()) == Vec2{-1, -1});
            CHECK(test::maxCorner(device.draws.front()) == Vec2{1, 1});
            CHECK(device.draws.front().vertices.front().color == Color::black());
            std::array<bool, 4> strips{};
            usize glints = 0;
            usize meters = 0;
            usize previews = 0;
            for (const auto& draw : device.draws) {
                if (joinedPreview(draw)) {
                    ++previews;
                }
                if (matches(draw, strip) || matches(draw, activeStrip)) {
                    CHECK(previews == 1);
                    const Vec2 corner = test::minCorner(draw);
                    const auto slot = static_cast<usize>(corner.x / StatusBoxPainter::kWidth);
                    REQUIRE(slot < strips.size());
                    // Selected characters retain their horned strip even when fallen.
                    CHECK(matches(draw, slot == 1 || slot == 2 ? activeStrip : strip));
                    strips[slot] = true;
                    CHECK(corner.y == StatusBoxPainter::kBarY);
                    CHECK(test::maxCorner(draw).y == StatusBoxPainter::kY);
                    CHECK(draw.transform == transform);
                    CHECK(draw.vertices.front().uv == Vec2{0, 0});
                    for (const auto& vertex : draw.vertices) {
                        const Vec4 projected = draw.transform * Vec4{vertex.position, 1};
                        CHECK(projected.x >= -projected.w);
                        CHECK(projected.x <= projected.w);
                        CHECK(projected.y >= -projected.w);
                        CHECK(projected.y <= projected.w);
                    }
                }
                if (matches(draw, glint)) {
                    ++glints;
                    CHECK(previews == 1);
                    CHECK(test::minCorner(draw) == Vec2{256, StatusBoxPainter::kTurboY});
                    CHECK(test::maxCorner(draw) ==
                          Vec2{256 + static_cast<f32>(glint.width()),
                               StatusBoxPainter::kTurboY + static_cast<f32>(glint.height())});
                    CHECK(draw.transform == transform);
                }
                if (matches(draw, meter)) {
                    ++meters;
                }
            }
            CHECK(strips == std::array{true, true, true, true});
            CHECK(glints == 1); // Empty and fallen slots have strips, but no live meter.
            CHECK(meters == 1); // The two meter layers share one texture batch.
        }
    }
    screen.close();
}

TEST_CASE("secret entry and return hold the transition picture above the HUD without movies",
          "[level-loading][secret][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::assetOrSkip("WDATA/TOWN.WAD");
    test::assetOrSkip("WDATA/SECRET.WAD");
    test::FakeRenderDevice device;
    GameServices context;
    context.unpackedRoot = root;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(GENERATE("S1", "S4", "S9", "G1"));
    REQUIRE(level);
    if (level->isSecret()) {
        WorldData authored;
        REQUIRE(authored.load(root / level->worldDataFile()));
        const auto* info = authored.level(level->name);
        REQUIRE(info);
        CHECK(info->mapPoints.empty()); // Retail init_mapscreen selects its fallback picture.
    }
    std::array<PartyMember, 1> party;
    party[0].player = 2;
    party[0].save.name = "HERO";
    LevelLoadingScreen screen;
    screen.open(device, context, *level, party, !level->isSecret());
    CHECK(screen.active());
    CHECK(screen.movie().empty());
    CHECK(screen.dashCount() == 0);
    CHECK_FALSE(screen.update(-1));
    CHECK_FALSE(screen.update(1.75f));
    CHECK(screen.update(0.25f));

    TextureSet artwork;
    REQUIRE(artwork.load(root / "STATIC"));
    const auto index = artwork.find(TransitionScreen::kTexture);
    REQUIRE(index);
    const auto& picture = dynamic_cast<const test::FakeTexture&>(artwork.texture(device, *index));
    Canvas canvas;
    canvas.begin(device, makeScreenProjection(512, 384));
    screen.draw(canvas, device);
    canvas.end();
    bool pictureDrawn = false;
    std::array<bool, 4> panels{};
    for (const auto& draw : device.draws) {
        const auto& texture = dynamic_cast<const test::FakeTexture&>(*draw.texture);
        if (texture.width() == picture.width() && texture.height() == picture.height() &&
            texture.pixels == picture.pixels) {
            CHECK(test::minCorner(draw) == Vec2{0, 0});
            CHECK(test::maxCorner(draw) == Vec2{512, 320});
            CHECK(draw.vertices.front().color.a == 255);
            pictureDrawn = true;
        }
        if (test::minCorner(draw).y == StatusBoxPainter::kY &&
            test::maxCorner(draw).y == StatusBoxPainter::kY + StatusBoxPainter::kHeight &&
            test::maxCorner(draw).x - test::minCorner(draw).x == StatusBoxPainter::kWidth) {
            CHECK(pictureDrawn);
            const auto slot =
                static_cast<usize>(test::minCorner(draw).x / StatusBoxPainter::kWidth);
            REQUIRE(slot < panels.size());
            panels[slot] = true;
        }
    }
    CHECK(pictureDrawn);
    CHECK(panels == std::array{true, true, true, true});
    screen.close();
    CHECK_FALSE(screen.active());
    CHECK(screen.update(0));
    // The next ordinary visit restores its map, preview and authored movie.
    screen.open(device, context, *catalog.byName("G1"), party);
    CHECK(screen.movie() == "movieG1");
    CHECK_FALSE(screen.update(LevelLoadingScreen::kTransitionSeconds));
    screen.close();
}

TEST_CASE("stage map music narration and route dots obey only the movie volume",
          "[level-loading][audio][assets]") {
    const auto root = test::assetOrSkip("AUDIO/AUDATPS2.ROM").parent_path().parent_path();
    test::assetOrSkip("WDATA/TOWN.WAD");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const bool muted = GENERATE(false, true);
    sounds.setCategoryVolume(SoundCategory::Movie, muted ? 0.0f : 1.0f);
    sounds.setCategoryVolume(SoundCategory::Effects, muted ? 1.0f : 0.0f);
    sounds.setCategoryVolume(SoundCategory::Music, muted ? 1.0f : 0.0f);
    GameServices context;
    context.unpackedRoot = root;
    context.sounds = &sounds;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G2");
    REQUIRE(level);
    LevelLoadingScreen screen;
    screen.open(device, context, *level);
    REQUIRE(sounds.voiceCount() > 0);
    std::array<f32, 1600> samples{};
    bool audible = false;
    for (usize frame = 0; frame < 600; ++frame) {
        screen.update(1.0f / 60.0f);
        sounds.update();
        mixer.mix(samples);
        audible = audible || std::ranges::any_of(samples, [](f32 value) { return value != 0; });
    }
    CHECK(screen.dashCount() > 0);
    CHECK(audible == !muted);
    screen.close();
}
} // namespace
