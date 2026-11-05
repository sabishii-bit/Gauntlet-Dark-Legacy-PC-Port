#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/critters/CritterStatues.h"
#include "game/level/ItemFigure.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path checkpointArchive() {
    const auto path = test::scratchDirectory("statue-checkpoint");
    std::filesystem::create_directories(path / "models");
    writeTextFile(path / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(path / "objects.json", R"({"objects":[
      {"index":0,"name":"STONE","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(path / "skin.png", test::kTinyPng);
    writeTextFile(path / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(path);
    writeTextFile(path / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"GOL_STATUE","nodes":[
        {"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]},
        {"name":"SMOKE","particle":0,"parent":0,"position":[0,2,0]}],
        "sequences":[{"name":"IDLE","frames":0,"frameRate":30},
                     {"name":"ACTIVE","frames":10,"frameRate":30},
                     {"name":"EMPTY","frames":0,"frameRate":30}]}]})");
    return path;
}

CritterStatues::Placement statueAt(f32 x, s32 activeOn = 0) {
    CritterStatues::Placement result;
    result.kind = CombatantKind::Golem;
    result.form = "STONE";
    result.instance.info = 3;
    result.instance.minPlayers = 2;
    result.instance.flags = 7;
    result.instance.name = "PLACED";
    result.instance.position = {x, 2, 4};
    result.instance.rotation = {0.1f, 0.2f, 0.3f};
    result.instance.params = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    result.instance.collision.push_back({{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}}});
    result.radius = 3;
    result.height = 5;
    result.viewRadius = 10;
    result.sight = 12;
    result.activeOn = activeOn;
    result.carried = 8;
    return result;
}

struct StatueFixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterStatues native;
    CritterStatues replay;
    StatueFixture() {
        REQUIRE(archive.load(checkpointArchive()));
        for (auto* pool : {&native, &replay}) {
            pool->setContactGridBounds({-320, 0, -320}, {320, 0, 320});
            REQUIRE(pool->add(device, archive, statueAt(0, 3), nullptr));
            REQUIRE(pool->add(device, archive, statueAt(30), nullptr));
            REQUIRE(pool->add(device, archive, statueAt(60), nullptr));
        }
    }
};

/** The statues' native digest over their one archive, named alike for every pool. */
NativeStateDigest::Digest digestOf(const CritterStatues& statues, const ItemArchive& archive) {
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(archive, "stage/statues"));
    return test::liveDigest(statues, resources);
}

TEST_CASE("statue rewind restores compacted identities queued births and exact continuation",
          "[critter-statues-local-rewind][rollback]") {
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    StatueFixture fixture;
    auto& statues = fixture.native;
    CritterStatues::LocalSnapshot initial;
    statues.saveLocal(initial);
    const u64 firstId = statues.instanceOf(0);
    const u64 secondId = statues.instanceOf(1);
    statues.wake(0);
    statues.wake(1);
    statues.update(ticks, seconds, {}); // starts ACTIVE without consuming its clock
    for (s32 tick = 0; tick < 6; tick += ticks) {
        statues.update(ticks, seconds, {});
    }
    REQUIRE(statues.count() == 2);
    REQUIRE(statues.instanceOf(0) == secondId);
    const auto particles = statues.figure(0).particles().checkpoint();
    REQUIRE(particles.field.entries.size() == 1);
    REQUIRE_FALSE(particles.field.entries[0].emitter.particles.empty());
    CritterStatues::LocalSnapshot state;
    statues.saveLocal(state);
    const auto risen = statues.takeRisen();
    REQUIRE(risen.size() == 1);
    REQUIRE(risen.front() == statueAt(0, 3));
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto view = [](s32 tick) {
        return [seen = tick % 6 != 0](const Vec3&, f32) { return seen; };
    };
    std::vector<CritterStatues::LocalSnapshot> expected;
    for (s32 tick = 0; tick < 30; tick += ticks) {
        statues.update(ticks, seconds, view(tick));
        statues.saveLocal(expected.emplace_back());
    }
    REQUIRE(statues.restoreLocal(state));
    CritterStatues::LocalSnapshot current;
    statues.saveLocal(current);
    CHECK(statues.equalLocal(current, state));
    CHECK(statues.takeRisen() == risen);
    usize index = 0;
    for (s32 tick = 0; tick < 30; tick += ticks) {
        statues.update(ticks, seconds, view(tick));
        statues.saveLocal(current);
        CHECK(statues.equalLocal(current, expected[index++]));
    }
    REQUIRE(statues.restoreLocal(initial));
    REQUIRE(statues.count() == 3);
    CHECK(statues.instanceOf(0) == firstId);
    statues.saveLocal(current);
    CHECK(statues.equalLocal(current, initial));
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("item figure copies retain held pose presentation particles and fractional clocks",
          "[item-figure-local-rewind][rollback]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(checkpointArchive()));
    ItemFigure figure;
    REQUIRE(figure.place(device, archive, "GOL_STATUE", statueAt(0).instance, nullptr));
    figure.play(1, false);
    figure.gateParticlesOnSequence(true);
    figure.update(0.073f);
    figure.capturePresentation();
    figure.tilt(0.4f, -0.2f);
    figure.update(0.017f);
    figure.draw(device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::All, 0.4f);
    REQUIRE(figure.textureAnimator().clock().remainder > 0);
    REQUIRE_FALSE(figure.particles().checkpoint().field.entries[0].emitter.particles.empty());
    const auto uploads = device.texturesCreated;
    auto clone = std::make_unique<ItemFigure>(figure);
    CHECK(*clone == figure);
    for (s32 tick = 0; tick < 8; ++tick) {
        figure.capturePresentation();
        clone->capturePresentation();
        figure.update(0.019f);
        clone->update(0.019f);
        CHECK(*clone == figure);
    }
    figure.play(2, false);
    *clone = figure;
    CHECK(*clone == figure);
    figure.update(0.02f);
    clone->update(0.02f);
    CHECK(*clone == figure);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native gargoyle statue wakes and retires identically after fractional rewind",
          "[critter-statues-local-rewind][rollback][assets]") {
    const auto root = test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    auto placement = statueAt(0);
    placement.kind = CombatantKind::Gargoyle;
    placement.form = "GAR_EAGL";
    CritterStatues native;
    REQUIRE(native.add(device, archive, placement, nullptr));
    native.wake(0);
    native.update(1, 1.0f / 60, {});
    for (s32 tick = 0; tick < 17; ++tick) {
        native.update(1, 1.0f / 60, {});
    }
    CritterStatues::LocalSnapshot state;
    native.saveLocal(state);
    std::vector<CritterStatues::LocalSnapshot> expected;
    for (s32 tick = 17; tick < 171; ++tick) {
        native.update(1, 1.0f / 60, {});
        native.saveLocal(expected.emplace_back());
    }
    REQUIRE(native.count() == 0);
    const auto risen = native.takeRisen();
    REQUIRE(native.restoreLocal(state));
    CritterStatues::LocalSnapshot current;
    for (const auto& step : expected) {
        native.update(1, 1.0f / 60, {});
        native.saveLocal(current);
        CHECK(native.equalLocal(current, step));
    }
    REQUIRE(native.count() == 0);
    CHECK(native.takeRisen() == risen);
}

TEST_CASE("native statue rewind restores wrong wake branches across compaction and pending births",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot saved;
    std::array<CritterStatues::LocalSnapshot, 8> uninterrupted;
    CritterStatues::LocalSnapshot actual;
    CritterStatues::LocalSnapshot before;
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    const auto advance = [](CritterStatues& statues, s32 tick) {
        if (tick == 0 || tick == 10 || tick == 24) {
            statues.wake(statues.count() > 1 ? 1 : 0);
        }
        statues.update(1, 1.0f / 60, [tick](const Vec3&, f32) { return tick % 4 != 0; });
        if (tick % 13 == 12) {
            statues.takeRisen();
        }
    };
    bool sawRisen = false;
    bool sawCompaction = false;
    s32 tick = 0;
    for (s32 window = 0; window < 24; ++window) {
        CAPTURE(window, tick);
        fixture.replay.saveLocal(saved);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(fixture.replay, tick + step);
            fixture.replay.saveLocal(uninterrupted[static_cast<usize>(step)]);
        }
        REQUIRE(fixture.replay.restoreLocal(saved));
        for (usize i = 0; i < fixture.replay.count(); ++i) {
            fixture.replay.wake(i);
        }
        fixture.replay.setContactGridBounds({-900, 0, -800}, {700, 0, 600});
        for (s32 step = 0; step < distance; ++step) {
            fixture.replay.update(2, 1.0f / 30, {});
        }
        fixture.replay.takeRisen();
        REQUIRE(fixture.replay.restoreLocal(saved));
        fixture.replay.saveLocal(actual);
        CHECK(fixture.replay.equalLocal(actual, saved));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(fixture.native, tick);
            advance(fixture.replay, tick);
            fixture.replay.saveLocal(actual);
            CHECK(fixture.replay.equalLocal(uninterrupted[static_cast<usize>(step)], actual));
            CHECK(digestOf(fixture.replay, fixture.archive) ==
                  digestOf(fixture.native, fixture.archive));
            fixture.native.saveLocal(before);
            sawRisen |= !fixture.native.takeRisen().empty();
            REQUIRE(fixture.native.restoreLocal(before));
            sawCompaction |= fixture.native.count() < 3;
        }
        REQUIRE(fixture.replay.canRestoreLocal(saved));
    }
    CHECK(sawRisen);
    CHECK(sawCompaction);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("native statue history refuses different pools and loads",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot saved;
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    fixture.native.saveLocal(saved);
    CHECK(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.replay.equalLocal(saved, saved));
    CHECK_FALSE(fixture.replay.restoreLocal(saved));
    REQUIRE(fixture.native.restoreLocal(saved));
    REQUIRE(fixture.native.restoreLocal(saved));
    REQUIRE(fixture.native.add(fixture.device, fixture.archive, statueAt(90), nullptr));
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    fixture.native.saveLocal(saved);
    const auto previousId = fixture.native.instanceOf(fixture.native.count() - 1);
    fixture.native.clear();
    CHECK_FALSE(fixture.native.equalLocal(saved, saved));
    CHECK_FALSE(fixture.native.restoreLocal(saved));
    // Identities are never reused after a clear, so no older history can name them.
    REQUIRE(fixture.native.add(fixture.device, fixture.archive, statueAt(90), nullptr));
    CHECK(fixture.native.instanceOf(0) > previousId);
}

TEST_CASE("native statue equality sees contact-grid policy before any actor moves",
          "[critter-statues-local-rewind][native-equality]") {
    StatueFixture fixture;
    CritterStatues::LocalSnapshot before;
    CritterStatues::LocalSnapshot after;
    fixture.native.saveLocal(before);
    fixture.native.setContactGridBounds({-500, 0, -500}, {700, 0, 700});
    fixture.native.saveLocal(after);
    CHECK_FALSE(fixture.native.equalLocal(before, after));
    REQUIRE(fixture.native.restoreLocal(before));
    fixture.native.saveLocal(after);
    CHECK(fixture.native.equalLocal(before, after));
}

} // namespace
