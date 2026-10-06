#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureBindings.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/TreeParticles.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {
using namespace gdl;

TEST_CASE("texture bindings skip unresolved references and keep destination placeholders",
          "[texture-lending][asset-conformance]") {
    TextureSet primary;
    TextureSet reference;
    TextureSet owner;
    REQUIRE(primary.load(test::sampleLevel("binding-primary")));
    REQUIRE(reference.load(test::sampleLevel("binding-reference")));
    REQUIRE(owner.load(test::sampleLender("binding-owner")));
    const std::array<TextureSet*, 4> lenders{nullptr, &primary, &reference, &owner};
    const TextureBindings bindings(primary, lenders);
    REQUIRE(bindings.slot(3));
    CHECK(bindings.slot(3)->set == &owner);
    CHECK(bindings.named("torchb")->set == &owner);
    CHECK(bindings.image("torchb")->set == &owner);
    CHECK(bindings.slot(0)->set == &primary);
    CHECK_FALSE(bindings.slot(400));
    CHECK_FALSE(bindings.named("MISSING"));
    CHECK_FALSE(TextureBindings(primary).slot(3));

    const auto blank = test::scratchDirectory("binding-placeholder");
    writeTextFile(blank / "textures.json", R"({"bitmaps":[
      {"name":"TORCHB","width":1,"height":1,"flags":288,"file":"unused"}]})");
    TextureSet placeholder;
    REQUIRE(placeholder.load(blank));
    const TextureBindings slots(placeholder, lenders);
    CHECK(slots.slot(0)->set == &placeholder);
    CHECK(slots.named("TORCHB")->set == &placeholder);
    CHECK(slots.image("TORCHB")->set == &owner);
    CHECK(placeholder.image(0).pixels == std::vector<u8>{0, 0, 0, 0});
}

TEST_CASE("tree particles borrow external images without losing their local animation slot",
          "[texture-lending][asset-conformance]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.models.load(test::sampleLevel("binding-tree")));
    REQUIRE(archive.textures.load(test::sampleLevel("binding-tree-textures")));
    const auto animations = test::scratchDirectory("binding-tree-animation");
    writeTextFile(animations / "animations.json",
                  R"({"trees":[{"name":"TEST","nodes":[]}],"particles":[
      {"texture":"TORCHB","enables":16384,"id":"A"}]})");
    REQUIRE(archive.trees.load(animations));
    TextureSet owner;
    TextureSet reference;
    REQUIRE(owner.load(test::sampleLender("binding-tree-owner")));
    REQUIRE(reference.load(test::sampleLevel("binding-tree-reference")));
    const std::array<TextureSet*, 2> lenders{&reference, &owner};
    TreeInfo tree;
    tree.name = "PARTICLE";
    TreeNodeInfo node;
    node.particle = 0;
    tree.nodes.push_back(node);
    TreeParticles particles;
    particles.bind(tree, archive, device, Mat4{1}, {}, lenders);
    REQUIRE(particles.field().size() == 1);
    CHECK(particles.field().textureOf(0) == &owner.texture(device, 0));
    particles.setTextureFrame(3, device.whiteTexture());
    CHECK(particles.field().textureOf(0) == &device.whiteTexture());
}
} // namespace
