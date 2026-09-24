#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/TreeParticles.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {
using namespace gdl;

TEST_CASE("tree particle nodes follow posed attachments without requiring model assets",
          "[engine][world][tree-particles]") {
    const auto root = test::scratchDirectory("tree-particles");
    writeTextFile(root / "animations.json", R"({
        "particles":[{"preset":1,"enables":813937,"emitterLife":[0.9,0.43],
            "particleLife":[0.2,0.1],"rate":[70,40,40,0],"speed":72,"angle":0}],
        "trees":[{"name":"TEST","nodes":[
            {"name":"ROOT","parent":-1,"position":[0,0,0]},
            {"name":"EMITTER","parent":0,"position":[0,2,0],"particle":0,"direction":[1,0,0]},
            {"name":"BAD_INDEX","parent":0,"position":[0,0,0],"particle":8}]}]})");
    ItemArchive archive;
    REQUIRE(archive.trees.load(root));
    test::FakeRenderDevice device;
    TreeParticles particles;
    const std::array pose{Mat4{1}, glm::translate(Mat4{1}, Vec3{0, 2, 0}), Mat4{1}};
    particles.bind(archive.trees.tree(0), archive, device, Mat4{1}, pose);
    REQUIRE(particles.field().size() == 1);
    REQUIRE(particles.field().emitter(0).descriptor().direction == Vec3{1, 0, 0});
    const Mat4 parent = glm::rotate(glm::translate(Mat4{1}, Vec3{3, 4, 5}), 0.6f, Vec3{1, 0, 0});
    particles.step(1.0f / 30, parent, pose);
    REQUIRE(particles.field().particleCount() > 0);
    REQUIRE(particles.field().emitter(0).node() == parent * pose[1]);
    particles.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0});
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(device.draws[0].texture == &device.whiteTexture());
    const auto count = particles.field().particleCount();
    const test::FakeTexture unrelated{1, 1};
    particles.setTextureFrame(42, unrelated); // a missing texture is not slot zero
    REQUIRE(particles.field().textureOf(0) == &device.whiteTexture());
    REQUIRE(particles.field().particleCount() == count);
    particles.stop();
    REQUIRE(particles.field().particleCount() == count); // finish their lives, do not pop away
    for (s32 i = 0; i < 300; ++i) {
        particles.step(1.0f / 30, parent, pose);
    }
    REQUIRE(particles.field().particleCount() == 0);
    REQUIRE_FALSE(particles.field().active(0));
    particles.bind(TreeInfo{}, archive, device, Mat4{1}, {});
    REQUIRE(particles.field().size() == 0);
}
} // namespace
