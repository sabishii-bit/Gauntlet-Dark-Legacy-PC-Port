#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/BossVictoryPresentation.h"

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

TEST_CASE("victory captions use the table's page lengths and render typed multiline text",
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
    presentation.update(6, 0.0f, false, strings);
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
    REQUIRE(test::minCorner(device.draws.front()).y == Approx(252.375f));
    REQUIRE(test::maxCorner(device.draws.front()).y == Approx(277.125f));
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
} // namespace
