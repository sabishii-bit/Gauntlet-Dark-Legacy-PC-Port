#include <algorithm>
#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "game/combat/EffectFlags.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantCheckpoint.h"
#include "game/effects/CritterCueEmission.h"
#include "game/hud/BossMeter.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Native Plague gas uses its animation container without the extra TOP lift",
          "[legends][native-plague][native-plague-gas][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "slime/objects.wad"))
        SKIP("N64 assets missing");
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, test::assetOrSkip("CRITTER").parent_path(), 38,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    const auto source = formats::decodeLegendsN64Critter(readFile(root / "slime/moves.wad"));
    REQUIRE(source.types.size() == 1);
    REQUIRE(source.types.front().rootNode.empty());
    REQUIRE(source.sounds.size() == 32);
    const auto& rawGas = source.sounds[28];
    REQUIRE(rawGas.name == "ATCK09FX");
    REQUIRE(rawGas.flags == 1);
    REQUIRE(rawGas.offset == std::array<f32, 3>{0, -3, 0});
    REQUIRE(stock->data.sound(28));
    CHECK(stock->data.sound(28)->offset == Vec3{0, -3, 0});
    CHECK(stock->data.sound(28)->flags == effect_flags::kRootParent);
    // All of this single-body boss's flag1 effects share the same native
    // container. Do not repair only gas by inserting a guessed Y correction.
    for (usize i = 0; i < source.sounds.size(); ++i) {
        if ((source.sounds[i].flags & 1U) != 0) {
            REQUIRE(stock->data.sound(static_cast<s32>(i)));
            CHECK(stock->data.sound(static_cast<s32>(i))->flags ==
                  ((source.sounds[i].flags & ~1U) | effect_flags::kRootParent));
        }
    }
    const auto move = std::ranges::find(stock->data.moves(), "PGAS", &MoveDefinition::name);
    REQUIRE(move != stock->data.moves().end());
    CHECK(move->sound == 28);
    CHECK(move->soundFrame == 37);
    CHECK(move->sound2 == 29);
    CHECK(move->sound2Frame == 0);
    CHECK(move->frameStart == 38);
    CHECK(move->frameEnd == 60);
    const auto sequence = stock->tree->findSequence(move->anim);
    REQUIRE(sequence);
    const auto native = readFile(root / "slime/seq.wad");
    const auto section = [&](std::string_view name) -> usize {
        const usize directory = readU32BE(native, 0);
        for (usize i = 0; i < readU32BE(native, 4); ++i) {
            const usize at = directory + i * 12;
            if (std::equal(name.begin(), name.end(),
                           native.begin() + static_cast<std::ptrdiff_t>(at)))
                return readU32BE(native, at + 4);
        }
        FAIL("Missing native section " << name);
        return 0;
    };
    // Independent source-body oracle: TOP is a translated child of actor+7c,
    // not that container itself (e0881474 -> e002f278 -> e002dde8).
    const usize body = section("BODY");
    REQUIRE(readU16BE(native, body) == 0x2800);
    REQUIRE(readU16BE(native, body + 10) == 151);
    REQUIRE(stock->tree->nodes.front().name == "TOP");
    REQUIRE(stock->tree->nodes.front().parent == -1);
    CHECK(stock->tree->nodes.front().position == Vec3{0, 151.0f / 64.0f, 0});
    // ATCK09FX's first drawable is BODY173, with a position track starting
    // at exactly these signed 1/64-unit coordinates. Preserve its native art.
    const usize gasBody = body + 173 * 8;
    REQUIRE(readU16BE(native, gasBody) == 0x203e);
    REQUIRE(readU16BE(native, gasBody + 8) == 1041);
    REQUIRE(readU16BE(native, gasBody + 10) == 1131);
    REQUIRE(readU16BE(native, gasBody + 12) == 84);
    for (const f32 yaw : {0.0f, 1.2f}) {
        CAPTURE(yaw);
        const Vec3 origin{19, 7, -31};
        Combatant actor;
        REQUIRE(actor.spawn(*stock, 0, origin, yaw, nullptr, {}, 'K'));
        const std::array resources{CombatantCheckpoint::Resource{1, stock.get()}};
        CombatantCheckpoint checkpoint(actor, resources, {});
        const auto initial = checkpoint.checkpoint();
        REQUIRE(initial);
        auto state = *initial;
        auto& saved = state.parts.front().actor;
        saved.move = static_cast<s32>(move - stock->data.moves().begin());
        saved.pattern = -1;
        saved.forcedPattern = true;
        saved.moveDone = false;
        saved.finishedSeconds = 0;
        saved.moveTarget = -1;
        saved.shotFrame = 35;
        saved.soundsGiven = 2; // PGAS's separate frame0 sound has already played.
        saved.smoothValid = false;
        AnimationPlayer animation;
        animation.start(stock->tree->sequences[*sequence], *sequence, 0, 36);
        saved.player = animation.checkpoint();
        TreePose pose;
        pose.evaluate(*stock->tree, *sequence, 36);
        saved.pose = pose.checkpoint();
        REQUIRE(checkpoint.restore(state));
        actor.update(1, 0, {});
        CHECK(actor.takeCues().empty());
        actor.update(1, 1.0f / 30, {});
        const auto cues = actor.takeCues();
        REQUIRE(cues.size() == 1);
        const auto& cue = cues.front();
        REQUIRE(cue.tree == "ATCK09FX");
        CHECK(cue.sound.empty());
        CHECK(cue.follows);
        CHECK(cue.rootAttachment);
        CHECK_FALSE(cue.node);
        CHECK(cue.nodeOffset == Vec3{0, -3, 0});
        REQUIRE(cue.placement);
        const Mat4 container =
            portable::rotate(glm::translate(Mat4{1}, origin + Vec3{0, -1, 0}), yaw, Vec3{0, 1, 0});
        const Mat4 expected = glm::translate(container, Vec3{0, -3, 0});
        for (Mat4::length_type column = 0; column < 4; ++column)
            for (Vec4::length_type row = 0; row < 4; ++row)
                CHECK((*cue.placement)[column][row] == Catch::Approx(expected[column][row]));
        const auto top = actor.nodeTransform("TOP");
        REQUIRE(top);
        CHECK(Vec3{*top * Vec4{0, -3, 0, 1}}.y - cue.position.y == Catch::Approx(151.0f / 64));
        const auto emission = CritterCueEmission::resolve(cue, &stock->archive, stock->archive);
        EffectTrees effects;
        const auto id = effects.startSet(device, *emission.archive, emission.tree,
                                         emission.position, emission.setting);
        REQUIRE(id != 0);
        effects.placeAt(id, *cue.placement);
        REQUIRE(effects.count() == 1);
        const auto& shown = effects.effect(0);
        REQUIRE(shown.pose.matrices().size() >= 2);
        const Vec3 emitter{shown.transform() * shown.pose.matrices()[1] * Vec4{0, 0, 0, 1}};
        const Vec3 authored{expected * Vec4{-1041.0f / 64, 1131.0f / 64, 84.0f / 64, 1}};
        CHECK(emitter.x == Catch::Approx(authored.x).margin(0.001f));
        CHECK(emitter.y == Catch::Approx(authored.y).margin(0.001f));
        CHECK(emitter.z == Catch::Approx(authored.z).margin(0.001f));
        // The actual rendered effect uses the same corrected parent; advancing
        // it must not reintroduce TOP or replay the source sound/visual cue.
        effects.update(4.0f / 30);
        device.draws.clear();
        const auto camera = CameraFrame::at(origin + Vec3{0, 30, -40});
        effects.draw(device, Mat4{1}, {}, &camera);
        CHECK_FALSE(device.draws.empty());
        actor.update(1, 1.0f / 30, {});
        CHECK(actor.takeCues().empty());
    }
}

TEST_CASE("Native Plague tentacle honors single-frame visibility in once attacks",
          "[legends][native-plague][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "slime/objects.wad"))
        SKIP("N64 assets missing");
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, test::assetOrSkip("CRITTER").parent_path(), 38,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    REQUIRE(stock->tree->nodes.size() == 28);
    const auto& node = stock->tree->nodes[25];
    REQUIRE(node.name == "OANIM#2");
    // Isolate the reported mesh, but still bind/sample/draw through the actual
    // common TreeModel rather than inspecting only decoder metadata.
    for (usize i = 0; i < stock->tree->nodes.size(); ++i)
        stock->body.setMeshAlpha(i, i == 25 ? 1.0f : 0.0f);
    const auto draw = [&](u32 sequence, s32 frame) {
        TreePose pose;
        pose.evaluate(*stock->tree, sequence, static_cast<f32>(frame));
        stock->body.setFrame(sequence, frame);
        device.draws.clear();
        stock->body.draw(device, Mat4{1}, Mat4{1}, {}, pose.matrices());
    };
    for (const auto name : {"ATK3", "ATK9", "AT10"}) {
        CAPTURE(name);
        const auto sequence = stock->tree->findSequence(name);
        REQUIRE(sequence);
        REQUIRE(*sequence < node.objectFrames.size());
        CHECK(node.objectFrames[*sequence].frames == 2);
        CHECK_FALSE(node.objectFrames[*sequence].interpolate);
        draw(*sequence, 0);
        CHECK_FALSE(device.draws.empty());
        for (const auto frame : {1, stock->tree->sequences[*sequence].frames - 1}) {
            draw(*sequence, frame);
            CHECK(device.draws.empty());
        }
    }
    const auto ready = stock->tree->findSequence("REDY");
    REQUIRE(ready);
    REQUIRE(*ready < node.objectFrames.size());
    CHECK(node.objectFrames[*ready].frames == 1);
    draw(*ready, 59);
    CHECK_FALSE(device.draws.empty()); // Native looping hold remains separate.
}

TEST_CASE("Native Plague meter uses its own artwork and drains without losing the frame",
          "[legends][native-plague][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "slime/objects.wad"))
        SKIP("N64 assets missing");
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock = loadLegendsBossAssets(device, root, test::assetOrSkip("CRITTER").parent_path(), 38,
                                       readFile(root / "_program/game.bin"), {}, warnings);
    REQUIRE(stock);
    auto& textures = stock->archive.textures;
    BossMeter meter;
    REQUIRE(meter.bind(stock->data.meter(), &textures));
    Canvas canvas;
    const auto draw = [&] {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        meter.draw(canvas, device);
        canvas.end();
    };
    meter.update(0, 1500, 1500, true, false);
    draw();
    REQUIRE(device.draws.size() == 4);
    const std::array names{"METER_BG1", "METER_FG1", "METER_BG2", "METER_FG2"};
    const auto native = readFile(root / "slime/objects.wad");
    const auto section = [&](std::string_view tag) -> usize {
        const usize table = readU32BE(native, 0);
        const usize count = readU32BE(native, 4);
        for (usize row = 0; row < count; ++row) {
            const usize at = table + row * 12;
            if (std::equal(tag.begin(), tag.end(),
                           native.begin() + static_cast<std::ptrdiff_t>(at)))
                return readU32BE(native, at + 4);
        }
        FAIL("Missing native section " << tag);
        return 0;
    };
    for (usize i = 0; i < names.size(); ++i) {
        const auto index = textures.find(names[i]);
        REQUIRE(index);
        CHECK(device.draws[i].texture == &textures.texture(device, *index));
        const auto* pixels = textures.cachedImage(*index);
        REQUIRE(pixels);
        CHECK(pixels->width == 200);
        CHECK(pixels->height == 32);
        if (i % 2 == 0) {
            // Independent native-source oracle: BG slots5/6,13 TMEM words
            // per scanline. The previous packed100-byte decoder slipped four
            // bytes each row and scrambled the frame despite finding all4 images.
            const usize header = section("TXHD") + (5 + i / 2) * 24;
            REQUIRE(native[header + 18] == 2);
            REQUIRE(native[header + 19] == 0);
            REQUIRE(readU16BE(native, header + 16) * 8 == 3328);
            const usize texels = section("TEXR") + readU32BE(native, header);
            const usize palette = texels + 3328;
            const auto expand = [](u32 value) {
                return static_cast<u8>((value << 3U) | (value >> 2U));
            };
            for (u32 y = 0; y < 32; ++y)
                for (u32 x = 0; x < 200; ++x) {
                    const u8 packed = native[texels + y * 104 + x / 2];
                    const u8 slot = (packed >> ((x & 1U) ? 0 : 4)) & 15U;
                    const u16 value = readU16BE(native, palette + usize{slot} * 2);
                    CHECK(pixels->pixel(x, y) ==
                          Color::rgba(expand((value >> 11) & 31), expand((value >> 6) & 31),
                                      expand((value >> 1) & 31), (value & 1) ? 255 : 0));
                }
        }
    }
    const f32 right = test::maxCorner(device.draws[3]).x;
    meter.update(500, 1000, 1500, true, false);
    draw();
    REQUIRE(device.draws.size() == 4);
    CHECK(test::maxCorner(device.draws[3]).x < right);
    meter.update(500, 0, 1500, true, false);
    draw();
    CHECK(device.draws.size() == 3); // First cap and both background strips remain.
    meter.update(0, 0, 1500, false, false);
    draw();
    CHECK(device.draws.empty());
}
