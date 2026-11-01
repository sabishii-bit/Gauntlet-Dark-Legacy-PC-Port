#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Gates.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct FixtureScene {
    test::FakeRenderDevice device;
    LegendsPreview preview;
    std::filesystem::path directory;

    explicit FixtureScene(std::string_view stage) {
        directory = test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / stage;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        preview.open(device, map, std::move(adapter));
    }

    std::vector<ImportedWorldObject> named(std::string_view prefix) {
        std::vector<ImportedWorldObject> result;
        for (usize i = 0; i < preview.adapter()->fixtureCount(); ++i) {
            const auto object =
                preview.object(LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
            if (object && object->visible && object->name.starts_with(prefix)) {
                result.push_back(*object);
            }
        }
        return result;
    }

    bool pick(s32 id, f32 alpha = -1) {
        const auto object = preview.object(id);
        REQUIRE(object);
        WorldCamera view;
        view.position = object->position + Vec3{12, 16, -20};
        view.pitch = .6f;
        device.draws.clear();
        const usize resources = device.texturesCreated;
        preview.highlightObject(device, Mat4{1}, view, alpha, id, SelectionHighlight{Vec3{0}});
        const auto highlights = device.draws;
        CHECK(device.texturesCreated == resources);
        CHECK(highlights.size() % 2 == 0);
        for (usize draw = 1; draw < highlights.size(); draw += 2) {
            const auto& vertices = highlights[draw].vertices;
            for (usize i = 0; i + 2 < vertices.size(); i += 3) {
                std::array<Vec3, 3> points;
                for (usize corner = 0; corner < 3; ++corner) {
                    points[corner] =
                        vertices[i + corner].position / (1 - SelectionHighlight::kLift);
                }
                Vec3 normal = glm::cross(points[1] - points[0], points[2] - points[0]);
                if (glm::length(normal) < 1e-5f) {
                    continue;
                }
                normal = glm::normalize(normal);
                const Vec3 centre = (points[0] + points[1] + points[2]) / 3.0f;
                const auto hit = preview.pickObject(centre + normal * .02f, -normal, view, alpha);
                if (hit && hit->object.id == id) {
                    CHECK(hit->object.name == object->name);
                    CHECK(hit->distance <= .021f);
                    CHECK(device.texturesCreated == resources);

                    // The highlight is the real posed draw with the same lender
                    // texture and final UV transform, not a body-volume proxy.
                    device.draws.clear();
                    preview.adapter()->draw(device, Mat4{1}, CameraFrame::of(view),
                                            TreeModel::Pass::All, alpha);
                    bool original = false;
                    for (const auto& submitted : device.draws) {
                        if (submitted.texture != highlights[draw].texture) {
                            continue;
                        }
                        for (const auto& vertex : submitted.vertices) {
                            const Vec3 position{submitted.transform * Vec4{vertex.position, 1}};
                            const Vec2 uv =
                                vertex.uv * submitted.state.uvScale + submitted.state.uvOffset;
                            if (glm::distance(position, points[0]) < .001f &&
                                glm::distance(uv, vertices[i].uv) < .001f) {
                                original = true;
                                break;
                            }
                        }
                        if (original) {
                            break;
                        }
                    }
                    CHECK(original);
                    return true;
                }
            }
        }
        return false;
    }

    s32 pickNamed(std::string_view prefix) {
        const auto candidates = named(prefix);
        REQUIRE_FALSE(candidates.empty());
        for (const auto& candidate : candidates) {
            if (pick(candidate.id)) {
                return candidate.id;
            }
        }
        FAIL("No visible native fixture triangle was selectable: " << prefix);
        return -1;
    }
};
} // namespace

TEST_CASE("Native fixture selection reaches traps generators containers and locked gates",
          "[legends][fixture-selection][assets]") {
    FixtureScene fixture{"mount1"};
    fixture.pickNamed("GENERATOR:");
    fixture.pickNamed("CONTAINER:");
    fixture.pickNamed("TRAP:");
    const auto gates = formats::decodeLegendsN64Gates(
        readFile(fixture.directory / "ipr.rom"),
        readFile(fixture.directory.parent_path().parent_path() / "items/generic/ptrap.rom"));
    REQUIRE_FALSE(gates.gates.empty());
    fixture.pickNamed(formats::legendsN64TreeName("TRAP", gates.gates.front().idleSequence));

    const auto& preview = fixture.preview;
    CHECK_FALSE(preview.object(-1));
    CHECK_FALSE(preview.object(std::numeric_limits<s32>::max()));
    CHECK_FALSE(preview.pickObject(Vec3{0}, Vec3{0}, WorldCamera{}, -1));
    CHECK_FALSE(
        preview.pickObject(Vec3{std::numeric_limits<f32>::infinity()}, Vec3{1}, WorldCamera{}, -1));
}

TEST_CASE("Tower doors and boss arena barriers use visible fixture geometry for selection",
          "[legends][fixture-selection][assets]") {
    SECTION("locked Tower realm doors") {
        FixtureScene fixture{"tower2"};
        fixture.pickNamed("DOORS:");
    }
    SECTION("Dragon arena cover") {
        FixtureScene fixture{"mount6"};
        const s32 id = fixture.pickNamed("SAFEROCK:");
        const auto before = fixture.preview.object(id);
        REQUIRE(before);
        const auto targets = fixture.preview.adapter()->arenaTargets();
        const auto target = std::ranges::find_if(targets, [&](const auto& value) {
            return glm::distance(Vec3{value.placement[3]}, before->position) < .01f;
        });
        REQUIRE(target != targets.end());
        REQUIRE(fixture.preview.adapter()->strikeArenaRock(target->index, 40));
        CHECK(fixture.preview.object(id)->visible);
        CHECK(fixture.preview.object(id)->name == before->name);
        REQUIRE(fixture.pick(id));
    }
}

TEST_CASE("Native generator selection survives authored damage-state replacement",
          "[legends][fixture-selection][assets]") {
    FixtureScene fixture{"mount1"};
    auto& adapter = *fixture.preview.adapter();
    const s32 id = fixture.pickNamed("GENERATOR:");
    const auto object = fixture.preview.object(id);
    REQUIRE(object);
    const auto definitions = adapter.generators();
    const auto generator = std::ranges::find_if(definitions, [&](const auto& value) {
        return glm::distance(value.position, object->position) < .01f;
    });
    REQUIRE(generator != definitions.end());
    const std::array state{
        ImportedGeneratorState{generator->nativeIndex, 0, 1, generator->position, true}};
    adapter.capturePresentation();
    adapter.updateGenerators(state);
    adapter.update(1.0f / 30);
    REQUIRE(fixture.preview.object(id)->visible);
    CHECK(fixture.preview.object(id)->name == object->name);
    REQUIRE(fixture.pick(id, .5f));
    const std::array removed{
        ImportedGeneratorState{generator->nativeIndex, 0, 0, generator->position, false}};
    adapter.updateGenerators(removed);
    // A ruin may remain rendered; whichever authored variant remains still has
    // the original logical ID, never the replacement's vector position.
    if (fixture.preview.object(id)->visible) {
        REQUIRE(fixture.pick(id));
    }
}
