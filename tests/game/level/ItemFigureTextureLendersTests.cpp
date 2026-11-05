#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"

#include "FakeRenderDevice.h"
#include "formats/AnimationTree.h"
#include "game/level/ItemFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

Mesh triangle() {
    Mesh mesh;
    mesh.vertices = {{.position = {0, 0, 0}}, {.position = {1, 0, 0}}, {.position = {0, 1, 0}}};
    mesh.parts.push_back({.indices = {0, 1, 2}});
    return mesh;
}

ItemArchive fixture(bool tree) {
    ItemArchive archive;
    archive.models = ModelSet::fromDecoded({{.name = "PIECE"}}, {triangle()});
    archive.textures =
        TextureSet::fromDecoded({{.name = "SKIN", .flags = TextureSetEntry::kExternal}}, {Image{}});
    if (tree) {
        formats::AnimationFile data;
        formats::TreeDefinition figure;
        figure.name = "PIECE";
        figure.nodes.push_back({.name = "ROOT", .object = "PIECE"});
        data.trees.push_back(std::move(figure));
        archive.trees = AnimationSet::fromDecoded(std::move(data));
    }
    return archive;
}

TextureSet skin(Color color) {
    return TextureSet::fromDecoded({{.name = "SKIN", .width = 2, .height = 2}},
                                   {Image::filled(2, 2, color)});
}

TEST_CASE("Item figures borrow native textures only when explicitly supplied",
          "[item-figure][texture-lenders][legends]") {
    bool tree = true;
    SECTION("animated tree") {}
    SECTION("static fallback") {
        tree = false;
    }
    auto archive = fixture(tree);
    auto red = skin(Color{255, 0, 0, 255});
    auto blue = skin(Color{0, 0, 255, 255});
    test::FakeRenderDevice device;
    const std::array<TextureSet*, 1> redLenders{&red};
    const std::array<TextureSet*, 1> blueLenders{&blue};
    ItemFigure figure;
    const AssetScope scope{archive};
    REQUIRE_FALSE(figure.placeStaticFallback(device, scope, "PIECE", {}, nullptr, 0));
    REQUIRE(figure.placeStaticFallback(device, scope, "PIECE", {}, nullptr, 0, redLenders));
    figure.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &red.texture(device, 0));
    const ItemFigure saved = figure;

    // Rebinding the same tree to another lender must not reuse the earlier GPU binding.
    REQUIRE(figure.placeStaticFallback(device, scope, "PIECE", {}, nullptr, 0, blueLenders));
    device.draws.clear();
    figure.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &blue.texture(device, 0));
    device.draws.clear();
    saved.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &red.texture(device, 0));

    // The retained cache still distinguishes the original lender and local-only mode.
    REQUIRE(figure.placeStaticFallback(device, scope, "PIECE", {}, nullptr, 0, redLenders));
    device.draws.clear();
    figure.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &red.texture(device, 0));
    CHECK_FALSE(figure.placeStaticFallback(device, scope, "PIECE", {}, nullptr, 0));
}

TEST_CASE("Item figure texture animation reads its frames from the explicit lender",
          "[item-figure][texture-lenders][legends]") {
    auto archive = fixture(true);
    archive.textures = TextureSet::fromDecoded({{.name = "SKIN", .noPicture = true}}, {Image{}});
    formats::AnimationFile data;
    formats::TreeDefinition tree;
    tree.name = "PIECE";
    tree.nodes.push_back({.name = "ROOT", .object = "PIECE"});
    data.trees.push_back(std::move(tree));
    formats::TextureAnimation animation;
    animation.name = "CYCLE";
    animation.frameName = "FRAME0";
    animation.flag = -1;
    animation.texture = 0;
    animation.source = formats::TextureAnimation::kByName;
    animation.frames = 2;
    animation.rate = 1;
    data.textureAnimations.push_back(animation);
    archive.trees = AnimationSet::fromDecoded(std::move(data));
    auto frames = TextureSet::fromDecoded(
        {{.name = "FRAME0", .width = 2, .height = 2}, {.name = "FRAME1", .width = 2, .height = 2}},
        {Image::filled(2, 2, Color{255, 0, 0, 255}), Image::filled(2, 2, Color{0, 0, 255, 255})});
    const std::array<TextureSet*, 1> lenders{&frames};
    test::FakeRenderDevice device;
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "PIECE", {}, nullptr, lenders));
    figure.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &frames.texture(device, 0));
    figure.update(0.04f);
    device.draws.clear();
    figure.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.back().texture == &frames.texture(device, 1));
}

TEST_CASE("Item figure particle nodes use the same explicit texture scope",
          "[item-figure][texture-lenders][legends]") {
    auto archive = fixture(false);
    formats::AnimationFile data;
    formats::TreeDefinition tree;
    tree.name = "GLOW";
    tree.nodes.push_back(
        {.name = "EMITTER", .type = formats::TreeNodeType::Particles, .flags = 1, .particle = 0});
    data.trees.push_back(std::move(tree));
    formats::ParticleTemplateRecord particle;
    particle.enables =
        ParticleTemplate::kTexture | ParticleTemplate::kParticleLife | ParticleTemplate::kRate;
    particle.texture = "SKIN";
    particle.particleLife = {1, 0};
    particle.rate = {30, 30, 30, 30};
    data.particles.push_back(std::move(particle));
    archive.trees = AnimationSet::fromDecoded(std::move(data));
    auto lender = skin(Color::white());
    const std::array<TextureSet*, 1> lenders{&lender};
    test::FakeRenderDevice device;
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "GLOW", {}, nullptr, lenders));
    REQUIRE(figure.particles().field().size() == 1);
    CHECK(figure.particles().field().textureOf(0) == &lender.texture(device, 0));
}

TEST_CASE("Item figure appearance overrides do not leak across shared copies",
          "[item-figure][legends][presentation]") {
    auto archive = fixture(false);
    formats::AnimationFile data;
    formats::TreeDefinition tree;
    tree.name = "PIECE";
    tree.nodes.push_back({.name = "ROOT", .object = "PIECE"});
    tree.nodes.push_back({.name = "CHILD", .object = "PIECE", .parent = 0});
    data.trees.push_back(std::move(tree));
    archive.trees = AnimationSet::fromDecoded(std::move(data));
    auto lender = skin(Color::white());
    const std::array<TextureSet*, 1> lenders{&lender};
    test::FakeRenderDevice device;
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "PIECE", {}, nullptr, lenders));
    const ItemFigure copy = figure;
    const ItemFigure before = figure;
    copy.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    const Color litColor = device.draws.front().vertices.front().color;
    device.draws.clear();
    const std::array<usize, 1> hidden{0};
    const ItemFigure::Appearance appearance{Color::rgba(8, 8, 8), true, hidden, 0, true};
    figure.draw(device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::All, -1, &appearance);
    // Hiding the parent mesh must retain its child's own visible geometry.
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 3);
    CHECK(device.draws.front().vertices.front().color == Color::rgba(8, 8, 8));
    CHECK_FALSE(device.draws.front().state.cullBack);
    CHECK(figure == before);
    device.draws.clear();
    copy.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws.front().vertices.front().color == litColor);
    CHECK(device.draws.front().state.cullBack);
    CHECK(copy == before);
}
} // namespace
