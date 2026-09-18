#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/render/Mesh.h"

#include "formats/ObjWriter.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

TEST_CASE("meshes are written as one OBJ object with a group per texture", "[formats][obj]") {
    Mesh mesh;
    mesh.vertices.push_back(
        MeshVertex{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec2{0.0f, 0.0f}});
    mesh.vertices.push_back(
        MeshVertex{Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec2{1.0f, 0.0f}});
    mesh.vertices.push_back(
        MeshVertex{Vec3{0.0f, 1.0f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f}, Vec2{0.0f, 0.25f}});
    MeshPart part;
    part.texture = 326;
    part.indices = {0, 1, 2};
    mesh.parts.push_back(part);

    const std::string expected = "o ARROW\n"
                                 "v 0 0 0\n"
                                 "v 1 0 0\n"
                                 "v 0 1 0.5\n"
                                 "vt 0 1\n"
                                 "vt 1 1\n"
                                 "vt 0 0.75\n"
                                 "vn 0 0 1\n"
                                 "vn 0 0 1\n"
                                 "vn 0 0 1\n"
                                 "g part0\n"
                                 "usemtl tex326\n"
                                 "f 1/1/1 2/2/2 3/3/3\n";
    REQUIRE(encodeObj(mesh, "ARROW") == expected);
}

TEST_CASE("a prelit mesh writes its vertex colours after the positions", "[formats][obj]") {
    Mesh mesh;
    mesh.prelit = true;
    MeshVertex v;
    v.position = Vec3{1.0f, 2.0f, 3.0f};
    v.color = Color::rgba(255, 0, 128, 255);
    mesh.vertices.push_back(v);
    MeshPart part;
    part.indices = {0, 0, 0};
    mesh.parts.push_back(part);
    const std::string text = encodeObj(mesh, "LIT");
    REQUIRE(text.find("v 1 2 3 1 0 0.502\n") != std::string::npos);
    mesh.prelit = false;
    REQUIRE(encodeObj(mesh, "LIT").find("v 1 2 3\n") != std::string::npos);
}

TEST_CASE("a lightmapped mesh writes its second coordinates and names the lightmap",
          "[formats][obj]") {
    Mesh mesh;
    mesh.vertices.push_back(MeshVertex{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                                       Vec2{0.0f, 0.0f}, Vec2{0.25f, 0.5f}});
    mesh.vertices.push_back(MeshVertex{Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f},
                                       Vec2{1.0f, 0.0f}, Vec2{0.75f, 0.5f}});
    mesh.vertices.push_back(MeshVertex{Vec3{0.0f, 1.0f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f},
                                       Vec2{0.0f, 0.25f}, Vec2{0.25f, 0.25f}});
    MeshPart part;
    part.texture = 326;
    part.lightmap = 507;
    part.indices = {0, 1, 2};
    mesh.parts.push_back(part);

    const std::string expected = "o FLOOR\n"
                                 "v 0 0 0\n"
                                 "v 1 0 0\n"
                                 "v 0 1 0.5\n"
                                 "vt 0 1\n"
                                 "vt 1 1\n"
                                 "vt 0 0.75\n"
                                 "vn 0 0 1\n"
                                 "vn 0 0 1\n"
                                 "vn 0 0 1\n"
                                 "vl 0.25 0.5\n"
                                 "vl 0.75 0.5\n"
                                 "vl 0.25 0.25\n"
                                 "g part0\n"
                                 "usemtl tex326_lm507\n"
                                 "f 1/1/1 2/2/2 3/3/3\n";
    REQUIRE(encodeObj(mesh, "FLOOR") == expected);
}

} // namespace
