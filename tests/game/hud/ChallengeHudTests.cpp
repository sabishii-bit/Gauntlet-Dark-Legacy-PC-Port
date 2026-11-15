#include <algorithm>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "formats/AnimationTree.h"
#include "game/hud/ChallengeHud.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** The hourglass's art as plain pictures: the glass, its sand and two frames of falling
 * grains, which SAND_ANIM cycles on the game clock. */
ItemArchive plainHourglass() {
    std::vector<TextureSetEntry> entries{
        {.name = "TIMER", .width = 128, .height = 128, .file = {}},
        {.name = "TIMER_SAND", .width = 128, .height = 128, .file = {}},
        {.name = "SAND_ANIM00", .width = 8, .height = 32, .file = {}},
        {.name = "SAND_ANIM01", .width = 8, .height = 32, .file = {}}};
    std::vector<Image> images;
    images.reserve(entries.size());
    for (const TextureSetEntry& entry : entries) {
        images.push_back(Image::filled(entry.width, entry.height, Color::white()));
    }
    ItemArchive archive;
    archive.textures = TextureSet::fromDecoded(std::move(entries), std::move(images));
    formats::AnimationFile animations;
    animations.textureAnimations.push_back(
        {.name = "SAND_ANIM",
         .frameName = {},
         .texture = 2,
         .source = 2,
         .frames = 2,
         .rate = 1,
         .flag = static_cast<s16>(TextureAnimationInfo::kFreeRunning)});
    archive.trees = AnimationSet::fromDecoded(std::move(animations));
    return archive;
}

/** Checks that a rectangle's six vertices cover `area` and sample `uv`. */
void checkQuad(std::span<const ImmediateVertex> quad, const Rect& area, const Rect& uv) {
    REQUIRE(quad.size() == 6);
    constexpr f32 kFar = std::numeric_limits<f32>::max();
    Vec2 low{kFar, kFar};
    Vec2 high{-kFar, -kFar};
    Vec2 uvLow{kFar, kFar};
    Vec2 uvHigh{-kFar, -kFar};
    for (const ImmediateVertex& vertex : quad) {
        low = Vec2{std::min(low.x, vertex.position.x), std::min(low.y, vertex.position.y)};
        high = Vec2{std::max(high.x, vertex.position.x), std::max(high.y, vertex.position.y)};
        uvLow = Vec2{std::min(uvLow.x, vertex.uv.x), std::min(uvLow.y, vertex.uv.y)};
        uvHigh = Vec2{std::max(uvHigh.x, vertex.uv.x), std::max(uvHigh.y, vertex.uv.y)};
    }
    CHECK(low == Vec2{area.x, area.y});
    CHECK(high == Vec2{area.right(), area.bottom()});
    CHECK(uvLow == Vec2{uv.x, uv.y});
    CHECK(uvHigh == Vec2{uv.right(), uv.bottom()});
}

TEST_CASE("the hourglass's frame, grains and sand crops are public geometry", "[secret][hud]") {
    for (const f32 elapsed : {0.0f, 0.25f, 1.0f}) {
        CAPTURE(elapsed);
        const ChallengeHud::Sand at = ChallengeHud::sandAt(elapsed);
        const ChallengeHud::Sand timed = ChallengeHud::sand(1 - elapsed, 1);
        CHECK(at.upper == timed.upper);
        CHECK(at.upperUv == timed.upperUv);
        CHECK(at.lower == timed.lower);
        CHECK(at.lowerUv == timed.lowerUv);
    }
    test::FakeRenderDevice device;
    ItemArchive archive = plainHourglass();
    ChallengeHud hud;
    REQUIRE(hud.bind(device, archive));
    const ChallengeHud::Look look = hud.look(45, 60, true);
    REQUIRE(look.elapsed == 0.25f);
    REQUIRE(look.fallingFrame == 0);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, look);
    canvas.end();
    // The glass, the grains, then both bulbs' sand in one batch.
    REQUIRE(device.draws.size() == 3);
    const Rect whole{0, 0, 1, 1};
    CHECK(device.draws[0].texture == &archive.textures.texture(device, 0));
    checkQuad(device.draws[0].vertices, ChallengeHud::kFrameArea, whole);
    CHECK(device.draws[1].texture == &archive.textures.texture(device, 2));
    checkQuad(device.draws[1].vertices, ChallengeHud::kFallingArea, whole);
    const ChallengeHud::Sand shape = ChallengeHud::sandAt(look.elapsed);
    CHECK(device.draws[2].texture == &archive.textures.texture(device, 1));
    const std::span<const ImmediateVertex> sand = device.draws[2].vertices;
    REQUIRE(sand.size() == 12);
    checkQuad(sand.first(6), shape.upper, shape.upperUv);
    checkQuad(sand.subspan(6), shape.lower, shape.lowerUv);
    hud.clear();
}

TEST_CASE("the hourglass crops its two sand reservoirs as time elapses", "[secret][hud]") {
    const auto full = ChallengeHud::sand(60, 60);
    CHECK(full.upper.y == 24);
    CHECK(full.upper.height == 41);
    CHECK(full.upperUv.y == Approx(23.0f / 128));
    CHECK(full.lower.y == 106);
    CHECK(full.lower.height == 23);
    const auto empty = ChallengeHud::sand(0, 60);
    CHECK(empty.upper.y == 63);
    CHECK(empty.upper.height == 2);
    CHECK(empty.upperUv.height == 0);
    CHECK(empty.lower.y == 68);
    CHECK(empty.lower.height == 61);
    CHECK(empty.lowerUv.y == Approx(67.0f / 128));
    CHECK(ChallengeHud::sand(-10, 60).lower.height == 61);
    CHECK(ChallengeHud::sand(80, 60).upper.height == 41);
}

TEST_CASE("hourglass uses the shipped sand animation and draws both cropped halves",
          "[secret][hud][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    ChallengeHud hud;
    REQUIRE(hud.bind(device, archive));
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 30, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws.back().vertices.size() == 12);
    const auto* first = device.draws[1].texture;
    hud.step(1.0f / 30);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 29, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[1].texture != first);
    hud.clear();
}

TEST_CASE("native hourglass rewind retains subframe sand clock without binding textures again",
          "[secret][hud][challenge-hud-local-rewind][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    ChallengeHud expected;
    ChallengeHud corrected;
    REQUIRE(expected.bind(device, archive));
    REQUIRE(corrected.bind(device, archive));
    const auto seconds = [](s32 t) { return t % 3 == 0 ? 1.0f / 120 : 1.0f / 40; };
    s32 at = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 delay = 1 + window % 8;
        CAPTURE(window, at, delay);
        ChallengeHud::LocalSnapshot saved;
        corrected.saveLocal(saved);
        CHECK_FALSE(expected.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            expected.step(seconds(t));
            corrected.step(0.73f);
        }
        const auto created = device.texturesCreated;
        const auto uploaded = device.textureUpdates;
        REQUIRE(corrected.restoreLocal(saved));
        for (s32 t = at; t < at + delay; ++t) {
            corrected.step(seconds(t));
        }
        CHECK(device.texturesCreated == created);
        CHECK(device.textureUpdates == uploaded);
        const auto a = corrected.look(30, 60, true);
        const auto b = expected.look(30, 60, true);
        CHECK(a.elapsed == b.elapsed);
        CHECK(a.fallingFrame == b.fallingFrame);
        const auto draw = [&](ChallengeHud& hud) {
            device.draws.clear();
            Canvas canvas;
            canvas.begin(device, Mat4{1});
            hud.draw(canvas, 30, 60, true);
            canvas.end();
            return device.draws;
        };
        const auto reference = draw(expected);
        const auto actual = draw(corrected);
        REQUIRE(actual.size() == reference.size());
        for (usize i = 0; i < actual.size(); ++i) {
            test::checkRecordedDraw(actual[i], reference[i]);
        }
        at += delay;
    }
    ChallengeHud::LocalSnapshot stale;
    corrected.saveLocal(stale);
    corrected.clear();
    REQUIRE_FALSE(corrected.restoreLocal(stale));
}
} // namespace
