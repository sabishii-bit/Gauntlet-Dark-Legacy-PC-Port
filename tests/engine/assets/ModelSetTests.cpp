#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    writeTextFile(dir / "models/000_TRI.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex3\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({
  "source": "/sample/",
  "objects": [
    {"index": 0, "name": "TRI", "file": "models/000_TRI.obj", "meshTriangles": 1},
    {"index": 1, "name": "EMPTY"}
  ]
})");
    return dir;
}

TEST_CASE("a model set finds objects by name and loads their meshes", "[assets][model]") {
    ModelSet set;
    REQUIRE(set.load(sampleSet("model-set")));
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("tri") == 0U);
    REQUIRE(set.entry(0).triangles == 1);
    REQUIRE_FALSE(set.find("nothing").has_value());
    const Mesh& mesh = set.mesh(0);
    REQUIRE(mesh.triangleCount() == 1);
    REQUIRE(mesh.parts[0].texture == 3);
    REQUIRE(&set.mesh(0) == &mesh);
    REQUIRE_THROWS_AS(set.mesh(1), FileError);
}

TEST_CASE("a missing model manifest fails to load", "[assets][model]") {
    ModelSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("model-set-empty")));
    REQUIRE_FALSE(set.loaded());
}

TEST_CASE("the unpacked powerups hold the arrow meshes", "[assets][model][unpacked]") {
    const auto dir = test::unpackedOrSkip("POWERUPS/objects.json").parent_path();
    ModelSet set;
    REQUIRE(set.load(dir));
    const auto index = set.find("ICON_ARROWFR#0");
    REQUIRE(index.has_value());
    REQUIRE(set.mesh(*index).triangleCount() == 61);
}

} // namespace
