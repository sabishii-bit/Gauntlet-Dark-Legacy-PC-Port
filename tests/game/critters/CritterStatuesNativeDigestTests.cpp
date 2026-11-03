#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/critters/CritterStatues.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;
constexpr std::string_view kArchiveKey = "stage/statue-native-fixture/golem";

std::filesystem::path statueDigestFixture() {
    const auto path = test::scratchDirectory("statue-native-digest");
    std::filesystem::create_directories(path / "models");
    writeTextFile(path / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(path / "objects.json", R"({"objects":[
      {"name":"STONE","file":"models/body.obj"}]})");
    writeFile(path / "skin.png", test::kTinyPng);
    writeTextFile(path / "textures.json", R"({"bitmaps":[
      {"name":"SKIN","file":"skin.png","width":2,"height":2},
      {"name":"GLOW","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(path);
    writeTextFile(path / "animations.json", R"({
      "textureAnimations":[{"name":"SKIN","texture":0,"source":0,"frames":2,"rate":3}],
      "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
                    "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"GOL_STATUE","nodes":[
        {"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]},
        {"name":"SMOKE","particle":0,"parent":0,"position":[0,2,0]}],
        "sequences":[{"name":"IDLE","frames":0,"frameRate":30},
                     {"name":"ACTIVE","frames":10,"frameRate":30}]}]})");
    return path;
}

CritterStatues::Placement placementAt(f32 x, s32 activeOn = 0) {
    CritterStatues::Placement value;
    value.kind = CombatantKind::Golem;
    value.form = "STONE";
    value.instance.info = 4;
    value.instance.name = "PLACEMENT";
    value.instance.position = {x, 2, 4};
    value.instance.rotation = {0.1f, 0.2f, 0.3f};
    value.instance.minPlayers = 2;
    value.instance.flags = 7;
    value.instance.params = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    value.instance.collision.push_back({{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}}});
    value.radius = 3;
    value.height = 5;
    value.viewRadius = 10;
    value.sight = 12;
    value.activeOn = activeOn;
    value.carried = 8;
    return value;
}

struct Pool {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterStatues statues;
    NativeStateResources resources;

    explicit Pool(const std::filesystem::path& path, usize count = 3, s32 firstActiveOn = 3) {
        REQUIRE(archive.load(path));
        statues.setContactGridBounds({-320, 0, -320}, {320, 0, 320});
        for (usize index = 0; index < count; ++index) {
            REQUIRE(statues.add(
                device, archive,
                placementAt(static_cast<f32>(index * 30), index == 0 ? firstActiveOn : 0),
                nullptr));
        }
        REQUIRE(resources.registerArchive(archive, kArchiveKey));
        REQUIRE(statues.registerNativeResources(resources));
    }

    NativeStateDigest::Digest digest() const {
        NativeStateDigest sink(resources.resources());
        REQUIRE(statues.appendNativeDigest(sink));
        const auto result = sink.checkedFinish();
        REQUIRE(result);
        return *result;
    }
    NativeStateDigest::Digest digest(const CritterStatues::LocalSnapshot& saved) const {
        NativeStateDigest sink(resources.resources());
        REQUIRE(statues.appendLocalDigest(sink, saved));
        const auto result = sink.checkedFinish();
        REQUIRE(result);
        return *result;
    }

    void step(s32 tick, bool wrong = false) {
        if (tick == 0) {
            statues.wake(0);
            statues.wake(1);
        }
        if (wrong) {
            for (usize index = 0; index < statues.count(); ++index) {
                statues.wake(index);
            }
        }
        statues.update(1, wrong ? 2 * kStep : kStep,
                       [tick, wrong](const Vec3&, f32) { return wrong || tick % 5 != 4; });
        if (wrong) {
            (void)statues.takeRisen();
        }
    }
};

NativeStateDigest::Digest placementHash(const CritterStatues::Placement& placement) {
    NativeStateDigest sink;
    sink.value(placement);
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

TEST_CASE("statue native digests resolve independent art and remain independent of drawing",
          "[native-state-digest][critter-statues-native-digest]") {
    const auto path = statueDigestFixture();
    Pool first(path);
    Pool second(path);
    REQUIRE(first.statues.figure(0).presentation().tree !=
            second.statues.figure(0).presentation().tree);
    REQUIRE(&first.statues.archive(0) != &second.statues.archive(0));
    CHECK(first.digest() == second.digest());
    const auto textures = first.device.texturesCreated;
    bool drew = false;
    bool compacted = false;
    bool drewParticles = false;
    for (s32 tick = 0; tick < 28; ++tick) {
        CAPTURE(tick);
        first.step(tick);
        second.step(tick);
        REQUIRE(first.digest() == second.digest());
        compacted = compacted || first.statues.count() < 3;
        const auto native = first.digest();
        first.statues.draw(first.device, Mat4{1}, WorldLighting{});
        // Each fixture body has one mesh; extra draws are its active smoke.
        drewParticles = drewParticles || first.device.draws.size() > first.statues.count();
        for (usize index = 0; index < first.statues.count(); ++index) {
            for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
                first.statues.figure(index).draw(first.device, Mat4{1}, WorldLighting{}, 1, 1,
                                                 nullptr, TreeModel::Pass::All, alpha);
                CHECK(first.digest() == native);
            }
        }
        drew = drew || !first.device.draws.empty();
        first.device.draws.clear();
        CHECK(first.digest() == native);
    }
    CHECK(drew);
    CHECK(compacted);
    CHECK(drewParticles);
    CHECK(first.device.texturesCreated == textures);
    CHECK(first.device.textureUpdates == 0);
}

TEST_CASE("statue native hashes include hidden placements grid wake clocks and pending births",
          "[native-state-digest][critter-statues-native-digest]") {
    const auto path = statueDigestFixture();
    Pool first(path, 1);
    const Pool otherDuration(path, 1, 4);
    CHECK(first.statues.positionOf(0) == otherDuration.statues.positionOf(0));
    CHECK(first.statues.figure(0).sequence() == otherDuration.statues.figure(0).sequence());
    CHECK(first.digest() != otherDuration.digest());
    CritterStatues::LocalSnapshot initial;
    first.statues.saveLocal(initial);
    const auto before = first.digest();
    first.statues.setContactGridBounds({-10, 0, -20}, {20, 0, 30});
    CHECK(first.digest() != before);
    REQUIRE(first.statues.restoreLocal(initial));
    first.statues.wake(0);
    CHECK_FALSE(first.statues.rising(0));
    CHECK(first.statues.figure(0).sequence() == CritterStatues::kIdleSequence);
    CHECK(first.digest() != before);
    CHECK(first.digest(initial) == before);
    first.statues.update(1, kStep, {}); // starts ACTIVE without consuming its clock
    CritterStatues::LocalSnapshot rising;
    first.statues.saveLocal(rising);
    const auto risingHash = first.digest();
    for (s32 tick = 0; tick < 6; ++tick) {
        first.statues.update(1, kStep, {});
        CHECK(first.digest(rising) == risingHash); // never reads the bank's later shared clock
    }
    REQUIRE(first.statues.count() == 0);
    CritterStatues::LocalSnapshot queued;
    first.statues.saveLocal(queued);
    const auto withBirth = first.digest();
    const auto births = first.statues.takeRisen();
    REQUIRE(births.size() == 1);
    CHECK(births[0] == placementAt(0, 3));
    CHECK(first.digest() != withBirth);
    REQUIRE(first.statues.restoreLocal(queued));
    CHECK(first.digest() == withBirth);

    // A fresh registry can still index the compacted statue from retained art.
    NativeStateResources afterCompaction;
    REQUIRE(afterCompaction.registerArchive(first.archive, kArchiveKey));
    REQUIRE(first.statues.registerNativeResources(afterCompaction));
    NativeStateDigest saved(afterCompaction.resources());
    REQUIRE(first.statues.appendLocalDigest(saved, rising));
    REQUIRE(saved.checkedFinish());
    CHECK(*saved.checkedFinish() == risingHash);
    REQUIRE(first.statues.restoreLocal(initial));
    CHECK(first.digest() == before);
}

TEST_CASE(
    "statue native one through eight tick corrections retain compaction particles and outputs",
    "[native-state-digest][critter-statues-native-digest][local-rewind]") {
    const auto path = statueDigestFixture();
    Pool reference(path);
    Pool corrected(path);
    for (s32 tick = 0; tick < 3; ++tick) {
        reference.step(tick);
        corrected.step(tick);
    }
    CritterStatues::LocalSnapshot initial;
    corrected.statues.saveLocal(initial);
    const auto initialHash = corrected.digest();
    std::array<NativeStateDigest::Digest, 8> expected{};
    std::array<CritterStatues::LocalSnapshot, 8> exact;
    for (s32 tick = 0; tick < 8; ++tick) {
        reference.step(3 + tick);
        corrected.step(3 + tick);
        expected[static_cast<usize>(tick)] = reference.digest();
        corrected.statues.saveLocal(exact[static_cast<usize>(tick)]);
        REQUIRE(corrected.digest() == expected[static_cast<usize>(tick)]);
    }
    REQUIRE(reference.statues.count() == 2);
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(corrected.statues.restoreLocal(initial));
        for (s32 tick = 0; tick < distance; ++tick) {
            corrected.step(3 + tick, true);
        }
        CHECK(corrected.digest() != expected[static_cast<usize>(distance - 1)]);
        CHECK(corrected.digest(initial) == initialHash);
        REQUIRE(corrected.statues.restoreLocal(initial));
        for (s32 tick = 0; tick < distance; ++tick) {
            corrected.step(3 + tick);
            CHECK(corrected.digest() == expected[static_cast<usize>(tick)]);
        }
        CritterStatues::LocalSnapshot replayed;
        corrected.statues.saveLocal(replayed);
        CHECK(corrected.statues.equalLocal(exact[static_cast<usize>(distance - 1)], replayed));
    }
    CHECK(corrected.statues.takeRisen() == reference.statues.takeRisen());
}

TEST_CASE("statue placement hashing includes native spawn payload and authored contact surfaces",
          "[native-state-digest][critter-statues-native-digest]") {
    const auto original = placementAt(0, 3);
    const auto before = placementHash(original);
    const auto different = [&](auto&& change) {
        auto placement = original;
        change(placement);
        CHECK_FALSE(placement == original);
        CHECK(placementHash(placement) != before);
    };
    different([](auto& placement) { placement.instance.params[11] = 99; });
    different([](auto& placement) { placement.instance.collision[0].vertices[2].y = 4; });
    different([](auto& placement) { placement.onFloor = false; });
    different([](auto& placement) { placement.carried = 12; });
    different([](auto& placement) { placement.support = 7; });
    different([](auto& placement) { placement.supportLocal[3].x = 12; });
    different([](auto& placement) { placement.enemy = EnemySpawn{.kind = kDeathKind}; });
    auto death = original;
    death.enemy = EnemySpawn{.kind = kDeathKind};
    const auto deathHash = placementHash(death);
    death.enemy->throwInterval = 0.125f;
    CHECK(placementHash(death) != deathHash);
}

TEST_CASE("statue native digests reject missing stale foreign and unloaded bindings",
          "[native-state-digest][critter-statues-native-digest]") {
    const auto path = statueDigestFixture();
    Pool first(path);
    Pool second(path);
    CritterStatues::LocalSnapshot saved;
    const CritterStatues::LocalSnapshot unset{};
    first.statues.saveLocal(saved);
    NativeStateDigest unknown;
    CHECK_FALSE(first.statues.appendNativeDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
    NativeStateDigest missing(first.resources.resources());
    CHECK_FALSE(first.statues.appendLocalDigest(missing, unset));
    CHECK_FALSE(missing.checkedFinish());
    NativeStateDigest foreign(second.resources.resources());
    CHECK_FALSE(second.statues.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    CHECK_FALSE(second.statues.restoreLocal(saved));

    first.statues.clear();
    NativeStateDigest cleared(first.resources.resources());
    CHECK_FALSE(first.statues.appendLocalDigest(cleared, saved));
    CHECK_FALSE(cleared.checkedFinish());
    second.statues.saveLocal(saved);
    // A stage invalidates histories/registry before reloading an archive at the
    // same address. An unloaded archive is rejected even before that teardown.
    second.archive.clear();
    CHECK_FALSE(second.statues.canRestoreLocal(saved));
    NativeStateDigest unloaded(second.resources.resources());
    CHECK_FALSE(second.statues.appendNativeDigest(unloaded));
    CHECK_FALSE(unloaded.checkedFinish());
    NativeStateDigest unloadedSaved(second.resources.resources());
    CHECK_FALSE(second.statues.appendLocalDigest(unloadedSaved, saved));
    CHECK_FALSE(unloadedSaved.checkedFinish());
    CHECK_FALSE(second.statues.registerNativeResources(second.resources));
}

TEST_CASE("moving statue rewind retains its parent through idle waking and queued birth",
          "[critter-statues-native-digest][statue-platform][local-rewind]") {
    Pool pool(statueDigestFixture(), 0);
    WorldCollision collision;
    collision.build({CollisionTriangle{
        .vertices = {Vec3{-20, 0, -20}, Vec3{20, 0, -20}, Vec3{0, 0, 20}}, .object = 7}});
    collision.setMovingObjects(std::array<s32, 1>{7});
    auto placement = placementAt(2, 3);
    placement.support = 7;
    placement.supportLocal =
        itemPlacement(placement.instance.position, placement.instance.rotation);
    REQUIRE(pool.statues.add(pool.device, pool.archive, placement, &collision));
    REQUIRE(pool.statues.registerNativeResources(pool.resources));
    const Mat4 carried =
        glm::translate(Mat4{1}, Vec3{20, 5, -6}) * glm::rotate(Mat4{1}, 0.7f, Vec3{0, 1, 0});
    collision.setObjectTransform(7, carried);
    collision.setSolid(7, false); // moving trigger's collision does not sever the parent
    pool.statues.syncFloors();
    CHECK(glm::distance(pool.statues.positionOf(0), Vec3{carried * placement.supportLocal[3]}) <
          0.0001f);
    CHECK_FALSE(pool.statues.woken(0));
    CHECK(pool.statues.figure(0).sequence() == CritterStatues::kIdleSequence);
    CritterStatues::LocalSnapshot idle;
    WorldCollision::LocalSnapshot floor;
    pool.statues.saveLocal(idle);
    collision.saveLocal(floor);
    const auto idleHash = pool.digest();
    const auto replay = [&] {
        pool.statues.wake(0);
        pool.statues.update(1, kStep, {});
        REQUIRE(pool.statues.rising(0));
        for (s32 tick = 0; tick < 6; ++tick) {
            const Mat4 parent =
                glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 0, 0}) * carried;
            collision.setObjectTransform(7, parent);
            pool.statues.update(1, kStep, {});
            if (pool.statues.count() != 0) {
                CHECK(glm::distance(pool.statues.positionOf(0),
                                    Vec3{parent * placement.supportLocal[3]}) < 0.0001f);
            }
        }
        REQUIRE(pool.statues.count() == 0);
        REQUIRE(pool.statues.checkpoint()); // queued definitions still match the fixed roster
    };
    replay();
    CritterStatues::LocalSnapshot queued;
    pool.statues.saveLocal(queued);
    const auto queuedHash = pool.digest();
    const auto born = pool.statues.takeRisen();
    REQUIRE(born.size() == 1);
    const Mat4 expected = *collision.objectTransform(7) * placement.supportLocal;
    CHECK(glm::distance(born[0].instance.position, Vec3{expected[3]}) < 0.0001f);
    const Mat4 facing = itemPlacement(born[0].instance.position, born[0].instance.rotation);
    const Vec3 planar = glm::normalize(Vec3{expected[2].x, 0, expected[2].z});
    CHECK(glm::distance(Vec3{facing[2]}, planar) < 0.0001f);
    REQUIRE(collision.restoreLocal(floor));
    REQUIRE(pool.statues.restoreLocal(idle));
    CHECK(pool.digest() == idleHash);
    replay();
    CritterStatues::LocalSnapshot replayed;
    pool.statues.saveLocal(replayed);
    CHECK(pool.statues.equalLocal(queued, replayed));
    CHECK(pool.digest() == queuedHash);
    CHECK(pool.statues.takeRisen() == born);
}

TEST_CASE("statue native digest bytes stay pinned for a woken roster",
          "[native-state-digest][critter-statues-native-digest][native-digest-pin]") {
    Pool pool(statueDigestFixture(), 2, 4);
    pool.statues.setContactGridBounds({-10, 0, -20}, {20, 0, 30});
    pool.statues.wake(1);
    // SHA-256 of the native digest of the statues above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "9ba2bd4d3cd3c264ed9d76b3ea4986e5af6c9352e27cf0e963183a591da4f84b";
    CHECK(test::hexBytes(pool.digest()) == kDigest);
}
} // namespace
