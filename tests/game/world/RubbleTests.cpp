#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/Rubble.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("rubble is the named mesh of the first archive that has it, left where it stood",
          "[game][world][rubble]") {
    const auto root = test::scratchDirectory("rubble");
    writeTextFile(root / "tri.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"BAREXP0","file":"tri.obj","meshTriangles":1}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(
        root / "textures.json",
        R"({"bitmaps":[{"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"BARREL","nodes":[{"name":"ROOT","object":"BAREXP0","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":1,"frameRate":30}]}]})");
    ItemArchive empty;
    ItemArchive items;
    REQUIRE(items.load(root));
    test::FakeRenderDevice device;
    Rubble rubble;
    const std::array<ItemArchive*, 2> archives{&empty, &items};
    const Mat4 where = glm::translate(Mat4{1}, Vec3{4, 0, 2});
    REQUIRE(rubble.leave(device, archives, Rubble::kBlownBarrel, where));
    CHECK_FALSE(rubble.leave(device, archives, Rubble::kGasBarrel, where)); // none has it
    REQUIRE(rubble.size() == 1);
    CHECK(rubble.transform(0) == where);
    device.draws.clear();
    rubble.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position.x >= 4.0f);
    rubble.clear();
    CHECK(rubble.size() == 0);
}
} // namespace
