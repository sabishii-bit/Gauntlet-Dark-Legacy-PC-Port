#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/bosses/BossVictoryPresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Stage = BossVictory::Stage;

static_assert(!std::is_copy_constructible_v<BossVictoryPresentation>);
static_assert(!std::is_move_constructible_v<BossVictoryPresentation>);

std::filesystem::path wizardFixture(std::string_view name, bool animated = true) {
    const auto root = test::scratchDirectory(name);
    writeTextFile(root / "mesh.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                                     "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(root / "skin.png", test::kTinyPng);
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(root / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(root);
    const std::string sequence = animated ? R"({"name":"READY","frames":8,"frameRate":30,
        "repeats":true,"tracks":[{"node":0,"flags":16,"frames":[0,4],"values":[0,4]}]})"
                                          : "";
    writeTextFile(root / "animations.json", R"({"trees":[{"name":"WIZARD","nodes":[
        {"name":"ROOT","object":"BODY","position":[0,0,0]}],"sequences":[)" +
                                                sequence + "]}]}");
    return root;
}

void loadCaptions(MessageTable& table) {
    const auto root = test::scratchDirectory("victory-presentation-captions");
    writeTextFile(root / "text.json", R"({"messages":[
        {"name":"LICH_SPEECH","lines":["AB\nA","B"]},
        {"name":"RUNE_PHRASE0","lines":["A"]}]})");
    REQUIRE(table.load(root / "text.json"));
}

TEST_CASE("retail boss captions keep the rune report behind the full defeat reading time",
          "[game][screens][victory-presentation]") {
    const auto root = test::assetOrSkip("TEXT/english.rom");
    MessageTable strings;
    REQUIRE(strings.load(root));
    // Actual GUNE5D English pages: 105.25 / 117.5 / 108.75 text units.
    // speech_frame >> 1 must exceed those budgets to reach the terminator;
    // then the page holds 60 ticks and the next speech waits 0.5 seconds (30).
    struct Reading {
        s32 kind;
        char realm;
        s32 runeTick;
    };
    for (const auto [kind, realm, runeTick] :
         {Reading{34, 'B', 302}, Reading{37, 'D', 326}, Reading{41, 'G', 308}}) {
        for (const s32 step : {1, 2}) {
            CAPTURE(kind, step);
            BossVictoryPresentation presentation;
            presentation.begin(kind, realm, 1, 0, false);
            presentation.update(300, 5, false, strings);
            REQUIRE(presentation.update(64, 64.0f / 60, false, strings).voices.size() == 1);
            for (s32 tick = step; tick < runeTick; tick += step) {
                REQUIRE(presentation.update(step, static_cast<f32>(step) / 60, false, strings)
                            .voices.empty());
                REQUIRE(presentation.state().stage() == Stage::Defeat);
            }
            const auto followup =
                presentation.update(step, static_cast<f32>(step) / 60, false, strings);
            REQUIRE(followup.voices.size() == 1);
            REQUIRE(followup.voices.front().sound == BossVictory::runeVoiceOf(kind, realm, 0));
            REQUIRE(presentation.state().stage() == Stage::Runes);
        }
    }
}

TEST_CASE("victory presentation is inert when cleared and tolerates an absent wizard",
          "[game][screens][victory-presentation]") {
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    const MessageTable strings;
    const TextPainter text;
    Canvas canvas;
    REQUIRE_FALSE(presentation.state().running());
    REQUIRE(presentation.update(100, 1.0f, false, strings).voices.empty());
    presentation.drawWizard(device, Mat4{1.0f}, {});
    presentation.drawCaption(canvas, text, strings, 512, 384);
    REQUIRE(device.draws.empty());
    ItemArchive missing;
    presentation.begin(41, 'G', 0, 0, false);
    presentation.bindWizard(device, missing, Vec3{10.0f}, {});
    presentation.update(BossVictory::kWaitTicks, 0.0f, false, strings);
    const auto result = presentation.update(64, 1.0f, false, strings);
    REQUIRE(result.voices.size() == 1);
    REQUIRE(result.voices.front().sound == "S_DEFEATVOXG");
    presentation.drawWizard(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
    presentation.clear();
    presentation.clear();
    REQUIRE(presentation.state().stage() == Stage::None);
}

TEST_CASE("Skorne victory uses the fixed altar position regardless of party spread",
          "[skorne][victory-presentation]") {
    ItemArchive archive;
    REQUIRE(archive.load(wizardFixture("skorne-victory-position")));
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    const std::array party{Vec3{40, 10, 50}, Vec3{-10, 5, 20}};
    for (const s32 kind : {42, 43}) {
        presentation.begin(kind, 'E', 0, 0, false);
        presentation.bindWizard(device, archive, {0, -25, -19}, party);
        REQUIRE(presentation.wizardSubject().position == Vec3{0, -12, 6});
        REQUIRE(presentation.wizardSubject().attentionOffset == Vec3{0, 10, 0});
    }
}

TEST_CASE("only the two Skorne codes stand the wizard at the altar; every other code takes the "
          "midpoint",
          "[game][screens][victory-presentation][boss-kinds]") {
    ItemArchive archive;
    REQUIRE(archive.load(wizardFixture("victory-position-by-code")));
    test::FakeRenderDevice device;
    // The boss and the party sum to (9, -3, 12): a third of it, lifted three, is the midpoint.
    constexpr Vec3 kBoss{3, -6, 9};
    constexpr std::array kParty{Vec3{6, 0, 0}, Vec3{0, 3, 3}};
    struct Row {
        s32 code;
        Vec3 position;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, Vec3{3, 2, 4}},
        {33, Vec3{3, 2, 4}},
        {34, Vec3{3, 2, 4}},
        {35, Vec3{3, 2, 4}},
        {36, Vec3{3, 2, 4}},
        {37, Vec3{3, 2, 4}},
        {38, Vec3{3, 2, 4}},
        {39, Vec3{3, 2, 4}},
        {40, Vec3{3, 2, 4}},
        {41, Vec3{3, 2, 4}},
        {42, Vec3{0, -12, 6}},
        {43, Vec3{0, -12, 6}},
        {44, Vec3{3, 2, 4}},
        {45, Vec3{3, 2, 4}},
        {std::numeric_limits<s32>::min(), Vec3{3, 2, 4}},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        BossVictoryPresentation presentation;
        presentation.begin(row.code, 'G', 0, 0, false);
        presentation.bindWizard(device, archive, kBoss, kParty);
        CHECK(presentation.wizardSubject().position == row.position);
    }
}

TEST_CASE("victory wizard placement animation lighting and fade follow the visit",
          "[game][screens][victory-presentation]") {
    const auto root = wizardFixture("victory-presentation-wizard");
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    const MessageTable strings;
    const std::array<Vec3, 2> standing{Vec3{6, 0, 0}, Vec3{0, 0, 6}};
    presentation.begin(41, 'G', 0, 0, false);
    presentation.bindWizard(device, archive, Vec3{0.0f}, standing);
    const auto subject = presentation.wizardSubject();
    REQUIRE(subject.position == Vec3{2, 3, 2});
    REQUIRE(subject.facing == Approx(kPi / 4.0f));
    REQUIRE(subject.height == 1.0f);
    REQUIRE(subject.attentionOffset == Vec3{0, 10, 0});
    REQUIRE(subject.focus == BossCameraSubject::Focus::Wizard);
    REQUIRE(subject.awake);
    WorldLighting lighting;
    lighting.ambient = Vec3{0.25f, 0.5f, 0.75f};
    lighting.lightColor = Vec3{0.0f};
    presentation.drawWizard(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.empty());
    presentation.update(BossVictory::kWaitTicks, 0.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.empty()); // appearing starts transparent
    presentation.update(32, 0.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.size() == 1);
    const auto& fade = device.draws.front();
    REQUIRE(fade.vertices.front().position == subject.position);
    REQUIRE(fade.vertices.front().color == Color::white().withAlpha(128));
    REQUIRE(fade.state.blend == BlendMode::Additive);
    REQUIRE_FALSE(fade.state.depthWrite);
    device.draws.clear();
    presentation.update(32, 1.0f / 30.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.size() == 1);
    const auto& opaque = device.draws.front();
    REQUIRE(opaque.vertices.front().position.x == Approx(2.0f + std::sqrt(0.5f)));
    REQUIRE(opaque.vertices.front().position.z == Approx(2.0f - std::sqrt(0.5f)));
    REQUIRE(opaque.vertices.front().color.a == 255);
    REQUIRE_FALSE(opaque.state.depthWrite);
    REQUIRE(opaque.state.blend == BlendMode::Additive);
    presentation.clear();
    archive.clear(); // no subsequent update or draw may refer into the old archive
    device.draws.clear();
    presentation.update(2, 1.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, lighting);
    REQUIRE(device.draws.empty());
    REQUIRE(presentation.wizardSubject().position == Vec3{0.0f});
}

TEST_CASE("victory wizard selects and advances its animated lower body meshes",
          "[game][screens][victory-presentation]") {
    const auto root = wizardFixture("victory-presentation-object-frames");
    writeTextFile(root / "lower.obj",
                  "v 0 -2 0\nv 1 -2 0\nv 0 -1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"mesh.obj","meshTriangles":1},
        {"index":1,"name":"LOWER0","file":"lower.obj","meshTriangles":1},
        {"index":2,"name":"LOWER1","file":"mesh.obj","meshTriangles":1}]})");
    test::convertModelFixture(root);
    writeTextFile(root / "animations.json", R"({"trees":[{"name":"WIZARD","nodes":[
        {"name":"BODY","object":"BODY","position":[0,0,0]},
        {"name":"OANIM","position":[0,0,0],"type":2,"objectFrames":[{"object":"LOWER0","start":0,"frames":2}]}],
        "sequences":[{"name":"READY","frames":2,"frameRate":30,"repeats":true}]}]})");
    ItemArchive archive;
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    presentation.begin(41, 'G', 0, 0, false);
    presentation.bindWizard(device, archive, Vec3{0}, {});
    presentation.update(BossVictory::kWaitTicks, 0, false, {});
    presentation.update(64, 0, false, {});
    presentation.drawWizard(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[1].vertices[0].position.y == Approx(1));
    device.draws.clear();
    presentation.update(2, 1.0f / 30.0f, false, {});
    presentation.drawWizard(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[1].vertices[0].position.y == Approx(3));
}

TEST_CASE("a static victory wizard needs no animation sequence and can be rebound",
          "[game][screens][victory-presentation]") {
    ItemArchive archive;
    REQUIRE(archive.load(wizardFixture("victory-presentation-static", false)));
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    const MessageTable strings;
    presentation.begin(41, 'G', 0, 0, false);
    presentation.bindWizard(device, archive, Vec3{4, 0, 8}, {});
    REQUIRE(presentation.wizardSubject().position == Vec3{4, 3, 8});
    presentation.update(BossVictory::kWaitTicks, 0.0f, false, strings);
    presentation.update(64, 1.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.front().position == Vec3{4, 3, 8});
    ItemArchive missing;
    presentation.bindWizard(device, missing, Vec3{0.0f}, {});
    archive.clear();
    device.draws.clear();
    presentation.update(2, 1.0f, false, strings);
    presentation.drawWizard(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.empty());
    presentation.clear();
}

TEST_CASE("victory captions use the table's weighted text and render typed multiline text",
          "[game][screens][victory-presentation]") {
    MessageTable strings;
    loadCaptions(strings);
    BossVictoryPresentation presentation;
    presentation.begin(41, 'G', 0, 0, false);
    presentation.update(BossVictory::kWaitTicks, 0.0f, false, strings);
    presentation.update(64, 0.0f, false, strings);
    presentation.update(2, 0.0f, false, strings);
    REQUIRE(presentation.state().caption().has_value());
    REQUIRE(presentation.state().caption()->shown == 1);
    presentation.update(10, 0.0f, false, strings);
    REQUIRE(presentation.state().caption().has_value());
    REQUIRE(presentation.state().caption()->shown == 4);
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    const test::FakeTexture sheet{32, 16};
    TextPainter text;
    text.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    presentation.drawCaption(canvas, text, strings, 512, 384);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 18); // AB on the first line, A on the second
    // At the top of the screen (CaptionText's y of 16), lines 32 * 0.667 apart; the tower's
    // ceremony captions are the ones over the status boxes.
    REQUIRE(test::minCorner(device.draws.front()).y >= 16.0f);
    REQUIRE(test::minCorner(device.draws.front()).y < 18.0f);
    REQUIRE(test::maxCorner(device.draws.front()).y < 16.0f + 2.0f * 32.0f * 0.667f);
    const f32 middle =
        (test::minCorner(device.draws.front()).x + test::maxCorner(device.draws.front()).x) / 2.0f;
    REQUIRE(std::abs(middle - 256.0f) < 8.0f); // centred on the 512-wide screen's middle
    REQUIRE(device.draws.front().texture == &sheet);
    presentation.update(BossVictory::kPagePauseTicks, 0.0f, false, strings);
    REQUIRE(presentation.state().caption().has_value());
    REQUIRE(presentation.state().caption()->page == 1);
    REQUIRE(presentation.state().caption()->shown == 0);
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    presentation.drawCaption(canvas, text, strings, 512, 384);
    canvas.end();
    REQUIRE(device.draws.empty());
    presentation.update(2, 0.0f, false, strings);
    REQUIRE(presentation.state().caption().has_value());
    REQUIRE(presentation.state().caption()->shown == 1);
    const MessageTable missing;
    canvas.begin(device, Mat4{1.0f});
    presentation.drawCaption(canvas, text, missing, 512, 384);
    canvas.end();
    REQUIRE(device.draws.empty());
}

TEST_CASE("victory presentation reports voices and one sparkle then resets for another visit",
          "[game][screens][victory-presentation]") {
    MessageTable strings;
    loadCaptions(strings);
    BossVictoryPresentation presentation;
    for (s32 visit = 0; visit < 2; ++visit) {
        presentation.begin(41, 'G', 0, 0, true);
        std::vector<std::string> voices;
        s32 sparkles = 0;
        s32 tick = 0;
        while (presentation.state().stage() != Stage::Leaving && tick++ < 1000) {
            const auto result = presentation.update(1, 0.0f, true, strings);
            for (const auto& voice : result.voices) {
                voices.push_back(voice.sound);
            }
            sparkles += result.sparkle ? 1 : 0;
        }
        REQUIRE(presentation.state().stage() == Stage::Leaving);
        REQUIRE(voices == std::vector<std::string>{"S_DEFEATVOXG", "S_RUNEVOX0G"});
        REQUIRE(sparkles == 0);
        REQUIRE_FALSE(presentation.update(BossVictory::kExitTicks, 0.0f, true, strings).sparkle);
        // Gathering the last coin shortens the remaining wait to the ordinary exit delay.
        const auto result = presentation.update(
            BossVictory::kExitTicks - BossVictory::kExitSparkleTicks, 0.0f, false, strings);
        REQUIRE(result.sparkle);
        REQUIRE(presentation.state().stage() == Stage::Gone);
        REQUIRE_FALSE(presentation.update(1, 0.0f, false, strings).sparkle);
        presentation.update(BossVictory::kExitSparkleTicks - 1, 0.0f, false, strings);
        REQUIRE(presentation.state().finished());
        REQUIRE_FALSE(presentation.update(60, 1.0f, false, strings).sparkle);
    }
}
TEST_CASE("victory fractional poses preserve speech state and snap loop boundaries",
          "[game][screens][victory-presentation][presentation]") {
    ItemArchive archive;
    REQUIRE(archive.load(wizardFixture("victory-presentation-fractions")));
    test::FakeRenderDevice device;
    BossVictoryPresentation presentation;
    const MessageTable strings;
    const std::array<Vec3, 1> party{Vec3{0, 0, 10}};
    presentation.begin(41, 'G', 0, 0, false);
    presentation.bindWizard(device, archive, {}, party);
    presentation.update(BossVictory::kWaitTicks, 0, false, strings);
    presentation.update(32, 0, false, strings);
    presentation.update(32, 1.0f / 30, false, strings);
    const auto stage = presentation.state().stage();
    const auto x = [&](f32 blend) {
        device.draws.clear();
        presentation.drawWizard(device, Mat4{1}, {}, nullptr, blend);
        REQUIRE(device.draws.size() == 1);
        return device.draws.front().vertices.front().position.x;
    };
    CHECK(x(0) == Approx(0));
    CHECK(x(0.5f) == Approx(0.5f));
    CHECK(x(1) == Approx(1));
    CHECK(x(-1) == Approx(1));
    CHECK(presentation.state().stage() == stage);
    presentation.capturePresentation();
    CHECK(x(0) == Approx(1));
    CHECK(x(0.75f) == Approx(1));
    CHECK(presentation.state().stage() == stage);
    presentation.update(0, 8.0f / 30, false, strings);
    CHECK(x(0) == Approx(x(1))); // never interpolate backward through a wrapped sequence
    presentation.update(0, 0, false, strings);
    CHECK(x(0) == x(1));
    presentation.clear();
    device.draws.clear();
    presentation.drawWizard(device, Mat4{1}, {}, nullptr, 0.5f);
    CHECK(device.draws.empty());
}
TEST_CASE("native victory rewind survives lazy wizard binding and corrects an entire visit",
          "[local-rewind][victory-local-rewind]") {
    ItemArchive archive;
    REQUIRE(archive.load(wizardFixture("victory-local-rewind")));
    test::FakeRenderDevice device;
    BossVictoryPresentation actual;
    BossVictoryPresentation expected;
    BossVictoryPresentation foreign;
    MessageTable strings;
    loadCaptions(strings);
    const std::array party{Vec3{2, 0, 3}, Vec3{7, 0, 1}};
    BossVictoryPresentation::LocalSnapshot saved;
    CHECK_FALSE(actual.restoreLocal(saved));
    actual.saveLocal(saved);
    actual.begin(43, 'E', 0, 0, true);
    actual.bindWizard(device, archive, {90, -20, 4}, party);
    actual.update(800, 5, true, strings);
    const auto textures = device.texturesCreated;
    REQUIRE(actual.restoreLocal(saved));
    CHECK(actual.state().stage() == Stage::None);
    for (auto* presentation : {&actual, &expected}) {
        presentation->begin(41, 'G', 1, 0, false);
        presentation->bindWizard(device, archive, {1, 2, 3}, party);
    }
    CHECK(device.texturesCreated == textures);
    const auto draw = [&](const BossVictoryPresentation& presentation) {
        device.draws.clear();
        presentation.drawWizard(device, Mat4{1}, {}, nullptr, 0.37f);
        return device.draws;
    };
    s32 voices = 0;
    s32 sparkles = 0;
    for (s32 window = 0; window < 192; ++window) {
        CAPTURE(window);
        const s32 distance = window % 8 + 1;
        actual.saveLocal(saved);
        CHECK_FALSE(foreign.restoreLocal(saved));
        const auto before = draw(actual);
        actual.begin(43, 'E', 0, 0, true);
        actual.bindWizard(device, archive, {90, -20, 4}, {});
        for (s32 tick = 0; tick < distance; ++tick) {
            actual.update(180, 0.7f, true, strings, true);
        }
        REQUIRE(actual.restoreLocal(saved));
        const auto restored = draw(actual);
        REQUIRE(restored.size() == before.size());
        for (usize i = 0; i < before.size(); ++i) {
            test::checkRecordedDraw(restored[i], before[i]);
        }
        for (s32 tick = 0; tick < distance; ++tick) {
            actual.capturePresentation();
            expected.capturePresentation();
            const auto a = actual.update(2, 1.0f / 30, false, strings);
            const auto b = expected.update(2, 1.0f / 30, false, strings);
            CHECK(a.sparkle == b.sparkle);
            sparkles += static_cast<s32>(a.sparkle);
            REQUIRE(a.voices.size() == b.voices.size());
            voices += static_cast<s32>(a.voices.size());
            for (usize i = 0; i < a.voices.size(); ++i) {
                CHECK(a.voices[i].sound == b.voices[i].sound);
            }
            CHECK(actual.state().stage() == expected.state().stage());
            CHECK(actual.state().wizardAlpha() == expected.state().wizardAlpha());
            REQUIRE(actual.state().caption().has_value() == expected.state().caption().has_value());
            if (actual.state().caption()) {
                CHECK(actual.state().caption()->message == expected.state().caption()->message);
                CHECK(actual.state().caption()->page == expected.state().caption()->page);
                CHECK(actual.state().caption()->shown == expected.state().caption()->shown);
            }
        }
        const auto reference = draw(expected);
        const auto corrected = draw(actual);
        REQUIRE(corrected.size() == reference.size());
        for (usize i = 0; i < reference.size(); ++i) {
            test::checkRecordedDraw(corrected[i], reference[i]);
        }
    }
    CHECK(actual.state().finished());
    CHECK(voices == 2);
    CHECK(sparkles == 1);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == 0);
    actual.clear();
    CHECK_FALSE(actual.restoreLocal(saved));
    actual.saveLocal(saved);
    actual.bindWizard(device, archive, {}, {});
    REQUIRE(actual.restoreLocal(saved));
    ItemArchive different;
    REQUIRE(different.load(wizardFixture("victory-local-rewind-different")));
    actual.bindWizard(device, different, {}, {});
    CHECK_FALSE(actual.restoreLocal(saved));
}
} // namespace
