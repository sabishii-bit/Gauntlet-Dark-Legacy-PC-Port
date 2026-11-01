#include <array>
#include <memory>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/bosses/Bosses.h"
#include "game/critters/CombatantCheckpoint.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct NativeBoss {
    test::FakeRenderDevice device;
    Bosses boss;
    s32 kind;
    explicit NativeBoss(s32 which) : kind(which) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        boss.open(device, root, nullptr, {}, 'G');
    }
    void step(s32 frame, bool wrong = false) {
        if (frame == 0) {
            REQUIRE(boss.spawn(kind, {}, 0, 30));
        }
        if (frame == 20 && boss.legendRealm() != 0) {
            REQUIRE(boss.bringLegend(wrong ? 3 : 1));
        }
        if (frame > 150 && boss.legend().thrown()) {
            boss.landLegend();
        }
        if (frame == 1300) {
            EnemyHit hit;
            hit.player = wrong ? 2 : 1;
            hit.damage = boss.view().maxHealth * (wrong ? 0.01f : 16.0f);
            hit.level = 99;
            boss.hurt(hit);
        }
        std::array<EnemyView, 4> party;
        for (s32 seat = 0; seat < 4; ++seat) {
            auto& player = party[static_cast<usize>(seat)];
            player.player = seat;
            player.radius = 1;
            player.height = 6;
            player.position = {static_cast<f32>(seat * 3 - 4), 0,
                               static_cast<f32>((wrong ? 70 : 12) + frame % 9)};
        }
        boss.update(1, 1.0f / 60, party);
        if (frame % 11 == 0) {
            boss.takeCues();
            boss.takeShots();
            boss.takeBlows();
            boss.takeLosses();
            boss.takeLegendEvents();
        }
    }
    CombatantCheckpoint::State observeActor() {
        std::vector<CombatantCheckpoint::Resource> resources;
        for (auto* resource : boss.resources()) {
            resources.push_back({static_cast<u32>(resources.size() + 1), resource});
        }
        // The adapter only reads. Native local save/restore is the sole rewind path.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const CombatantCheckpoint observer{const_cast<Combatant&>(boss.fighter()), resources, {}};
        const auto state = observer.checkpoint();
        REQUIRE(state);
        return *state;
    }
    void equal(NativeBoss& expected) {
        const auto actual = boss.view();
        const auto reference = expected.boss.view();
        CHECK((std::tie(actual.kind, actual.name, actual.health, actual.maxHealth, actual.awake,
                        actual.alive) == std::tie(reference.kind, reference.name, reference.health,
                                                  reference.maxHealth, reference.awake,
                                                  reference.alive)));
        CHECK(boss.present() == expected.boss.present());
        CHECK(boss.cameraBase() == expected.boss.cameraBase());
        CHECK(boss.rewardOffset() == expected.boss.rewardOffset());
        const auto legendValues = [](const LegendRite& rite) {
            return std::tuple{rite.stage(),     rite.player(), rite.running(),   rite.holdsBoss(),
                              rite.wantsRoar(), rite.thrown(), rite.weakening(), rite.darkens()};
        };
        CHECK((legendValues(boss.legend()) == legendValues(expected.boss.legend())));
        CHECK(observeActor() == expected.observeActor());
        const auto resources = boss.resources();
        const auto referenceResources = expected.boss.resources();
        REQUIRE(resources.size() == referenceResources.size());
        for (usize i = 0; i < resources.size(); ++i) {
            CHECK(resources[i]->definition.name == referenceResources[i]->definition.name);
            CHECK(resources[i]->textures.clock() == referenceResources[i]->textures.clock());
        }
    }
};

TEST_CASE("native boss encounter rewind covers waking legend impact death and reward timing",
          "[local-rewind][boss-local-rewind][native-equality][assets]") {
    const s32 kind = GENERATE(34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44);
    CAPTURE(kind);
    auto expected = std::make_unique<NativeBoss>(kind);
    auto replay = std::make_unique<NativeBoss>(kind);
    LocalStateHistory<Bosses::LocalSnapshot> history;
    s32 frame = 0;
    for (s32 window = 0; frame < 1800; ++window) {
        CAPTURE(frame, window);
        const s32 distance = window % 8 + 1;
        REQUIRE(history.capture(static_cast<u64>(frame),
                                [&](auto& value) { return replay->boss.saveLocal(value); }));
        const auto* saved = history.find(static_cast<u64>(frame));
        REQUIRE(saved);
        std::vector<Bosses::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            replay->step(frame + i);
            REQUIRE(replay->boss.saveLocal(nativeExpected[static_cast<usize>(i)]));
        }
        REQUIRE(replay->boss.restoreLocal(*saved));
        for (s32 i = 0; i < distance; ++i) {
            replay->step(frame + i, true);
        }
        REQUIRE(replay->boss.restoreLocal(*saved));
        replay->equal(*expected);
        for (s32 i = 0; i < distance; ++i) {
            expected->step(frame + i);
            replay->step(frame + i);
            Bosses::LocalSnapshot corrected;
            REQUIRE(replay->boss.saveLocal(corrected));
            CHECK(replay->boss.equalLocal(corrected, nativeExpected[static_cast<usize>(i)]));
        }
        replay->equal(*expected);
        CHECK(replay->boss.takeDefeat() == expected->boss.takeDefeat());
        const auto actualEvents = replay->boss.takeLegendEvents();
        const auto expectedEvents = expected->boss.takeLegendEvents();
        REQUIRE(actualEvents.size() == expectedEvents.size());
        for (usize i = 0; i < actualEvents.size(); ++i) {
            CHECK(actualEvents[i].cue == expectedEvents[i].cue);
            CHECK(actualEvents[i].player == expectedEvents[i].player);
            CHECK(actualEvents[i].realm == expectedEvents[i].realm);
        }
        frame += distance;
    }
}

TEST_CASE("boss local history retains loaded artwork and rejects stage teardown",
          "[local-rewind][boss-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<NativeBoss>(35);
    Bosses::LocalSnapshot cold;
    Bosses::LocalSnapshot active;
    REQUIRE(encounter->boss.saveLocal(cold));
    REQUIRE(encounter->boss.spawn(35, {}, 0, 30));
    REQUIRE(encounter->boss.saveLocal(active));
    const auto* model = encounter->boss.resources().front();
    const auto* child = encounter->boss.fighter().child(1);
    REQUIRE(child);
    const auto uploads = encounter->device.texturesCreated;
    REQUIRE(encounter->boss.restoreLocal(cold));
    CHECK_FALSE(encounter->boss.present());
    CHECK(encounter->boss.resources().empty());
    REQUIRE(encounter->boss.spawn(35, {}, 0, 30));
    CHECK(encounter->boss.resources().front() == model);
    CHECK(encounter->boss.fighter().child(1) == child);
    REQUIRE(encounter->boss.restoreLocal(active));
    Bosses::LocalSnapshot corrected;
    REQUIRE(encounter->boss.saveLocal(corrected));
    CHECK(encounter->boss.equalLocal(active, corrected));
    CHECK(encounter->device.texturesCreated == uploads);
    encounter->boss.close();
    CHECK_FALSE(encounter->boss.restoreLocal(active));
    CHECK_FALSE(encounter->boss.equalLocal(active, active));
    CHECK_FALSE(encounter->boss.restoreLocal(cold));
}

TEST_CASE("native boss equality observes waiting clocks before any fighter is visible",
          "[local-rewind][boss-local-rewind][native-equality]") {
    Bosses boss;
    const Bosses foreign{};
    Bosses::LocalSnapshot initial;
    Bosses::LocalSnapshot after;
    CHECK_FALSE(boss.equalLocal(initial, initial));
    REQUIRE(boss.saveLocal(initial));
    CHECK_FALSE(foreign.equalLocal(initial, initial));
    boss.arm(1.0f / 60);
    REQUIRE(boss.saveLocal(after));
    CHECK_FALSE(boss.present());
    CHECK_FALSE(boss.equalLocal(initial, after));
    CHECK_FALSE(boss.equalLocal(after, initial));
    REQUIRE(boss.restoreLocal(initial));
    REQUIRE(boss.saveLocal(after));
    CHECK(boss.equalLocal(initial, after));
}

TEST_CASE("boss legend presentation configuration is immutable within a rewind epoch",
          "[local-rewind][boss-local-rewind][legend-profile]") {
    test::FakeRenderDevice device;
    Bosses boss;
    const auto digest = [&] {
        NativeStateDigest value;
        REQUIRE(boss.appendNativeDigest(value));
        REQUIRE(value.checkedFinish());
        return *value.checkedFinish();
    };
    boss.open(device, {}, nullptr, {}, 'G');
    const auto ordinary = digest();
    LegendProfile profile;
    profile.targetNode = 2;
    boss.open(device, {}, nullptr, {}, 'G', {}, &profile);
    const auto imported = digest();
    CHECK(imported != ordinary);
    Bosses::LocalSnapshot saved;
    REQUIRE(boss.saveLocal(saved));
    profile.targetNode = 3;
    profile.eyeReplacement = "OTHER_EYE";
    CHECK(digest() == imported);
    REQUIRE(boss.restoreLocal(saved));
    NativeStateDigest restored;
    REQUIRE(boss.appendLocalDigest(restored, saved));
    CHECK(restored.checkedFinish() == imported);
    boss.open(device, {}, nullptr, {}, 'G', {}, &profile);
    CHECK(digest() != imported);
    CHECK_FALSE(boss.restoreLocal(saved));
    boss.open(device, {}, nullptr, {}, 'G');
    CHECK(digest() == ordinary);
}

TEST_CASE("native legend equality includes waiting and recovery between public phase changes",
          "[local-rewind][boss-local-rewind][native-equality]") {
    LegendRite rite;
    const auto* weakness = legendWeaknessOf(38);
    REQUIRE(weakness);
    rite.begin(1, *weakness);
    REQUIRE_FALSE(rite.update(1, true, false).empty());
    auto saved = rite;
    CHECK(rite.equalLocal(saved));
    CHECK(rite.update(1, true, false).empty());
    CHECK(rite.stage() == saved.stage());
    CHECK(rite.thrown() == saved.thrown());
    CHECK(rite.wantsRoar() == saved.wantsRoar());
    CHECK_FALSE(rite.equalLocal(saved));
    rite = saved;
    CHECK(rite.equalLocal(saved));

    rite.update(LegendRite::kLongRoarWait, true, true);
    REQUIRE(rite.running());
    saved = rite;
    CHECK(rite.update(1, true, true).empty());
    CHECK(rite.stage() == saved.stage());
    CHECK_FALSE(rite.equalLocal(saved));
}
} // namespace
