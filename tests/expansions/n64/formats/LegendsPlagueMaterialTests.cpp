#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/formats/LegendsN64Texture.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
void text(std::vector<u8>& bytes, usize at, std::string_view value) {
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

TEST_CASE("N64 once-only vertex visibility survives a simultaneous keyed texture binding",
          "[formats][legends][native-plague-material][object-frames]") {
    // One BODY drawable and its native vertex permutation, then simultaneous
    // property16 (one pose) and property7 (four texture slots) on that drawable.
    std::vector<u8> bytes(188);
    put32(bytes, 0, 128);
    put32(bytes, 4, 5);
    const std::array<std::string_view, 5> tags{"BODY", "BDAT", "SEQS", "SLST", "GRPS"};
    const std::array<u32, 5> offsets{8, 24, 32, 96, 104};
    const std::array<u32, 5> counts{16, 4, 64, 1, 1};
    for (usize i = 0; i < tags.size(); ++i) {
        text(bytes, 128 + i * 12, tags[i]);
        put32(bytes, 132 + i * 12, offsets[i]);
        put32(bytes, 136 + i * 12, counts[i]);
    }
    put16(bytes, 10, 0xffff);
    put16(bytes, 12, 1);
    put16(bytes, 16, 0xe00);
    put32(bytes, 18, 24);
    put16(bytes, 24, 1); // One original vertex; all three cache uploads refer to it.
    bytes[32] = 2;
    bytes[33] = 16;
    bytes[34] = 1;
    put16(bytes, 38, 4);
    bytes[40] = 16;
    put16(bytes, 42, 8); // Vertex payload at64.
    bytes[44] = 7;
    put16(bytes, 46, 4); // Texture payload at48.
    put16(bytes, 50, 2);
    bytes[53] = 4;
    bytes[55] = 16;
    bytes[64] = 4; // Keep before, but do not hold after.
    put16(bytes, 66, 1);
    put16(bytes, 70, 1);
    bytes[80] = 64;
    bytes[81] = 128;
    bytes[82] = 192;
    text(bytes, 96, "TEST");
    put32(bytes, 100, 32);
    text(bytes, 104, "EFFECT");
    put32(bytes, 120, 1);
    put32(bytes, 124, 96);
    LegendsN64Model model;
    model.name = "SOURCE";
    model.mesh.vertices.resize(3);
    model.mesh.parts = {{LegendsN64Model::kRuntimeTexture, 0, {0, 1, 2}}};
    model.vertexSources = {0, 0, 0};
    bool hiddenTail = true;
    SECTION("once hides after the only authored pose") {}
    SECTION("hold-after keeps the last pose") {
        bytes[64] |= 8;
        hiddenTail = false;
    }
    SECTION("looping sequence keeps its single pose") {
        bytes[34] = 0;
        hiddenTail = false;
    }
    const std::array models{model};
    for (const bool bake : {false, true}) {
        CAPTURE(bake, hiddenTail);
        const auto decoded = decodeLegendsN64Sequences(bytes, models, {}, {}, bake);
        REQUIRE(decoded.warnings.empty());
        REQUIRE(decoded.animation.trees.size() == 1);
        const auto& node = decoded.animation.trees[0].nodes[0];
        REQUIRE(node.objectFrames.size() == 1);
        const auto& run = node.objectFrames[0];
        const auto first =
            std::ranges::find(decoded.modelVariants, run.object, &LegendsN64Model::name);
        REQUIRE(first != decoded.modelVariants.end());
        REQUIRE(run.frames == (bake ? 4 : hiddenTail ? 2 : 1));
        REQUIRE(decoded.modelVariants.end() - first >= run.frames);
        REQUIRE(first->mesh.vertices.size() == 3);
        CHECK(first->mesh.vertices[0].position == Vec3{1, 2, 3});
        CHECK(first->mesh.parts[0].texture == 2);
        for (s32 frame = 1; frame < run.frames; ++frame) {
            const auto& shape = first[frame].mesh;
            CHECK(shape.vertices.empty() == hiddenTail);
            CHECK(shape.parts.empty() == hiddenTail);
            if (!hiddenTail) {
                CHECK(shape.vertices[0].position == Vec3{1, 2, 3});
                CHECK(shape.parts[0].texture == 2U + static_cast<u32>(frame));
            }
        }
        if (hiddenTail) {
            CHECK_FALSE(run.interpolate);
        }
    }
}

TEST_CASE("N64 shaded color ramps retain normals and RGB without multiplying shade alpha",
          "[formats][legends][color-ramp][native-plague-material]") {
    LegendsN64Texture source;
    source.name = "GRAY";
    source.image = {3, 1, {0, 0, 0, 7, 128, 128, 128, 99, 255, 255, 255, 255}};
    LegendsN64Model model;
    model.name = "SHADED";
    model.mesh.vertices.resize(3);
    model.mesh.parts = {{0, 0, {0, 1, 2}}};
    model.mesh.vertices[0].position = Vec3{1, 2, 3};
    model.mesh.vertices[0].normal = Vec3{-64.0f / 127, 96.0f / 127, 12.0f / 127};
    model.mesh.vertices[0].color = Color::rgba(43, 137, 59, 11);
    model.mesh.vertices[0].uv = Vec2{3, 4};
    model.vertexSources = {19, 20, 21};
    model.colorRamps = {{0, Color::rgba(141, 246, 40), Color::rgba(17, 86, 5), true}};
    SECTION("normals remain lighting inputs") {
        model.mesh.prelit = false;
    }
    SECTION("prelit shade keeps authored RGB") {
        model.mesh.prelit = true;
    }

    std::vector<LegendsN64Model> models{model};
    std::vector<LegendsN64Texture> textures{source};
    AnimationFile animation;
    std::vector<std::string> warnings;
    resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
    CHECK(warnings.empty());
    REQUIRE(textures.size() == 2);
    CHECK(textures[0].image.pixels == source.image.pixels);
    CHECK(textures[1].image.pixel(0, 0) == Color::rgba(141, 246, 40, 7));
    CHECK(textures[1].image.pixel(1, 0) == Color::rgba(79, 166, 22, 99));
    CHECK(textures[1].image.pixel(2, 0) == Color::rgba(17, 86, 5, 255));
    CHECK(models[0].mesh.prelit == model.mesh.prelit);
    REQUIRE(models[0].mesh.parts[0].indices.size() == 3);
    for (usize i = 0; i < 3; ++i) {
        const auto copy = models[0].mesh.parts[0].indices[i];
        auto expected = model.mesh.vertices[i];
        expected.color.a = 255;
        CHECK(models[0].mesh.vertices[copy] == expected);
        CHECK(models[0].vertexSources[copy] == model.vertexSources[i]);
        CHECK(models[0].mesh.vertices[i] == model.mesh.vertices[i]);
    }
    CHECK(models[0].colorRamps.empty());
    const auto resolvedVertices = models[0].mesh.vertices;
    resolveLegendsN64ColorRamps(models, textures, animation);
    CHECK(models[0].mesh.vertices == resolvedVertices);
    CHECK(textures.size() == 2);
}

TEST_CASE("N64 mixed lit and unlit ramp parts fail closed without altering original artwork",
          "[formats][legends][color-ramp][native-plague-material]") {
    LegendsN64Model model;
    model.name = "MIXED";
    model.mesh.vertices.resize(3);
    model.mesh.parts = {{0, 0, {0, 1, 2}}, {0, 0, {0, 1, 2}}};
    model.colorRamps = {{0, Color::black(), Color::white(), true},
                        {1, Color::black(), Color::white(), false}};
    LegendsN64Texture source;
    source.image = Image::filled(1, 1, Color::rgba(10, 20, 30, 40));
    std::vector<LegendsN64Model> models{model};
    std::vector<LegendsN64Texture> textures{source};
    AnimationFile animation;
    std::vector<std::string> warnings;
    resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(models[0].colorRamps == model.colorRamps);
    CHECK(models[0].mesh.vertices == model.mesh.vertices);
    CHECK_FALSE(models[0].mesh.prelit);
    CHECK(models[0].mesh.parts[0].texture == 0);
    CHECK(models[0].mesh.parts[1].texture == 0);
    CHECK(textures.size() == 1);
    CHECK(textures[0].image.pixels == source.image.pixels);
}

TEST_CASE("Native Plague breath and splash use green ramps while shaded effects retain normals",
          "[formats][legends][color-ramp][native-plague-material][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "slime/objects.wad")) {
        SKIP("Native N64 Plague artwork unavailable");
    }
    const auto objects = readFile(root / "slime/objects.wad");
    auto models = decodeLegendsN64Models(objects);
    REQUIRE(models.size() > 39);
    REQUIRE(models[16].colorRamps.size() == 1);
    CHECK(models[16].colorRamps[0].shadeRgb);
    CHECK_FALSE(models[16].mesh.prelit);
    CHECK(models[16].name == "PBOSS_ATCK01_HITXLNODE");
    REQUIRE(models[22].colorRamps.size() == 1);
    REQUIRE(models[39].colorRamps.size() == 1);
    const auto breath = models[22].colorRamps[0];
    const auto splash = models[39].colorRamps[0];
    CHECK(breath.primitive == Color::rgba(141, 216, 102));
    CHECK(breath.environment == Color::rgba(4, 176, 37));
    CHECK_FALSE(breath.shadeRgb);
    CHECK(splash.primitive == Color::rgba(150, 232, 84));
    CHECK(splash.environment == Color::rgba(36, 148, 13));
    CHECK_FALSE(splash.shadeRgb);
    auto decoded = decodeLegendsN64Sequences(readFile(root / "slime/seq.wad"), models,
                                             readFile(root / "_program/game.bin"), {}, true);
    models.insert(models.end(), decoded.modelVariants.begin(), decoded.modelVariants.end());
    const auto before = models;
    auto textures = decodeLegendsN64Textures(objects, readFile(root / "slime/texdyn.rom"));
    const auto originalTextures = textures;
    resolveLegendsN64ColorRamps(models, textures, decoded.animation);
    REQUIRE(textures.size() > originalTextures.size());
    for (usize i = 0; i < originalTextures.size(); ++i) {
        CHECK(textures[i].image.pixels == originalTextures[i].image.pixels);
    }
    usize shaded = 0;
    usize breathParts = 0;
    usize splashParts = 0;
    for (usize i = 0; i < models.size(); ++i) {
        const auto& old = before[i];
        const auto& resolved = models[i];
        for (const auto& ramp : old.colorRamps) {
            const auto& part = resolved.mesh.parts.at(ramp.part);
            const auto& oldPart = old.mesh.parts.at(ramp.part);
            if (oldPart.texture == LegendsN64Model::kRuntimeTexture) {
                continue; // Original placeholders are not drawn; node variants supply slots.
            }
            CAPTURE(old.name, ramp.part);
            const auto& source = originalTextures.at(oldPart.texture);
            if (source.external || source.image.pixels.empty()) {
                // This reader-level test intentionally has no external archive
                // lender. Missing pixels must remain unresolved, not fabricated.
                CHECK(part.texture == oldPart.texture);
                CHECK(resolved.colorRamps == old.colorRamps);
                continue;
            }
            REQUIRE(part.texture >= originalTextures.size());
            REQUIRE(part.texture < textures.size());
            const auto& image = textures[part.texture].image;
            const auto& original = originalTextures.at(oldPart.texture).image;
            REQUIRE(image.pixels.size() == original.pixels.size());
            REQUIRE_FALSE(image.pixels.empty());
            const bool isBreath =
                ramp.primitive == breath.primitive && ramp.environment == breath.environment;
            const bool isSplash =
                ramp.primitive == splash.primitive && ramp.environment == splash.environment;
            const std::array low{ramp.primitive.r, ramp.primitive.g, ramp.primitive.b};
            const std::array high{ramp.environment.r, ramp.environment.g, ramp.environment.b};
            for (usize pixel = 0; pixel < image.pixels.size(); pixel += 4) {
                for (usize channel = 0; channel < 3; ++channel) {
                    const int intensity = original.pixels[pixel + channel];
                    CHECK(image.pixels[pixel + channel] == (int{low[channel]} * (255 - intensity) +
                                                            int{high[channel]} * intensity + 127) /
                                                               255);
                }
                if (isBreath || isSplash) {
                    CHECK(image.pixels[pixel + 1] > image.pixels[pixel]);
                    CHECK(image.pixels[pixel + 1] > image.pixels[pixel + 2]);
                }
                CHECK(image.pixels[pixel + 3] == original.pixels[pixel + 3]);
            }
            REQUIRE(part.indices.size() == oldPart.indices.size());
            for (usize vertex = 0; vertex < part.indices.size(); ++vertex) {
                auto expected = old.mesh.vertices.at(oldPart.indices[vertex]);
                expected.color = ramp.shadeRgb ? expected.color.withAlpha(255) : Color::white();
                CHECK(resolved.mesh.vertices.at(part.indices[vertex]) == expected);
            }
            if (ramp.shadeRgb) {
                ++shaded;
                CHECK(resolved.mesh.prelit == old.mesh.prelit);
            }
            if (isBreath) {
                ++breathParts;
            }
            if (isSplash) {
                ++splashParts;
            }
        }
    }
    CHECK(shaded > 0);
    CHECK(breathParts > 0);
    CHECK(splashParts > 0);
}
} // namespace
