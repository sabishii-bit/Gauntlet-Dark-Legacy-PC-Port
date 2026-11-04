#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/bosses/BossDefinition.h"
#include "game/critters/Combatant.h"
#include "game/critters/Gargoyle.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"
#include "game/level/NativeStateResources.h"
#include "game/rollback/LocalStateHistory.h"
#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

/** The native digest of `actor`, over `stock` registered under the key every load of the
 * family shares, so two loads compare alike. */
NativeStateDigest::Digest digestOf(const Combatant& actor, const CombatantAssets& stock) {
    NativeStateResources resources;
    REQUIRE(resources.registerCombatantAssets(stock, "combatant-local-rewind"));
    return test::liveDigest(actor, resources.resources());
}

CombatantDefinition familyOf(std::string_view name) {
    if (name == "GENERAL") {
        return General::definition();
    }
    if (name == "GOLEM") {
        return Golem::definition();
    }
    if (name == "GARG") {
        return Gargoyle::definition();
    }
    return bossDefinition(name);
}

struct Encounter {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    Combatant reference, predicted;

    explicit Encounter(std::string_view name) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        REQUIRE(stock.load(device, root, familyOf(name), 'G'));
        for (auto* actor : {&reference, &predicted}) {
            REQUIRE(actor->spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
        }
    }

    NativeStateDigest::Digest digest(const Combatant& actor) const {
        return digestOf(actor, stock);
    }

    void step(Combatant& actor, s32 frame, bool wrong = false) {
        if (frame % 83 == 0) {
            REQUIRE(actor.spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
            actor.setArenaTargets(
                std::array{CombatArenaTarget{2, glm::translate(Mat4{1}, Vec3{3, 0, 8}), false},
                           CombatArenaTarget{5, glm::translate(Mat4{1}, Vec3{-3, 0, 8}), true}});
            actor.setArenaAnchors(std::array{Mat4{1}});
        }
        if (frame % 83 == 71) {
            EnemyHit hit;
            hit.player = wrong ? 3 : 1;
            hit.level = 99;
            hit.damage = wrong ? 3.0f : actor.maxHealth() * 16;
            actor.hurt(hit);
        }
        if (frame % 31 == 5) {
            actor.freeze(wrong ? 2 : 4);
        }
        if (frame % 37 == 6) {
            actor.blind(wrong ? 3 : 7);
        }
        if (frame % 41 == 7) {
            actor.curb(wrong ? 0.1f : 0.2f);
        }
        actor.hold(frame % 53 < 3);
        actor.setShrink(frame % 47 < 8 ? 0.5f : 1.0f);
        actor.damagedPlayer(wrong ? 3 : 1, 0.5f);
        std::array<EnemyView, 4> players;
        for (s32 seat = 0; seat < 4; ++seat) {
            auto& view = players[static_cast<usize>(seat)];
            view.player = seat;
            view.radius = 1;
            view.height = 6;
            view.position = {static_cast<f32>(seat * 3 - 5), 0,
                             static_cast<f32>(frame % 11 + (wrong ? 25 : 6))};
        }
        actor.update(1, kStep, players, {}, frame % 59 < 5);
        if (frame % 13 == 0) {
            // Some outputs survive a tick boundary; others have already been
            // consumed. Both paths must be restored exactly on correction.
            actor.takeCues();
            actor.takeBlows();
            actor.takeShots();
            actor.takeLosses();
        }
    }

    void equal() const { CHECK(digest(predicted) == digest(reference)); }
};

/** One family loaded on its own device, with one fighter spawned from it. */
struct Fighter {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    Combatant actor;
    explicit Fighter(const CombatantDefinition& family) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        REQUIRE(stock.load(device, root, family, 'G'));
        REQUIRE(actor.spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
    }
    NativeStateDigest::Digest digest() const { return digestOf(actor, stock); }
};

EnemyView visiblePlayer(s32 id, Vec3 position) {
    EnemyView view;
    view.player = id;
    view.position = position;
    view.radius = 1;
    view.height = 6;
    return view;
}

TEST_CASE("native combatant families replay corrected one to eight tick histories",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    const auto* const name =
        GENERATE("GENERAL", "GOLEM", "GARG", "CHIMERA", "LICH", "GARM", "YETI", "DRAGON", "DJINN",
                 "DRIDER", "PBOSS", "WRAITH", "SKORNE1", "SKORNE2");
    CAPTURE(name);
    auto encounter = std::make_unique<Encounter>(name);
    LocalStateHistory<Combatant::LocalSnapshot> history;
    s32 frame = 0;
    const u32 uploads = encounter->device.texturesCreated;
    for (s32 window = 0; window < 64; ++window) {
        const s32 distance = window % 8 + 1;
        REQUIRE(history.capture(static_cast<u64>(frame), [&](auto& saved) {
            return encounter->predicted.saveLocal(saved);
        }));
        const auto* saved = history.find(static_cast<u64>(frame));
        REQUIRE(saved);
        // The native comparison uses this exact owner and retained bindings;
        // the independent second actor remains a separate diagnostic oracle.
        std::vector<Combatant::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            encounter->step(encounter->predicted, frame + i);
            REQUIRE(encounter->predicted.saveLocal(nativeExpected[static_cast<usize>(i)]));
        }
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        for (s32 i = 0; i < distance; ++i) {
            encounter->step(encounter->predicted, frame + i, true);
        }
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        encounter->equal();
        for (s32 i = 0; i < distance; ++i) {
            CAPTURE(window, distance, frame, i);
            encounter->step(encounter->reference, frame + i);
            encounter->step(encounter->predicted, frame + i);
            encounter->equal();
            Combatant::LocalSnapshot corrected;
            REQUIRE(encounter->predicted.saveLocal(corrected));
            CHECK(
                encounter->predicted.equalLocal(corrected, nativeExpected[static_cast<usize>(i)]));
        }
        frame += distance;
    }
    CHECK(encounter->device.texturesCreated == uploads);
    CHECK(encounter->device.textureUpdates == 0);
}

TEST_CASE("local combatant rewind retains boss child addresses across clear and kind reuse",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA");
    auto& actor = encounter->predicted;
    REQUIRE(actor.childCount() == 3);
    std::array<const Combatant*, 3> children{};
    for (s32 i = 0; i < 3; ++i) {
        children[static_cast<usize>(i)] = actor.child(41 + i);
    }
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto original = encounter->digest(actor);
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    CombatantAssets general;
    REQUIRE(general.load(encounter->device, root, General::definition(), 'G'));
    const auto uploads = encounter->device.texturesCreated;
    for (s32 run = 0; run < 3; ++run) {
        actor.clear();
        CHECK(actor.childCount() == 0);
        CHECK(actor.child(41) == nullptr);
        REQUIRE(actor.spawn(general, 80, {2, 0, 3}, 1, nullptr, {}, 'G'));
        REQUIRE(actor.restoreLocal(saved));
        CHECK(encounter->digest(actor) == original);
        Combatant::LocalSnapshot corrected;
        REQUIRE(actor.saveLocal(corrected));
        CHECK(actor.equalLocal(saved, corrected));
        for (s32 i = 0; i < 3; ++i) {
            CHECK(actor.child(41 + i) == children[static_cast<usize>(i)]);
        }
    }
    CHECK(encounter->device.texturesCreated == uploads);
}

TEST_CASE("local combatant snapshots fence owners assets and borrowed per-tick contacts",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    Combatant::LocalSnapshot saved;
    Combatant::LocalSnapshot empty;
    REQUIRE(actor.saveLocal(saved));
    CHECK(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.restoreLocal(empty));
    CHECK_FALSE(actor.equalLocal(empty, empty));
    CHECK_FALSE(encounter->reference.restoreLocal(saved));
    CHECK_FALSE(encounter->reference.equalLocal(saved, saved));
    const auto original = encounter->digest(actor);
    REQUIRE(actor.restoreLocal(saved));
    std::array contacts{CombatantObstacle{}};
    actor.setObstacles(contacts);
    CHECK_FALSE(actor.restoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.saveLocal(empty));
    actor.setObstacles({});
    std::array bodies{EnemyBody{}};
    actor.setSwarm(bodies);
    CHECK_FALSE(actor.restoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.saveLocal(empty));
    actor.setSwarm({});
    REQUIRE(actor.restoreLocal(saved));
    CHECK(encounter->digest(actor) == original);
    encounter->stock.clear();
    CHECK_FALSE(actor.canRestoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.restoreLocal(saved));
    actor.clear();
    encounter->reference.clear();
}

TEST_CASE("local combatant model replacements use retained resources after correction",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    const auto found = std::ranges::find_if(encounter->stock.tree->nodes, [&](const auto& node) {
        return !node.object.empty() &&
               encounter->stock.archive.models.find(node.object).has_value();
    });
    REQUIRE(found != encounter->stock.tree->nodes.end());
    REQUIRE(actor.replaceNodeModel(encounter->device, found->name, found->object));
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto expected = encounter->digest(actor);
    const auto uploads = encounter->device.texturesCreated;
    actor.tint(Color{12, 98, 133, 255});
    REQUIRE(actor.replaceNodeModel(encounter->device, found->name, ""));
    REQUIRE(actor.restoreLocal(saved));
    CHECK(encounter->digest(actor) == expected);
    actor.draw(encounter->device, Mat4{1}, WorldLighting{});
    CHECK(encounter->device.texturesCreated == uploads);
    CHECK(encounter->device.textureUpdates == 0);
    CHECK_FALSE(encounter->device.draws.empty());
    Combatant::LocalSnapshot rendered;
    REQUIRE(actor.saveLocal(rendered));
    CHECK(actor.equalLocal(saved, rendered)); // shared draw caches are not native state
}

TEST_CASE("local combatant rewind has no portable-packet scalar admission limits",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    EnemyHit hit;
    hit.player = 2;
    hit.level = 99;
    hit.damage = 4000000;
    actor.hurt(hit);
    REQUIRE(actor.dying());
    const f32 health = actor.health();
    REQUIRE(health < -1000000);
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    actor.clear();
    REQUIRE(actor.restoreLocal(saved));
    CHECK(actor.dying());
    CHECK(actor.health() == health);
    Combatant::LocalSnapshot after;
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(saved, after));
    REQUIRE_FALSE(actor.takeLosses().empty());
    REQUIRE(actor.saveLocal(after));
    CHECK_FALSE(actor.equalLocal(saved, after));
    REQUIRE(actor.restoreLocal(saved));
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(saved, after));
}

TEST_CASE("destroyed combatant owners cannot restore into reused addresses",
          "[local-rewind][combatant-local-rewind][native-equality]") {
    alignas(Combatant) std::array<std::byte, sizeof(Combatant)> storage{};
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* actor = std::construct_at(reinterpret_cast<Combatant*>(storage.data()));
    Combatant::LocalSnapshot saved;
    REQUIRE(actor->saveLocal(saved));
    std::destroy_at(actor);
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    actor = std::construct_at(reinterpret_cast<Combatant*>(storage.data()));
    CHECK_FALSE(actor->restoreLocal(saved));
    CHECK_FALSE(actor->equalLocal(saved, saved));
    std::destroy_at(actor);
}

TEST_CASE("native combatant equality observes hidden inactive values",
          "[local-rewind][combatant-local-rewind][native-equality]") {
    Combatant actor;
    Combatant::LocalSnapshot initial;
    REQUIRE(actor.saveLocal(initial));
    const auto changed = [&](auto&& change) {
        REQUIRE(actor.restoreLocal(initial));
        change();
        CHECK_FALSE(actor.present());
        Combatant::LocalSnapshot after;
        REQUIRE(actor.saveLocal(after));
        CHECK_FALSE(actor.equalLocal(initial, after));
        CHECK_FALSE(actor.equalLocal(after, initial));
        REQUIRE(actor.restoreLocal(initial));
        REQUIRE(actor.saveLocal(after));
        CHECK(actor.equalLocal(initial, after));
    };
    changed([&] { actor.takeFullHarm(true); });
    changed([&] { actor.tint(Color{21, 32, 43, 255}); });
    changed([&] { actor.setArenaAnchors(std::array{glm::translate(Mat4{1}, Vec3{3, 0, 4})}); });
    changed([&] {
        actor.setArenaTargets(
            std::array{CombatArenaTarget{2, Mat4{1}, false}, CombatArenaTarget{5, Mat4{1}, true}});
    });
}

TEST_CASE("native combatant equality retains a cleared slot's next incarnation",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GENERAL");
    auto& actor = encounter->predicted;
    actor.clear();
    Combatant::LocalSnapshot before;
    Combatant::LocalSnapshot after;
    REQUIRE(actor.saveLocal(before));
    const auto incarnation = actor.incarnation();
    REQUIRE(actor.spawn(encounter->stock, 40, {}, 0, nullptr, {}, 'G'));
    actor.clear();
    REQUIRE(actor.saveLocal(after));
    CHECK_FALSE(actor.present());
    CHECK(actor.incarnation() == incarnation + 1);
    CHECK_FALSE(actor.equalLocal(before, after));
    REQUIRE(actor.restoreLocal(before));
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(before, after));
}

TEST_CASE("measure native multi-part combatant copy and eight tick replay",
          "[.][local-rewind-timing][combatant-local-rewind-timing][assets]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA");
    LocalStateHistory<Combatant::LocalSnapshot> history;
    for (s32 i = 0; i < 60; ++i) {
        encounter->step(encounter->predicted, i);
    }
    const auto capture = [&](auto& value) { return encounter->predicted.saveLocal(value); };
    for (u64 i = 0;
         i < gdl::game::LocalStateHistory<gdl::game::Combatant::LocalSnapshot>::kCapacity; ++i) {
        REQUIRE(history.capture(i, capture));
    }
    using Clock = std::chrono::steady_clock;
    std::vector<f64> saves;
    std::vector<f64> restores;
    std::vector<f64> replays;
    for (u64 sample = 16; sample < 272; ++sample) {
        const auto begin = Clock::now();
        const bool captured = history.capture(sample, capture);
        const auto end = Clock::now();
        REQUIRE(captured);
        const auto* saved = history.find(sample);
        REQUIRE(saved);
        for (s32 i = 60; i < 68; ++i) {
            encounter->step(encounter->predicted, i, true);
        }
        const auto restoreStart = Clock::now();
        const bool restored = encounter->predicted.restoreLocal(*saved);
        const auto restoreEnd = Clock::now();
        REQUIRE(restored);
        const auto replayStart = Clock::now();
        for (s32 i = 60; i < 68; ++i) {
            encounter->step(encounter->predicted, i);
        }
        const auto replayEnd = Clock::now();
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        saves.push_back(std::chrono::duration<f64, std::milli>(end - begin).count());
        restores.push_back(
            std::chrono::duration<f64, std::milli>(restoreEnd - restoreStart).count());
        replays.push_back(std::chrono::duration<f64, std::milli>(replayEnd - replayStart).count());
    }
    const auto report = [](const char* operation, const std::vector<f64>& samples) {
        const auto summary = test::summarizeNativeTimings(samples);
        REQUIRE(summary);
        std::cout << "combatant-local-" << operation << " samples=" << summary->samples
                  << " mean_ms=" << summary->mean << " p99_ms=" << summary->p99
                  << " max_ms=" << summary->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}

TEST_CASE("native retired combatant slots preserve identity and may be restored before reuse",
          "[combatant-local-rewind][assets]") {
    Fighter native(General::definition());
    Fighter replay(General::definition());
    const s32 id = native.actor.id();
    const u64 incarnation = native.actor.incarnation();
    EnemyHit hit;
    hit.player = 2;
    hit.damage = 1000000;
    native.actor.hurt(hit);
    replay.actor.hurt(hit);
    for (s32 tick = 0; tick < 1200 && native.actor.present(); ++tick) {
        native.actor.update(2, 1.0f / 30, {});
        replay.actor.update(2, 1.0f / 30, {});
    }
    REQUIRE_FALSE(native.actor.present());
    Combatant::LocalSnapshot retired;
    REQUIRE(native.actor.saveLocal(retired));
    CHECK(native.actor.data() == nullptr);
    CHECK(native.actor.id() == id);
    CHECK(native.actor.incarnation() == incarnation);
    REQUIRE_FALSE(native.actor.takeLosses().empty());
    REQUIRE(native.actor.restoreLocal(retired));
    const auto uploads = native.device.texturesCreated;
    CHECK(replay.digest() == native.digest());
    REQUIRE(native.actor.spawn(native.stock, 40, {2, 0, 3}, 0, nullptr, {}, 'G'));
    REQUIRE(replay.actor.spawn(replay.stock, 40, {2, 0, 3}, 0, nullptr, {}, 'G'));
    Combatant::LocalSnapshot reused;
    REQUIRE(native.actor.saveLocal(reused));
    CHECK(native.actor.incarnation() > incarnation);
    CHECK(replay.digest() == native.digest());
    REQUIRE(native.actor.restoreLocal(retired));
    REQUIRE(native.actor.restoreLocal(reused));
    Combatant::LocalSnapshot after;
    REQUIRE(native.actor.saveLocal(after));
    CHECK(native.actor.equalLocal(after, reused));
    CHECK(native.device.texturesCreated == uploads);
}

TEST_CASE(
    "native great creature continuation restores animation contacts damage and ordered queues",
    "[combatant-local-rewind][assets]") {
    const s32 rate = GENERATE(30, 60);
    const s32 family = GENERATE(0, 1, 2);
    const std::array definitions{Golem::definition(), General::definition(),
                                 Gargoyle::definition()};
    CAPTURE(rate, family);
    auto first = std::make_unique<Fighter>(definitions[static_cast<usize>(family)]);
    auto replay = std::make_unique<Fighter>(definitions[static_cast<usize>(family)]);
    auto views = std::array{visiblePlayer(3, {1, 0, 8}), visiblePlayer(1, {-8, 0, 11})};
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 13;
    hit.level = 20;
    hit.flags = 0x10;
    hit.close = true;
    for (auto* fighter : {first.get(), replay.get()}) {
        for (s32 frame = 0; frame < 25; ++frame) {
            fighter->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views);
        }
        fighter->actor.damagedPlayer(3, 7);
        fighter->actor.freeze(3);
        fighter->actor.blind(7);
        fighter->actor.curb(0.2f);
        fighter->actor.setShrink(0.75f);
        fighter->actor.tint(Color{31, 82, 193, 201});
        fighter->actor.hurt(hit);
    }
    const auto baseline = first->digest();
    const u32 textures = replay->device.texturesCreated;
    CHECK(replay->digest() == baseline);
    bool changed = false;
    for (s32 frame = 0; frame < 150; ++frame) {
        CAPTURE(frame);
        views[0].position.x = frame < 60 ? 1.0f : -3.0f;
        views[1].hidden = frame > 90;
        first->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views, {},
                            frame >= 40 && frame < 50);
        replay->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views, {},
                             frame >= 40 && frame < 50);
        const auto expected = first->digest();
        CHECK(replay->digest() == expected);
        changed = changed || expected != baseline;
        if (frame == 73) {
            Combatant::LocalSnapshot undo;
            REQUIRE(replay->actor.saveLocal(undo));
            replay->actor.hurt(hit);
            replay->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views);
            REQUIRE(replay->actor.canRestoreLocal(undo));
            REQUIRE(replay->actor.restoreLocal(undo));
            CHECK(replay->digest() == expected);
        }
    }
    CHECK(changed);
    CHECK(replay->device.texturesCreated == textures);
    CHECK(replay->device.textureUpdates == 0);
    CHECK(replay->device.draws.empty());
}

TEST_CASE("combatant restore binds native child poses and auxiliary clocks without spawning",
          "[combatant-local-rewind][assets]") {
    const auto* const name = GENERATE("CHIMERA", "LICH");
    auto first = std::make_unique<Fighter>(bossDefinition(name));
    auto replay = std::make_unique<Fighter>(bossDefinition(name));
    const auto views = std::array{visiblePlayer(3, {2, 0, 15}), visiblePlayer(1, {-9, 0, 11})};
    const std::array arena{CombatArenaTarget{2, glm::translate(Mat4{1}, Vec3{3, 0, 8}), false},
                           CombatArenaTarget{5, glm::translate(Mat4{1}, Vec3{-3, 0, 8}), true}};
    for (auto* fighter : {first.get(), replay.get()}) {
        for (s32 frame = 0; frame < 45; ++frame) {
            fighter->actor.update(1, 1.0f / 60, views);
        }
        fighter->actor.setArenaTargets(arena);
        fighter->actor.setArenaAnchors(std::array{Mat4{1}});
    }
    CHECK(replay->digest() == first->digest());
    if (std::string_view(name) == "CHIMERA") {
        REQUIRE(first->actor.childCount() == 3);
    } else {
        REQUIRE(first->stock.attachments.size() == 1);
    }
    Combatant::LocalSnapshot baseline;
    REQUIRE(replay->actor.saveLocal(baseline));
    // A wrong branch respawns the body and moves its parts and attachments elsewhere.
    const auto astray = std::array{visiblePlayer(1, {-6, 0, 4})};
    REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
    for (s32 frame = 0; frame < 10; ++frame) {
        replay->actor.update(1, 1.0f / 60, astray);
    }
    CHECK(replay->digest() != first->digest());
    const u32 uploads = replay->device.texturesCreated;
    REQUIRE(replay->actor.restoreLocal(baseline));
    CHECK(replay->digest() == first->digest());
    for (s32 frame = 0; frame < 80; ++frame) {
        CAPTURE(name, frame);
        first->actor.update(1, 1.0f / 60, views);
        replay->actor.update(1, 1.0f / 60, views);
        CHECK(replay->digest() == first->digest());
    }
    CHECK(replay->device.texturesCreated == uploads);
}

TEST_CASE("native actor reuse can be rewound without erasing queued events or incarnation",
          "[combatant-local-rewind][assets]") {
    auto fixture = std::make_unique<Fighter>(Golem::definition());
    Combatant::LocalSnapshot baseline;
    REQUIRE(fixture->actor.saveLocal(baseline));
    const u64 incarnation = fixture->actor.incarnation();
    REQUIRE(fixture->actor.spawn(fixture->stock, 40, {4, 0, 8}, 1, nullptr, {}, 'G'));
    CHECK(fixture->actor.incarnation() == incarnation + 1);
    REQUIRE(fixture->actor.canRestoreLocal(baseline));
    REQUIRE(fixture->actor.restoreLocal(baseline));
    Combatant::LocalSnapshot after;
    REQUIRE(fixture->actor.saveLocal(after));
    CHECK(fixture->actor.equalLocal(after, baseline));
}

TEST_CASE("native broken hit-node poses and queued secondary shots survive correction",
          "[combatant-local-rewind][assets]") {
    auto first = std::make_unique<Fighter>(bossDefinition("GARM"));
    auto replay = std::make_unique<Fighter>(bossDefinition("GARM"));
    const auto parts = first->stock.data.parts();
    const auto found = std::ranges::find_if(parts, [](const auto& part) {
        return (part.flags & CritterPart::kBreakable) != 0 && part.healthScale > 0 &&
               part.damageScale > 0;
    });
    REQUIRE(found != parts.end());
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 100000;
    hit.level = 99;
    hit.node = static_cast<s32>(found - parts.begin());
    first->actor.hurt(hit);
    replay->actor.hurt(hit);
    CHECK(replay->digest() == first->digest());
    // Broken, the node is no longer one of its creature's body targets.
    CHECK(std::ranges::none_of(first->actor.bodyTargets(), [&](const auto& target) {
        return target.id == first->actor.id() && target.node == hit.node;
    }));
    Combatant::LocalSnapshot broken;
    REQUIRE(replay->actor.saveLocal(broken));
    // A wrong branch takes what the break queued, then respawns a whole body and moves it.
    replay->actor.takeShots();
    replay->actor.takeCues();
    replay->actor.takeLosses();
    REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
    const auto astray = std::array{visiblePlayer(1, {-6, 0, 4})};
    for (s32 tick = 0; tick < 5; ++tick) {
        replay->actor.update(1, 1.0f / 60, astray);
    }
    CHECK(replay->digest() != first->digest());
    REQUIRE(replay->actor.restoreLocal(broken));
    CHECK(replay->digest() == first->digest());
    const auto players = std::array{visiblePlayer(3, {1, 0, 10})};
    for (s32 tick = 0; tick < 20; ++tick) {
        first->actor.update(1, 1.0f / 60, players);
        replay->actor.update(1, 1.0f / 60, players);
        CHECK(replay->digest() == first->digest());
    }
}

TEST_CASE("combatant arena random state resumes the next native selection after slot reuse",
          "[combatant-local-rewind][assets]") {
    auto first = std::make_unique<Fighter>(Golem::definition());
    auto replay = std::make_unique<Fighter>(Golem::definition());
    auto plain = std::make_unique<Fighter>(Golem::definition());
    const std::array arena{CombatArenaTarget{0, Mat4{1}, false},
                           CombatArenaTarget{1, Mat4{1}, true},
                           CombatArenaTarget{2, Mat4{1}, false}};
    first->actor.setArenaTargets(arena);
    replay->actor.setArenaTargets(arena);
    CHECK(replay->digest() == first->digest());
    Combatant::LocalSnapshot selected;
    REQUIRE(replay->actor.saveLocal(selected));
    // Reused, the slot keeps of its arena only the random state, which a plain spawn never drew.
    REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
    REQUIRE(plain->actor.spawn(plain->stock, 40, {}, 0, nullptr, {}, 'G'));
    REQUIRE(replay->digest() != plain->digest());
    // A wrong branch draws from a shorter roster, reuses the slot and draws again.
    const std::array astray{CombatArenaTarget{4, Mat4{1}, true},
                            CombatArenaTarget{6, Mat4{1}, false}};
    replay->actor.setArenaTargets(astray);
    REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
    replay->actor.setArenaTargets(astray);
    CHECK(replay->digest() != first->digest());
    REQUIRE(replay->actor.restoreLocal(selected));
    CHECK(replay->digest() == first->digest());
    for (s32 count = 0; count < 8; ++count) {
        REQUIRE(first->actor.spawn(first->stock, 40, {}, 0, nullptr, {}, 'G'));
        REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
        first->actor.setArenaTargets(arena);
        replay->actor.setArenaTargets(arena);
        CHECK(replay->digest() == first->digest());
    }
}
} // namespace
