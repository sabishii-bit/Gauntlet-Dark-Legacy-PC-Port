#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ObjModel.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr std::string_view kSample = "# comment\n"
                                     "o thing\n"
                                     "v 0 0 0\n"
                                     "v 1 0 0\n"
                                     "v 1 1 0\n"
                                     "v 0 1 0\n"
                                     "vt 0 1\n"
                                     "vt 1 0.5\n"
                                     "vn 0 0 1\n"
                                     "g part0\n"
                                     "usemtl tex7\n"
                                     "f 1/1/1 2/2/1 3/2/1 4/1/1\n"
                                     "usemtl tex9\n"
                                     "f 1//1 3//1 2//1\n";

TEST_CASE("OBJ faces become triangles with per-texture parts", "[assets][obj]") {
    const Mesh mesh = parseObj(kSample);
    REQUIRE(mesh.parts.size() == 2);
    REQUIRE(mesh.parts[0].texture == 7);
    REQUIRE(mesh.parts[0].indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    REQUIRE(mesh.parts[1].texture == 9);
    REQUIRE(mesh.parts[1].indices.size() == 3);
    REQUIRE(mesh.vertices.size() == 7); // four corners with texcoords, three without
    REQUIRE(mesh.vertices[1].position == Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(mesh.vertices[1].uv.x == Approx(1.0f));
    REQUIRE(mesh.vertices[1].uv.y == Approx(0.5f));
    REQUIRE(mesh.vertices[0].uv.y == Approx(0.0f));
    REQUIRE(mesh.vertices[0].normal == Vec3{0.0f, 0.0f, 1.0f});
    REQUIRE(mesh.triangleCount() == 3);
}

TEST_CASE("OBJ lightmap coordinates and materials read back", "[assets][obj]") {
    const Mesh mesh = parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                               "vt 0 1\nvt 1 1\nvt 0 0\n"
                               "vl 0.25 0.5\nvl 0.75 0.5\nvl 0.25 0.25\n"
                               "vn 0 0 1\n"
                               "usemtl tex3_lm7\nf 1/1/1 2/2/1 3/3/1\n"
                               "usemtl texX_lm9\nf 1/1/1 3/3/1 2/2/1\n");
    REQUIRE(mesh.parts.size() == 2);
    REQUIRE(mesh.parts[0].texture == 3);
    REQUIRE(mesh.parts[0].lightmap == 7);
    REQUIRE(mesh.parts[1].texture == 0); // an unreadable index falls back
    REQUIRE(mesh.parts[1].lightmap == 9);
    REQUIRE(mesh.vertices.size() == 3);
    REQUIRE(mesh.vertices[1].uv == Vec2{1.0f, 0.0f});
    REQUIRE(mesh.vertices[1].lightmapUv == Vec2{0.75f, 0.5f});
    REQUIRE(mesh.vertices[2].lightmapUv == Vec2{0.25f, 0.25f});
}

TEST_CASE("OBJ vertex colours read back as a prelit mesh", "[assets][obj]") {
    const Mesh lit = parseObj("v 0 0 0 1 0 0.5\nv 1 0 0 0 1 0\nv 0 1 0 0 0 0\n"
                              "usemtl tex0\nf 1 2 3\n");
    REQUIRE(lit.prelit);
    REQUIRE(lit.vertices.size() == 3);
    REQUIRE(lit.vertices[0].color == Color::rgba(255, 0, 128, 255));
    REQUIRE(lit.vertices[1].color == Color::rgba(0, 255, 0, 255));
    REQUIRE(lit.vertices[2].color == Color::rgba(0, 0, 0, 255));
    const Mesh plain = parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl tex0\nf 1 2 3\n");
    REQUIRE_FALSE(plain.prelit);
    REQUIRE(plain.vertices[0].color == Color::white());
}

TEST_CASE("malformed OBJ input is rejected", "[assets][obj]") {
    REQUIRE_THROWS_AS(parseObj("v 1 2 x\n"), FormatError);
    REQUIRE_THROWS_AS(parseObj("v 0 0 0\nf 1 2 3\n"), FormatError);
    REQUIRE_THROWS_AS(parseObj("v 0 0 0\nf 0 1 1\n"), FormatError);
    REQUIRE(parseObj("").vertices.empty());
}

TEST_CASE("OBJ files load from disk", "[assets][obj]") {
    const auto dir = test::scratchDirectory("obj-model");
    writeTextFile(dir / "thing.obj", kSample);
    REQUIRE(loadObj(dir / "thing.obj").triangleCount() == 3);
    REQUIRE_THROWS_AS(loadObj(dir / "missing.obj"), FileError);
}

} // namespace
