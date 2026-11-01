#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMap triangleMap() {
    formats::LegendsMap map;
    formats::LegendsMapObject object;
    object.name = "test";
    object.mesh.vertices = {{.position = {0, 0, 0}, .uv = {0, 0}},
                            {.position = {10, 0, 0}, .uv = {2, 0}},
                            {.position = {0, 0, 10}, .uv = {0, 2}}};
    object.mesh.parts.push_back({.texture = 0, .indices = {0, 1, 2}});
    object.transform = glm::translate(Mat4{1}, Vec3{3, 5, 7});
    map.objects.push_back(std::move(object));
    return map;
}

void render(LegendsPreview& preview, test::FakeRenderDevice& device) {
    preview.render(device, Mat4{1}, 640, 448, kPi / 2);
}

formats::LegendsMap selectionMap(const std::filesystem::path& directory) {
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                             ? readFile(directory / "texdyn.rom")
                             : std::vector<u8>{};
    for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

void checkSelectionPose(LegendsPreview& preview, test::FakeRenderDevice& device,
                        const WorldCamera& camera, f32 alpha, s32 id,
                        const test::RecordedDraw& rendered) {
    REQUIRE(rendered.vertices.size() == 3);
    std::array<Vec3, 3> triangle;
    for (usize i = 0; i < triangle.size(); ++i) {
        triangle[i] = Vec3{rendered.transform * Vec4{rendered.vertices[i].position, 1}};
    }
    const Vec3 normal =
        glm::normalize(glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]));
    const Vec3 centre = (triangle[0] + triangle[1] + triangle[2]) / 3.0f;
    const Vec3 eye = centre + normal * 12.0f;
    const auto hit = preview.pickObject(eye, -normal, camera, alpha);
    REQUIRE(hit);
    CHECK(hit->object.id == id);
    CHECK(hit->distance == Approx(12).margin(.001f));
    device.draws.clear();
    preview.highlightObject(device, Mat4{1}, camera, alpha, id, SelectionHighlight{eye});
    REQUIRE(device.draws.size() == 2);
    const auto& wash = device.draws.back();
    REQUIRE(wash.vertices.size() == 3);
    CHECK(wash.texture == rendered.texture);
    for (usize i = 0; i < triangle.size(); ++i) {
        CHECK(glm::distance(wash.vertices[i].position,
                            triangle[i] + (eye - triangle[i]) * SelectionHighlight::kLift) < .001f);
    }
}

TEST_CASE("A fallen character cannot replace a Legends visit with its restored entry save",
          "[legends][preview][save]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    const auto map = triangleMap();
    preview.open(device, map);
    const auto camera = preview.camera();
    PartyMember member;
    member.fallen = true;
    REQUIRE_THROWS_WITH(preview.openPlayable(device, map, {}, member),
                        "Legends traversal: select a living character before entering");
    CHECK(preview.isOpen());
    CHECK_FALSE(preview.playable());
    CHECK(preview.camera() == camera);
    CHECK(preview.triangleCount() == 1);
}

TEST_CASE("Native cull changes remain separate draws even when their texture is shared",
          "[legends][preview][native-world-culling]") {
    auto map = triangleMap();
    auto& object = map.objects.front();
    object.mesh.parts.push_back(object.mesh.parts.front());
    object.partCullBack = {true, false};
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    preview.drawWorld(device, Mat4{1}, {}, TreeModel::Pass::Opaque, -1);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.cullBack != device.draws[1].state.cullBack);
    CHECK(device.draws[0].vertices.size() == 3);
    CHECK(device.draws[1].vertices.size() == 3);
    CHECK(device.draws[0].texture == device.draws[1].texture);
    object.partCullBack.clear(); // Dreamcast and old survey maps retain their defaults.
    preview.open(device, map);
    device.draws.clear();
    preview.drawWorld(device, Mat4{1}, {}, TreeModel::Pass::Opaque, -1);
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws.front().state.cullBack);
    CHECK(device.draws.front().vertices.size() == 6);
    object.partCullBack = {true};
    CHECK_THROWS_AS(preview.open(device, map), FormatError);
}

TEST_CASE("Town fences draw only the authored side facing the viewer without moving the surface",
          "[legends][preview][native-world-culling][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/town2";
    const auto source = selectionMap(directory);
    for (const std::string_view name : {"XS_FENCE__BA04", "XS_FENCE__BA05", "XS_FENCE__BA06"}) {
        CAPTURE(name);
        const auto found =
            std::ranges::find(source.objects, name, &formats::LegendsMapObject::name);
        REQUIRE(found != source.objects.end());
        auto map = source;
        map.objects = {*found};
        test::FakeRenderDevice device;
        LegendsPreview preview;
        preview.open(device, map);
        preview.drawWorld(device, Mat4{1}, {}, TreeModel::Pass::All, -1);
        REQUIRE(device.draws.size() == 1);
        const auto& draw = device.draws.front();
        REQUIRE(draw.vertices.size() == 12);
        REQUIRE(draw.state.cullBack);
        CHECK(draw.state.depthTest);
        CHECK(draw.state.depthWrite);
        CHECK(draw.transform == Mat4{1}); // No depth/position nudge hides the problem.
        const Vec3 a = draw.vertices[0].position;
        const Vec3 b = draw.vertices[1].position;
        const Vec3 c = draw.vertices[2].position;
        const Vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const Vec3 centre = (a + b + c) / 3.0f;
        std::array<std::vector<usize>, 2> visible;
        for (usize side = 0; side < 2; ++side) {
            const Vec3 eye = centre + normal * (side == 0 ? 10.0f : -10.0f);
            for (usize i = 0; i < draw.vertices.size(); i += 3) {
                const auto p = draw.vertices[i].position;
                const auto q = draw.vertices[i + 1].position;
                const auto r = draw.vertices[i + 2].position;
                const f32 signedFacing = glm::dot(glm::cross(q - p, r - p), eye - p);
                if (!draw.state.cullBack || signedFacing > 0)
                    visible[side].push_back(i);
            }
            REQUIRE(visible[side].size() == 2);
        }
        CHECK(visible[0] == std::vector<usize>{0, 3});
        CHECK(visible[1] == std::vector<usize>{6, 9});
        CHECK(draw.vertices[0].color != draw.vertices[6].color);
        // Each side's first triangle is the same plane with opposite winding:
        // both still exist, but cannot compete for depth from either viewpoint.
        for (usize i = 6; i < 9; ++i)
            CHECK(std::abs(glm::dot(draw.vertices[i].position - a, normal)) < 0.00001f);
    }
}

TEST_CASE("Native scenery can be picked and highlighted without GameCube layout objects",
          "[legends][preview][selection]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    auto map = triangleMap();
    auto upper = map.objects.front();
    upper.name = "upper";
    upper.transform = glm::translate(upper.transform, Vec3{0, 3, 0});
    map.objects.push_back(upper);
    preview.open(device, map);
    const Vec3 eye{4, 20, 8};
    const auto hit = preview.pickObject(eye, Vec3{0, -1, 0}, preview.camera(), -1);
    REQUIRE(hit);
    CHECK(hit->object.id == 1);
    CHECK(hit->object.name == "upper");
    CHECK(hit->distance == Approx(12));
    CHECK_FALSE(preview.pickObject(Vec3{40, 20, 80}, Vec3{0, -1, 0}, preview.camera(), -1));
    CHECK_FALSE(preview.pickObject(eye, Vec3{0}, preview.camera(), -1));
    CHECK_FALSE(preview.object(-1));
    CHECK_FALSE(preview.object(2));
    preview.highlightObject(device, Mat4{1}, preview.camera(), -1, 1, SelectionHighlight{eye});
    REQUIRE(device.draws.size() == 2);
    CHECK_FALSE(device.draws.back().state.depthWrite);
    CHECK(device.draws.back().vertices.front().position.y > 8);
    preview.close();
    CHECK_FALSE(preview.object(0));
    CHECK_FALSE(preview.pickObject(eye, Vec3{0, -1, 0}, preview.camera(), -1));
}

TEST_CASE("Hidden native objects do not intercept selection or draw a highlight",
          "[legends][preview][selection]") {
    auto map = triangleMap();
    auto hidden = map.objects.front();
    hidden.name = "hidden";
    hidden.transform = glm::translate(hidden.transform, Vec3{0, 3, 0});
    hidden.nativeDrawFlags = 2;
    map.objects.push_back(std::move(hidden));
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    const Vec3 eye{4, 20, 8};
    const auto hit = preview.pickObject(eye, {0, -1, 0}, preview.camera(), .5f);
    REQUIRE(hit);
    CHECK(hit->object.id == 0);
    REQUIRE(preview.object(1));
    CHECK_FALSE(preview.object(1)->visible);
    preview.highlightObject(device, Mat4{1}, preview.camera(), .5f, 1, SelectionHighlight{eye});
    CHECK(device.draws.empty());
}

TEST_CASE("Native billboard selection uses the rendered camera-facing triangle",
          "[legends][preview][selection]") {
    auto map = triangleMap();
    auto& object = map.objects.front();
    object.nativeDrawFlags = 0x09001800;
    object.mesh.vertices[1].position = {4, 0, 0};
    object.mesh.vertices[2].position = {0, 6, 0};
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    for (const Vec3 eye : {Vec3{20, 20, -30}, Vec3{-30, 25, 20}}) {
        WorldCamera camera;
        camera.position = eye;
        camera.yaw = .2f;
        camera.pitch = .5f;
        device.draws.clear();
        preview.drawWorld(device, Mat4{1}, camera, TreeModel::Pass::All, .5f);
        REQUIRE(device.draws.size() == 1);
        const auto rendered = device.draws.front();
        CHECK(rendered.transform != Mat4{1});
        checkSelectionPose(preview, device, camera, .5f, 0, rendered);
    }
}

TEST_CASE("Native scrolling texture masks remain aligned in selection highlights",
          "[legends][preview][selection][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    auto map = selectionMap(directory);
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto texture =
        std::ranges::find(map.textures, std::string{"LAVAFLOW"}, &formats::LegendsMapTexture::name);
    REQUIRE(texture != map.textures.end());
    const auto slot = static_cast<u32>(texture - map.textures.begin());
    const auto object = std::ranges::find_if(map.objects, [&](const auto& entry) {
        return std::ranges::any_of(entry.mesh.parts,
                                   [&](const auto& part) { return part.texture == slot; });
    });
    REQUIRE(object != map.objects.end());
    const auto index = static_cast<s32>(object - map.objects.begin());
    const auto selected = std::ranges::find(object->mesh.parts, slot, &MeshPart::texture);
    const auto part = *selected;
    REQUIRE(object->partCullBack.size() == object->mesh.parts.size());
    const bool cullBack =
        object->partCullBack[static_cast<usize>(selected - object->mesh.parts.begin())];
    for (auto& entry : map.objects) {
        entry.mesh.parts.clear();
        entry.partCullBack.clear();
    }
    object->mesh.parts.push_back(part);
    object->partCullBack.push_back(cullBack);
    auto* motion = adapter.get();
    LegendsPreview preview;
    preview.open(device, map, std::move(adapter));
    for (s32 tick = 0; tick < 3; ++tick) {
        motion->update(1.0f / 30);
        const auto view = motion->texture(slot);
        REQUIRE(view.texture);
        REQUIRE(view.offset != Vec2{0});
        const Vec2 dimensions{static_cast<f32>(texture->width), static_cast<f32>(texture->height)};
        REQUIRE(dimensions.x > 0);
        REQUIRE(dimensions.y > 0);
        device.draws.clear();
        preview.highlightObject(device, Mat4{1}, preview.camera(), -1, index,
                                SelectionHighlight{preview.camera().position});
        REQUIRE(device.draws.size() == 2);
        const auto& wash = device.draws.back();
        CHECK(wash.texture == view.texture);
        REQUIRE(wash.vertices.size() == part.indices.size());
        for (usize i = 0; i < part.indices.size(); ++i) {
            const Vec2 expected =
                object->mesh.vertices[part.indices[i]].uv / dimensions * view.scale + view.offset;
            CHECK(glm::distance(wash.vertices[i].uv, expected) < .00001f);
        }
    }
}

TEST_CASE("Native moving-platform selection shares the displayed interpolation pose",
          "[legends][preview][selection][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/castle3";
    auto map = selectionMap(directory);
    const auto switches = formats::decodeLegendsN64Switches(
        readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
    const auto plate = std::ranges::find(switches.switches, std::string{"TRAPW16"},
                                         &formats::LegendsN64Switch::target);
    REQUIRE(plate != switches.switches.end());
    const LegendsWorldMotion support{map, switches.switches, 4};
    const auto feet = support.platePosition(plate->nativeIndex);
    REQUIRE(feet);
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 4);
    const auto object =
        std::ranges::find(map.objects, plate->target, &formats::LegendsMapObject::name);
    REQUIRE(object != map.objects.end());
    const auto index = static_cast<usize>(object - map.objects.begin());
    // Isolate one real native face, retaining its native object identity,
    // authored placement, culling and switch-driven motion rather than a fake offset.
    std::optional<MeshPart> face;
    bool cullBack = false;
    REQUIRE(object->partCullBack.size() == object->mesh.parts.size());
    for (usize partIndex = 0; partIndex < object->mesh.parts.size(); ++partIndex) {
        const auto& part = object->mesh.parts[partIndex];
        for (usize i = 0; i + 2 < part.indices.size() && !face; i += 3) {
            const auto& a = object->mesh.vertices[part.indices[i]].position;
            const auto& b = object->mesh.vertices[part.indices[i + 1]].position;
            const auto& c = object->mesh.vertices[part.indices[i + 2]].position;
            if (glm::length(glm::cross(b - a, c - a)) > .001f) {
                face = MeshPart{
                    .texture = part.texture,
                    .indices = {part.indices[i], part.indices[i + 1], part.indices[i + 2]}};
                cullBack = object->partCullBack[partIndex];
            }
        }
    }
    REQUIRE(face);
    for (auto& entry : map.objects) {
        entry.mesh.parts.clear();
        entry.partCullBack.clear();
    }
    object->mesh.parts.push_back(*face);
    object->partCullBack.push_back(cullBack);
    auto* motion = adapter.get();
    LegendsPreview preview;
    preview.open(device, map, std::move(adapter));
    motion->touchSwitches(std::array{LegendsPortalVisitor{0, *feet, true, .1f, 3}});
    for (s32 tick = 0; tick < 120; ++tick) {
        motion->capturePresentation();
        motion->update(1.0f / 60);
        if (motion->worldObjectOffset(index, 0) != motion->worldObjectOffset(index, 1)) {
            break;
        }
    }
    REQUIRE(motion->worldObjectOffset(index, 0) != motion->worldObjectOffset(index, 1));
    const Vec3 authored{object->transform *
                        Vec4{object->mesh.vertices[face->indices[0]].position, 1}};
    const auto camera = preview.camera();
    for (const f32 alpha : {0.0f, .25f, .75f, 1.0f}) {
        device.draws.clear();
        preview.drawWorld(device, Mat4{1}, camera, TreeModel::Pass::All, alpha);
        const auto found = std::ranges::find_if(device.draws, [&](const auto& draw) {
            return draw.vertices.size() == 3 && draw.vertices.front().position == authored;
        });
        REQUIRE(found != device.draws.end());
        const auto rendered = *found;
        CHECK(glm::distance(Vec3{rendered.transform * Vec4{authored, 1}},
                            authored + motion->worldObjectOffset(index, alpha)) < .001f);
        checkSelectionPose(preview, device, camera, alpha, static_cast<s32>(index), rendered);
    }
}

TEST_CASE("Legends map preview uses native placement and keeps the game-independent fly camera",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    CHECK_FALSE(preview.isOpen());
    auto map = triangleMap();
    preview.open(device, map);
    CHECK(preview.isOpen());
    CHECK(preview.triangleCount() == 1);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    REQUIRE(draw.vertices.size() == 3);
    CHECK(draw.vertices[0].position == Vec3{3, 5, 7});
    CHECK(draw.vertices[1].position == Vec3{13, 5, 7});
    CHECK(draw.vertices[2].position == Vec3{3, 5, 17});
    CHECK(draw.texture == &device.whiteTexture());
    CHECK(draw.state.depthTest);
    CHECK(draw.state.depthWrite);
    CHECK_FALSE(draw.state.cullBack);

    const Vec3 centre{8, 5, 12};
    CHECK(glm::dot(glm::normalize(centre - preview.camera().position),
                   preview.camera().forward()) == Approx(1).margin(0.0001));
    Input input;
    input.beginPoll();
    input.setKey(Key::W, true);
    const auto start = preview.camera();
    preview.update(input, 0.1f, true);
    CHECK(preview.camera() == start);
    preview.update(input, 0.1f, false);
    CHECK(preview.camera().position != start.position);
    preview.close();
    CHECK_FALSE(preview.isOpen());
    CHECK(preview.triangleCount() == 0);
    device.draws.clear();
    render(preview, device);
    CHECK(device.draws.empty());
}

TEST_CASE("Legends preview rejects malformed replacements without losing its current map",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, triangleMap());
    const auto camera = preview.camera();
    auto map = triangleMap();
    SECTION("invalid index") {
        map.objects[0].mesh.parts[0].indices[1] = 3;
    }
    SECTION("incomplete triangle") {
        map.objects[0].mesh.parts[0].indices.pop_back();
    }
    SECTION("nonfinite position") {
        map.objects[0].mesh.vertices[0].position.x = std::numeric_limits<f32>::infinity();
    }
    SECTION("nonfinite texture coordinate") {
        map.objects[0].mesh.vertices[0].uv.y = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("finite coordinates overflow the camera bounds") {
        map.objects[0].mesh.vertices[0].position.x = std::numeric_limits<f32>::max();
    }
    SECTION("no geometry") {
        map.objects.clear();
    }
    SECTION("invalid texture byte count") {
        map.textures.push_back({.image = {.width = 4, .height = 4, .pixels = {255}}});
    }
    SECTION("singular billboard placement") {
        map.objects[0].nativeDrawFlags = 0x01000000;
        map.objects[0].transform[0] = Vec4{0};
    }
    CHECK_THROWS_AS(preview.open(device, map), FormatError);
    CHECK(preview.isOpen());
    CHECK(preview.triangleCount() == 1);
    CHECK(preview.camera() == camera);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices[0].position == Vec3{3, 5, 7});
}

TEST_CASE("Legends scenery keeps separate native objects when they share a texture",
          "[legends][preview]") {
    auto map = triangleMap();
    auto second = map.objects.front();
    second.name = "second";
    second.transform = glm::translate(Mat4{1}, Vec3{30, 5, 7});
    map.objects.push_back(std::move(second));
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].texture == device.draws[1].texture);
    CHECK(device.draws[0].vertices[0].position == Vec3{3, 5, 7});
    CHECK(device.draws[1].vertices[0].position == Vec3{30, 5, 7});
    CHECK(preview.triangleCount() == 2);
}

TEST_CASE(
    "Legends preview uses native texture slots and texel coordinates without borrowing GC flags",
    "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    auto map = triangleMap();
    map.textures.push_back({.image = Image::filled(2, 2, Color::white()), .clampU = true});
    auto& image = map.textures[0].image;
    SECTION("opaque") {}
    SECTION("binary alpha") {
        image.pixels[3] = 0;
    }
    SECTION("soft alpha") {
        image.pixels[3] = 127;
    }
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws[0];
    REQUIRE(draw.texture != &device.whiteTexture());
    CHECK(draw.texture->width() == 2);
    CHECK(draw.vertices[1].uv.x == Approx(1));
    CHECK(draw.vertices[2].uv.y == Approx(1));
    CHECK(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    CHECK(device.lastTextureDesc.wrapDown() == TextureWrap::Repeat);
    CHECK(device.lastTextureDesc.generateMipmaps);
    CHECK(draw.state.alphaTest == (image.pixels[3] < 255 ? DrawState::kTranslucentAlphaTest : 0));
    CHECK(draw.state.depthWrite == (image.pixels[3] != 127));
    CHECK(draw.state.blend == (image.pixels[3] == 127 ? BlendMode::Alpha : BlendMode::Opaque));
}

TEST_CASE("N64 soft scenery preserves authored depth writes after opaque actors",
          "[legends][preview][world-presentation]") {
    auto map = triangleMap();
    map.objects.front().hasNativeDrawState = true;
    map.textures.push_back({.image = Image::filled(2, 2, Color::rgba(100, 150, 200, 128))});
    bool writesDepth = true;
    SECTION("native waterfall default") {}
    SECTION("explicit no-Z-update") {
        map.objects.front().nativeDrawFlags = 0x80;
        writesDepth = false;
    }
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    WorldCamera view;
    preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Opaque, -1);
    CHECK(device.draws.empty());
    preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Blended, -1);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].state.depthTest);
    CHECK(device.draws[0].state.depthWrite == writesDepth);
    CHECK(device.draws[0].state.blend == BlendMode::Alpha);
    CHECK_FALSE(device.draws[0].state.lightmapRgb);
}

TEST_CASE("Dreamcast torch planes retain additive depth state and native camera facing",
          "[legends][preview][dreamcast-presentation]") {
    auto map = triangleMap();
    auto& object = map.objects.front();
    object.hasNativeDrawState = true;
    object.dreamcastDrawState = true;
    object.nativeDrawFlags = 0x01c01880;
    object.partLightmaps = {1};
    map.lightmaps.push_back({1, Image::filled(2, 2, Color::rgba(128, 64, 32))});
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    WorldCamera view;
    view.yaw = 0.8f;
    view.pitch = 0.4f;
    preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Opaque, -1);
    CHECK(device.draws.empty());
    preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Blended, -1);
    REQUIRE(device.draws.size() == 1);
    const auto draw = device.draws.front();
    CHECK(draw.state.blend == BlendMode::Additive);
    CHECK(draw.state.depthTest);
    CHECK_FALSE(draw.state.depthWrite);
    CHECK_FALSE(draw.state.lightmapRgb);
    CHECK(draw.state.lightmap == nullptr);
    const Mat4 expected = CameraFrame::of(view).face(object.transform, 1);
    for (const auto& vertex : draw.vertices) {
        CHECK(glm::length(draw.transform * Vec4{vertex.position, 1} -
                          expected * Vec4{vertex.position, 1}) < .001f);
    }
    checkSelectionPose(preview, device, view, -1, 0, draw);
}

TEST_CASE("Dreamcast facing effects can retain planar and collapsed native poses",
          "[legends][preview][dreamcast-presentation]") {
    auto map = triangleMap();
    auto& object = map.objects.front();
    object.hasNativeDrawState = true;
    object.dreamcastDrawState = true;
    object.nativeDrawFlags = 0x01c01880;
    SECTION("zero-thickness authored plane") {
        object.transform = glm::scale(object.transform, Vec3{1, 1, 0});
    }
    SECTION("collapsed animation key") {
        object.transform = glm::scale(object.transform, Vec3{0});
    }
    test::FakeRenderDevice device;
    LegendsPreview preview;
    REQUIRE_NOTHROW(preview.open(device, map));
    const WorldCamera view;
    preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Blended, -1);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    const Mat4 expected = CameraFrame::of(view).face(object.transform, 1);
    for (const auto& vertex : draw.vertices) {
        CHECK(glm::length(draw.transform * Vec4{vertex.position, 1} -
                          expected * Vec4{vertex.position, 1}) < .001f);
    }
}

TEST_CASE("N64 facing scenery rotates around its own anchor without moving the world",
          "[legends][preview][world-presentation]") {
    auto map = triangleMap();
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto fixed = device.draws.front();
    const auto camera = preview.camera();
    device.draws.clear();
    map.objects[0].nativeDrawFlags = 0x09001800;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    const auto& faced = device.draws.front();
    CHECK(preview.camera() == camera);
    CHECK(faced.vertices == fixed.vertices);
    CHECK(faced.transform != fixed.transform);
    const Vec4 anchor{3, 5, 7, 1};
    CHECK(glm::length(faced.transform * anchor - fixed.transform * anchor) < .001f);
    CHECK(glm::length(faced.transform * Vec4{13, 5, 7, 1} - fixed.transform * Vec4{13, 5, 7, 1}) >
          .1f);
}

TEST_CASE("Native Mountain Valley water and mist retain depth-tested translucent draws",
          "[legends][preview][world-presentation][native-assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    for (auto& texture : formats::decodeLegendsN64Textures(objects)) {
        map.textures.push_back(
            {std::move(texture.image), texture.clampU, texture.clampV, texture.hasTransparency});
    }
    for (const auto& object : map.objects) {
        REQUIRE(object.hasNativeDrawState);
        // Native e001ea34 only disables Z_UPD/Z_CMP for these explicit bits.
        REQUIRE((object.nativeDrawFlags & 0xc0) == 0);
    }
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    preview.drawWorld(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::Blended, -1);
    usize translucent = 0;
    for (const auto& draw : device.draws) {
        CHECK(draw.state.depthTest);
        CHECK(draw.state.depthWrite);
        translucent += draw.state.blend == BlendMode::Alpha;
    }
    CHECK(translucent > 0);
}

TEST_CASE("N64 world draw flags do not borrow collision flag semantics",
          "[legends][preview][world-presentation]") {
    auto map = triangleMap();
    map.objects.front().flags = 2; // Not the native drawable's hidden bit.
    map.objects.front().nativeDrawFlags = 0x100000c0;
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    render(preview, device);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().state.blend == BlendMode::Alpha);
    CHECK_FALSE(device.draws.front().state.depthWrite);
    CHECK_FALSE(device.draws.front().state.depthTest);
    device.draws.clear();
    map.objects.front().nativeDrawFlags |= 2;
    preview.open(device, map);
    render(preview, device);
    CHECK(device.draws.empty());
}

TEST_CASE("Native decal submission separates a coplanar surface but retains near and far occlusion",
          "[legends][preview][world-presentation][native-decals]") {
    auto map = triangleMap();
    auto& object = map.objects.front();
    object.flags = 0x18000001;
    object.nativeDrawFlags = 0x08001800;
    object.hasNativeDrawState = true;
    map.textures.push_back({.image = Image::filled(2, 2, Color::rgba(100, 150, 200, 128))});
    const auto originalMesh = object.mesh;
    const auto originalTransform = object.transform;
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, map);
    const Vec3 centre = Vec3{object.transform * Vec4{10.0f / 3, 0, 10.0f / 3, 1}};
    for (const f32 distance : {8.0f, 80.0f, 500.0f}) {
        CAPTURE(distance);
        WorldCamera view;
        view.pitch = .65f;
        view.yaw = -.3f;
        view.position = centre - view.forward() * distance;
        const Mat4 clip = WorldCamera::projection(kPi / 2, 16.0f / 9) * view.view();
        device.draws.clear();
        preview.drawWorld(device, clip, view, TreeModel::Pass::Blended, -1);
        REQUIRE(device.draws.size() == 1);
        const auto& draw = device.draws.front();
        REQUIRE(draw.state.depthTest);
        REQUIRE(draw.state.depthWrite);
        REQUIRE(draw.state.blend == BlendMode::Alpha);
        const auto depth = [](const Vec4& p) { return p.z / p.w; };
        const f32 original = depth(clip * Vec4{centre, 1});
        const f32 adjusted = depth(draw.transform * Vec4{centre, 1});
        const f32 nearerOccluder = depth(clip * Vec4{centre - view.forward() * 2.0f, 1});
        const f32 behind = depth(clip * Vec4{centre + view.forward() * 2.0f, 1});
        // The renderer uses reverse depth (GEQUAL): a coplanar decal wins,
        // but geometry genuinely closer than its native pull still occludes it.
        CHECK(adjusted > original);
        CHECK(adjusted > behind);
        CHECK(adjusted < nearerOccluder);
        CHECK(std::isfinite(adjusted));
        CHECK(adjusted < WorldCamera::kDepthRange);
        // Inspection and collision coordinates stay authored; rendering cannot
        // move the selectable object toward whichever camera is looking at it.
        const auto hit = preview.pickObject(centre + Vec3{0, 10, 0}, {0, -1, 0}, view, -1);
        REQUIRE(hit);
        CHECK(hit->distance == Approx(10));
        CHECK(draw.vertices.front().position ==
              Vec3{originalTransform * Vec4{originalMesh.vertices.front().position, 1}});
    }
    // Identical high flag bits in another platform's world metadata are not a
    // native N64 material directive.
    map.objects.front().hasNativeDrawState = false;
    preview.open(device, map);
    device.draws.clear();
    preview.drawWorld(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::Blended, -1);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().transform == Mat4{1});
}

TEST_CASE("Native Skorne cracked floors and window projections retain their authored decal policy",
          "[legends][preview][world-presentation][native-decals][native-assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted/levels";
    const std::array<std::pair<const char*, const char*>, 3> objects{{
        {"boss2", "XS_CRACKEDOVERLAY__DC05"},
        {"boss1", "NSXS_WINDOWFLOOR__DC_4_693"},
        {"boss1", "NSXS_WINDOWLIGHT17__NC_695"},
    }};
    for (const auto& [level, name] : objects) {
        CAPTURE(level, name);
        auto map = selectionMap(root / level);
        const auto found = std::find_if(map.objects.begin(), map.objects.end(),
                                        [&](const auto& object) { return object.name == name; });
        REQUIRE(found != map.objects.end());
        const bool decal = (found->flags & 0x10000000) != 0;
        CHECK(decal == (std::string_view{name} != "NSXS_WINDOWLIGHT17__NC_695"));
        CHECK(found->hasNativeDrawState);
        const auto object = *found;
        map.objects = {object};
        test::FakeRenderDevice device;
        LegendsPreview preview;
        preview.open(device, map);
        for (const f32 distance : {12.0f, 160.0f}) {
            CAPTURE(distance);
            WorldCamera view;
            view.pitch = .6f;
            view.yaw = -.2f;
            view.position = Vec3{object.transform[3]} - view.forward() * distance;
            device.draws.clear();
            preview.drawWorld(device, Mat4{1}, view, TreeModel::Pass::Blended, -1);
            REQUIRE_FALSE(device.draws.empty());
            for (const auto& draw : device.draws) {
                CHECK(draw.state.depthTest);
                CHECK(draw.state.depthWrite);
                const Vec3 delta{draw.transform[3]};
                CHECK(glm::dot(delta, view.forward()) == Approx(decal ? -1.5f : 0).margin(.001f));
                for (const auto& vertex : draw.vertices) {
                    CHECK(std::isfinite(vertex.position.y));
                    const auto source =
                        std::find_if(object.mesh.vertices.begin(), object.mesh.vertices.end(),
                                     [&](const auto& original) {
                                         return Vec3{object.transform *
                                                     Vec4{original.position, 1}} == vertex.position;
                                     });
                    REQUIRE(source != object.mesh.vertices.end());
                }
            }
        }
    }
}

TEST_CASE("Legends preview does not divide by a minimized viewport or invalid field of view",
          "[legends][preview]") {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.open(device, triangleMap());
    preview.render(device, Mat4{1}, 640, 0, kPi / 2);
    preview.render(device, Mat4{1}, 0, 448, kPi / 2);
    preview.render(device, Mat4{1}, 640, 448, 0);
    preview.render(device, Mat4{1}, 640, 448, kPi);
    CHECK(device.draws.empty());
}

TEST_CASE("Legends walking renders the selected native character and holds input under the console",
          "[legends][preview][assets]") {
    const auto root = test::assetOrSkip("PLAYERS").parent_path();
    auto map = triangleMap();
    map.collision = {{.vertices = {Vec3{-20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, -20}},
                      .owner = 0,
                      .floor = true},
                     {.vertices = {Vec3{20, 0, -20}, Vec3{-20, 0, 20}, Vec3{20, 0, 20}},
                      .owner = 0,
                      .floor = true}};
    map.starts.push_back({.position = {0, 0, 0}});
    PartyMember member;
    member.save.name = "TEST";
    test::FakeRenderDevice device;
    LegendsPreview preview;
    preview.openPlayable(device, map, root, member);
    REQUIRE(preview.playable());
    REQUIRE(preview.traversal());
    const auto initial = preview.traversal()->actor().position();
    Input input;
    input.beginPoll();
    input.setKey(Key::W, true);
    const GameConfig config;
    preview.updatePlayable(input, 0.1f, true, config);
    CHECK(preview.traversal()->actor().position() == initial);
    for (s32 frame = 0; frame < 60; ++frame) {
        preview.updatePlayable(input, 1.0f / 60.0f, false, config);
    }
    CHECK(glm::distance(preview.traversal()->actor().position(), initial) > 1);
    render(preview, device);
    CHECK(device.draws.size() > 1);
    CHECK(member.save.name == "TEST");
    preview.open(device, triangleMap());
    CHECK_FALSE(preview.playable());
    CHECK(preview.traversal() == nullptr);
    preview.close();
    CHECK_FALSE(preview.isOpen());
}

TEST_CASE("native Legends maps reach the renderer without conversion or GameCube level state",
          "[legends][preview][assets]") {
    const auto assets = test::dataDirectory().parent_path() / "assets";
    const auto survey = [&](const std::filesystem::path& levels, bool n64, usize expected) {
        if (!std::filesystem::is_directory(levels)) {
            SKIP("native Legends extraction is not installed");
        }
        test::FakeRenderDevice device;
        LegendsPreview preview;
        usize count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(levels)) {
            const auto world = entry.path() / (n64 ? "worlds.wad" : "worlds.rom");
            const auto models = entry.path() / (n64 ? "objects.wad" : "objects.rom");
            if (!std::filesystem::is_regular_file(world) ||
                !std::filesystem::is_regular_file(models) ||
                std::filesystem::file_size(world) == 0) {
                continue;
            }
            INFO(entry.path().string());
            const auto modelBytes = readFile(models);
            auto map = n64 ? formats::LegendsMap::parseN64(readFile(world), modelBytes)
                           : formats::LegendsMap::parseDreamcast(readFile(world), modelBytes);
            if (n64) {
                for (auto& texture : formats::decodeLegendsN64Textures(modelBytes)) {
                    map.textures.push_back({std::move(texture.image), texture.clampU,
                                            texture.clampV, texture.hasTransparency});
                }
            }
            if (!n64) {
                map.textures = formats::decodeLegendsDreamcastTextures(
                    modelBytes, readFile(entry.path() / "textures.rom"));
            }
            preview.open(device, map);
            REQUIRE(preview.isOpen());
            CHECK(preview.triangleCount() > 0);
            device.draws.clear();
            render(preview, device);
            usize submitted = 0;
            for (const auto& draw : device.draws) {
                submitted += draw.vertices.size() / 3;
                for (int column = 0; column < 4; ++column) {
                    for (int row = 0; row < 4; ++row) {
                        REQUIRE(std::isfinite(draw.transform[column][row]));
                    }
                }
            }
            // Native hidden drawables and explicit no-Z-test effects are not
            // survey defaults. Count the visible authored geometry, while the
            // focused material tests verify each depth and blend mode.
            usize visibleTriangles = 0;
            for (const auto& object : map.objects)
                if ((object.nativeDrawFlags & 2U) == 0)
                    visibleTriangles += object.mesh.triangleCount();
            CHECK(submitted == visibleTriangles);
            ++count;
        }
        CHECK(count == expected);
    };
    SECTION("Nintendo 64") {
        survey(assets / "n64/extracted/levels", true, 34);
    }
    SECTION("Dreamcast") {
        survey(assets / "dreamcast/extracted/native/levels", false, 41);
    }
}
} // namespace
