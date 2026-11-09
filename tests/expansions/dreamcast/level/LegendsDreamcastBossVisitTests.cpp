#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossVisit.h"
#include "game/widgets/ScrollBox.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast boss apparition rejects invalid callbacks without consuming victory",
          "[dreamcast-boss-visit]") {
    LegendsDreamcastBossVisit visit;
    const std::array<Vec3, 1> players{Vec3{2, 0, 4}};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(visit.begin(26, 2, 0, 0, Vec3{0}, {}));
    CHECK_FALSE(visit.begin(26, 2, 0, 0, Vec3{nan, 0, 0}, players));
    CHECK_FALSE(visit.begin(25, 2, 0, 0, Vec3{0}, players));
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, players));
    CHECK_FALSE(visit.begin(26, 2, 0, 0, Vec3{0}, players));
    const auto ticks = visit.state().ticksLeft();
    visit.update(nan);
    visit.update(-1);
    visit.update(10);
    CHECK(visit.state().ticksLeft() == ticks);
}

TEST_CASE("Missing optional Dreamcast apparition art audio and translations cannot block travel",
          "[dreamcast-boss-visit]") {
    test::FakeRenderDevice device;
    LegendsDreamcastBossVisit visit;
    visit.load(device, nullptr);
    const std::array<Vec3, 1> players{Vec3{0}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, players));
    unsigned appeared = 0, departed = 0, sparkled = 0, finished = 0;
    for (unsigned tick = 0; tick < 30 * 90 && !visit.state().finished(); ++tick) {
        visit.capturePresentation();
        const auto event = visit.update(1.0f / 30);
        appeared += event.appeared;
        departed += event.depart;
        sparkled += event.sparkle;
        finished += event.finished;
        visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, .5f);
    }
    CHECK(visit.state().finished());
    CHECK(appeared == 1);
    CHECK(departed == 1);
    CHECK(sparkled == 1);
    CHECK(finished == 1);
    CHECK(device.draws.empty());
    CHECK_FALSE(visit.update(1.0f / 30).finished);
}

TEST_CASE("Dreamcast boss apparition uses the realm wizard and does not advance while drawing",
          "[dreamcast-boss-visit][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "items/levelb6/anim.rom"))
        SKIP("Dreamcast extraction is not installed");
    test::FakeRenderDevice device;
    auto archive = loadLegendsDreamcastArchive(root / "items/levelb6");
    REQUIRE(archive);
    LegendsDreamcastBossVisit visit;
    visit.load(device, archive.get());
    const std::array<Vec3, 1> players{Vec3{0}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, players));
    for (unsigned frame = 0; frame < 240; ++frame) {
        visit.capturePresentation();
        visit.update(1.0f / 30);
    }
    REQUIRE(visit.state().wizardAlpha() > 0);
    const auto stage = visit.state().stage();
    visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, .5f);
    REQUIRE_FALSE(device.draws.empty());
    const auto calls = device.draws.size();
    for (const auto& draw : device.draws) {
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.state.blend == BlendMode::Additive);
    }
    visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, .5f);
    CHECK(device.draws.size() == 2 * calls);
    CHECK(visit.state().stage() == stage);
}

TEST_CASE("Dreamcast apparition follows live players with native angular slew",
          "[dreamcast-boss-visit]") {
    LegendsDreamcastBossVisit visit;
    std::array<Vec3, 1> players{Vec3{0, 0, 10}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, players));
    // Players may collect their reward while Sumner's five-second wait runs.
    players[0] = {10, 0, 10};
    visit.updatePlayers(players);
    for (usize step = 0; step < 215; ++step)
        visit.update(1.0f / 30);
    REQUIRE(visit.state().wizardAlpha() > 0);
    CHECK(visit.position() == Vec3{5, 3, 5});
    const f32 previous = visit.facing();
    players[0] = {-100, 0, 10};
    visit.updatePlayers(players);
    visit.capturePresentation();
    visit.update(1.0f / 30);
    CHECK(previous - visit.facing() == Catch::Approx(glm::half_pi<f32>() / 30));
    const f32 valid = visit.facing();
    players[0].x = std::numeric_limits<f32>::quiet_NaN();
    visit.updatePlayers(players);
    visit.update(1.0f / 30);
    CHECK(std::isfinite(visit.facing()));
    CHECK(valid - visit.facing() == Catch::Approx(glm::half_pi<f32>() / 30));
    visit.updatePlayers({});
    const f32 empty = visit.facing();
    visit.update(1.0f / 30);
    CHECK(visit.facing() == empty);
}

TEST_CASE("Dreamcast typed captions retain the complete native line's centre",
          "[dreamcast-boss-visit]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    std::vector<BitmapGlyph> glyphs;
    for (s32 code = 33; code < 127; ++code)
        glyphs.push_back({.code = code, .width = 12});
    const auto font = BitmapFont::fromGlyphs(20, 6, std::move(glyphs));
    const test::FakeTexture sheet{32, 32};
    TextPainter text;
    text.setFont(&font, &sheet);
    LegendsDreamcastBossVisit visit;
    visit.bindStrings(&strings);
    const std::array<Vec3, 1> party{Vec3{0}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, party));
    for (usize frame = 0; frame < 400; ++frame) {
        visit.update(1.0f / 30);
        if (const auto caption = visit.state().caption(); caption && caption->shown >= 8)
            break;
    }
    REQUIRE(visit.state().caption());
    const auto lines = ScrollBox::splitLines(strings.get("legends.dc.victory.dragon.0"));
    REQUIRE_FALSE(lines.empty());
    const f32 scale = LegendsDreamcastBossCeremony::kCaptionScale;
    const s32 left = 256 - text.measure(lines.front(), scale) / 2;
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    visit.drawCaption(canvas, text, 512);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    const auto first = device.draws.front().vertices.front().position;
    CHECK(first.x == Catch::Approx(static_cast<f32>(left) + .5f * scale));
    for (usize frame = 0; frame < 8; ++frame)
        visit.update(1.0f / 30);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    visit.drawCaption(canvas, text, 512);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().vertices.front().position == first);
}

TEST_CASE("Dreamcast first Skorne displays only the collected native runes",
          "[dreamcast-boss-visit][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "items/levele2/anim.rom"))
        SKIP("Dreamcast extraction is not installed");
    auto archive = loadLegendsDreamcastArchive(root / "items/levele2");
    REQUIRE(archive);
    const auto tree = archive->trees.find("RUNESTONES");
    REQUIRE(tree);
    const auto& runes = archive->trees.tree(*tree);
    REQUIRE(runes.sequences.size() == 3);
    CHECK(runes.sequences[0].name == "OFF");
    CHECK(runes.sequences[1].name == "ACTIVE");
    CHECK(runes.sequences[2].name == "ON");
    const std::array<Vec3, 1> party{Vec3{0}};
    std::array<usize, 3> calls{};
    for (usize sample = 0; sample < calls.size(); ++sample) {
        const u64 mask = sample == 0 ? 0 : (sample == 1 ? 1 : 0xfff);
        test::FakeRenderDevice device;
        LegendsDreamcastBossVisit visit;
        visit.load(device, archive.get());
        REQUIRE(visit.begin(32, 5, mask, 0, Vec3{0}, party));
        for (usize step = 0; step < 125; ++step) {
            visit.capturePresentation();
            visit.update(1.0f / 30);
        }
        REQUIRE(visit.state().wizardAlpha() == 1);
        // Opaque runes participate in the solid pass; WIZARD is additive.
        const auto uploads = device.texturesCreated;
        visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::DepthWriting, 1);
        calls[sample] = device.draws.size();
        for (const auto& draw : device.draws)
            CHECK(draw.state.depthWrite);
        visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::DepthWriting, 1);
        CHECK(device.draws.size() == 2 * calls[sample]);
        CHECK(device.texturesCreated == uploads);
    }
    CHECK(calls[0] < calls[1]);
    CHECK(calls[1] < calls[2]);
}

TEST_CASE("Dreamcast victory preloads its native departure skin and begins it at twenty ticks",
          "[dreamcast-boss-visit][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "weapons/objects.rom"))
        SKIP("Dreamcast extraction is not installed");
    test::FakeRenderDevice device;
    LegendsDreamcastBossVisit visit;
    REQUIRE(visit.loadSkins(device, root));
    CHECK(device.texturesCreated == 10);
    const std::array<Vec3, 1> party{Vec3{0}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, party));
    for (usize frame = 0; frame < 1000; ++frame) {
        const auto event = visit.update(1.0f / 30);
        if (event.sparkle) {
            CHECK(visit.state().ticksLeft() == 20);
            break;
        }
        CHECK(visit.skin() == nullptr);
    }
    const auto* first = visit.skin();
    REQUIRE(first);
    const auto& image = static_cast<const test::FakeTexture&>(*first);
    CHECK_FALSE(image.pixels.empty());
    visit.update(1.0f / 30);
    CHECK(visit.skin() == first); // Native rate .5: two updates per texture.
    visit.update(1.0f / 30);
    REQUIRE(visit.skin());
    CHECK(visit.skin() != first);
    CHECK(device.texturesCreated == 10);
    for (usize frame = 0; frame < 30; ++frame)
        visit.update(1.0f / 30);
    CHECK(visit.state().finished());
    CHECK(visit.state().wizardAlpha() == 1);
    CHECK(visit.skin() != nullptr); // Retained during the scene's final fade.
}

TEST_CASE("Dreamcast boss shard appears before the apparition without late GPU uploads",
          "[dreamcast-boss-visit][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "items/levelb6/anim.rom"))
        SKIP("Dreamcast extraction is not installed");
    test::FakeRenderDevice device;
    auto archive = loadLegendsDreamcastArchive(root / "items/levelb6");
    REQUIRE(archive);
    LegendsDreamcastBossVisit visit;
    visit.load(device, archive.get());
    const auto uploads = device.texturesCreated;
    const std::array<Vec3, 1> party{Vec3{0}};
    REQUIRE(visit.begin(26, 2, 0, 0, Vec3{0}, party, Vec3{10, 3, 20}));
    CHECK(visit.state().wizardAlpha() == 0);
    visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::Effects, 1);
    REQUIRE_FALSE(device.draws.empty());
    for (usize frame = 0; frame < 30; ++frame) {
        visit.capturePresentation();
        visit.update(1.0f / 30);
    }
    device.draws.clear();
    visit.draw(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::Effects, 1);
    CHECK_FALSE(device.draws.empty()); // Native BOSSKEY2 persists after the birth tree.
    CHECK(visit.state().wizardAlpha() == 0);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
