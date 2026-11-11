#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/bosses/BossDefinition.h"
#include "game/critters/Combatant.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/level/CombatantObstacle.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

NativeStateDigest::Digest checked(const NativeStateDigest& value) {
    const auto result = value.checkedFinish();
    REQUIRE(result);
    return *result;
}

struct Encounter {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    Combatant actor;
    NativeStateResources resources;
    std::string resourceKey;

    /** The fighter `name` of the installed game data, in the costume of realm G. */
    explicit Encounter(std::string_view name)
        : Encounter(name, test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path(), 'G') {
    }

    /** The fighter `name` loaded from `root`, in the costume of `realm`. */
    Encounter(std::string_view name, const std::filesystem::path& root, char realm)
        : resourceKey("combatant-digest/" + std::string(name)) {
        const auto family = name == "GENERAL" ? General::definition()
                            : name == "GOLEM" ? Golem::definition()
                                              : bossDefinition(name);
        REQUIRE(stock.load(device, root, family, realm));
        REQUIRE(actor.spawn(stock, 40, {}, 0, nullptr, {}, realm));
        REQUIRE(resources.registerCombatantAssets(stock, resourceKey));
        device.recordDraws = false;
    }

    NativeStateDigest::Digest digest() const {
        NativeStateDigest value(resources.resources());
        REQUIRE(actor.appendNativeDigest(value));
        return checked(value);
    }

    NativeStateDigest::Digest digest(const Combatant::LocalSnapshot& saved) const {
        NativeStateDigest value(resources.resources());
        REQUIRE(actor.appendLocalDigest(value, saved));
        return checked(value);
    }

    void step(s32 frame, bool wrong = false) {
        if (frame % 19 == 4) {
            actor.freeze(wrong ? 2 : 5);
        }
        if (frame % 23 == 7) {
            actor.blind(wrong ? 1 : 4);
        }
        if (frame % 29 == 9) {
            actor.curb(wrong ? 0.25f : 0.5f);
        }
        actor.hold(wrong || frame % 37 < 2);
        actor.setShrink(wrong ? 0.25f : 1.0f);
        actor.damagedPlayer(wrong ? 3 : 1, wrong ? 2.0f : 0.25f);
        std::array<EnemyView, 2> players{
            EnemyView{.player = 0, .position = {2, 0, wrong ? 90.0f : 8.0f}},
            EnemyView{.player = 1, .position = {-3, 0, 12}}};
        actor.update(1, kStep, players);
        if (frame % 11 == 0) {
            (void)actor.takeCues();
            (void)actor.takeBlows();
            (void)actor.takeShots();
        }
    }

    void render() {
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            actor.draw(device, Mat4{1}, WorldLighting{}, nullptr, nullptr, nullptr, alpha);
            actor.drawShadow(device, Mat4{1}, {0, 40, 40}, WorldLighting{}, alpha);
        }
    }
};

TEST_CASE("combatant native digest observes inactive retained state without resources",
          "[native-state-digest][combatant-native-digest]") {
    Combatant actor;
    Combatant::LocalSnapshot initial;
    REQUIRE(actor.saveLocal(initial));
    NativeStateDigest start;
    REQUIRE(actor.appendNativeDigest(start));
    const auto expected = checked(start);
    const auto change = GENERATE(0, 1, 2, 3);
    if (change == 0) {
        actor.takeFullHarm(true);
    } else if (change == 1) {
        actor.tint(Color{41, 52, 63, 255});
    } else if (change == 2) {
        actor.setArenaAnchors(std::array{glm::translate(Mat4{1}, Vec3{4, 0, 8})});
    } else {
        actor.setArenaTargets(
            std::array{CombatArenaTarget{4, Mat4{1}, false}, CombatArenaTarget{9, Mat4{1}, true}});
        // Remove the roster again: the selected target, collection latch and
        // RNG state remain relevant although no actor or target is visible.
        actor.setArenaTargets({});
    }
    CHECK_FALSE(actor.present());
    NativeStateDigest changed;
    REQUIRE(actor.appendNativeDigest(changed));
    CHECK(checked(changed) != expected);
    NativeStateDigest saved;
    REQUIRE(actor.appendLocalDigest(saved, initial));
    CHECK(checked(saved) == expected);
    REQUIRE(actor.restoreLocal(initial));
    NativeStateDigest restored;
    REQUIRE(actor.appendNativeDigest(restored));
    CHECK(checked(restored) == expected);
}

TEST_CASE("combatant native hashes agree across independent art loads and corrected histories",
          "[native-state-digest][combatant-native-digest][assets]") {
    const auto* const family = GENERATE("GENERAL", "GOLEM", "CHIMERA", "LICH");
    CAPTURE(family);
    auto reference = std::make_unique<Encounter>(family);
    auto predicted = std::make_unique<Encounter>(family);
    CHECK(&reference->stock != &predicted->stock);
    CHECK(reference->stock.tree != predicted->stock.tree);
    REQUIRE(reference->digest() == predicted->digest());
    s32 frame = 0;
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        Combatant::LocalSnapshot before;
        REQUIRE(predicted->actor.saveLocal(before));
        const auto beforeHash = predicted->digest();
        REQUIRE(predicted->digest(before) == beforeHash);
        for (s32 i = 0; i < distance; ++i) {
            predicted->step(frame + i, true);
            predicted->render();
        }
        // Saved children must not accidentally read the current parent values.
        CHECK(predicted->digest(before) == beforeHash);
        const auto uploads = predicted->device.texturesCreated;
        const auto textureUpdates = predicted->device.textureUpdates;
        REQUIRE(predicted->actor.restoreLocal(before));
        CHECK(predicted->digest() == beforeHash);
        CHECK(predicted->device.texturesCreated == uploads);
        CHECK(predicted->device.textureUpdates == textureUpdates);
        for (s32 i = 0; i < distance; ++i) {
            reference->step(frame + i);
            predicted->step(frame + i);
            const auto beforeDraw = predicted->digest();
            predicted->render();
            CHECK(predicted->digest() == beforeDraw);
            CHECK(predicted->digest() == reference->digest());
        }
        frame += distance;
    }
}

TEST_CASE("combatant native hashes distinguish hidden timer and move admission state",
          "[native-state-digest][combatant-native-digest][assets]") {
    auto encounter = std::make_unique<Encounter>("GENERAL");
    auto& actor = encounter->actor;
    const auto change = GENERATE(0, 1, 2, 3, 4);
    const auto mutate = [&](bool second) {
        switch (change) {
        case 0: actor.freeze(second ? 9 : 8); break;
        case 1: actor.blind(second ? 9 : 8); break;
        case 2: actor.curb(second ? 0.9f : 0.8f); break;
        case 3: actor.hold(second); break;
        case 4: actor.damagedPlayer(1, second ? 2.0f : 1.0f); break;
        default: break;
        }
    };
    mutate(false);
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto before = encounter->digest();
    const auto position = actor.position();
    const auto health = actor.health();
    const auto move = std::string(actor.moveName());
    mutate(true);
    CHECK(actor.position() == position);
    CHECK(actor.health() == health);
    CHECK(actor.moveName() == move);
    CHECK(encounter->digest() != before);
    CHECK(encounter->digest(saved) == before);
    REQUIRE(actor.restoreLocal(saved));
    CHECK(encounter->digest() == before);
}

TEST_CASE("combatant native digest includes child-only damage memory and pending outputs",
          "[native-state-digest][combatant-native-digest][assets]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA");
    auto& actor = encounter->actor;
    REQUIRE(actor.childCount() == 3);
    Combatant::LocalSnapshot before;
    REQUIRE(actor.saveLocal(before));
    const auto beforeHash = encounter->digest();
    const auto rootHealth = actor.health();
    for (s32 child = 41; child < 44; ++child) {
        REQUIRE(actor.restoreLocal(before));
        actor.damagedPlayer(1, 2, child);
        CHECK(actor.health() == rootHealth);
        CHECK(encounter->digest() != beforeHash);
        CHECK(encounter->digest(before) == beforeHash);
    }
    REQUIRE(actor.restoreLocal(before));
    EnemyHit hit;
    hit.player = 1;
    hit.level = 99;
    hit.damage = 4000000;
    actor.hurt(hit);
    REQUIRE(actor.dying());
    Combatant::LocalSnapshot dying;
    REQUIRE(actor.saveLocal(dying));
    const auto withOutputs = encounter->digest();
    REQUIRE_FALSE(actor.takeLosses().empty());
    CHECK(encounter->digest() != withOutputs);
    REQUIRE(actor.restoreLocal(dying));
    CHECK(encounter->digest() == withOutputs);
    // Same actor storage, different incarnation and zero active children.
    actor.clear();
    CHECK(encounter->digest(dying) == withOutputs);
    REQUIRE(actor.restoreLocal(dying));
    CHECK(encounter->digest() == withOutputs);
}

TEST_CASE("combatant native patrol bindings use owner identity without reading live routes",
          "[native-state-digest][combatant-native-digest][assets]") {
    auto first = std::make_unique<Encounter>("GENERAL");
    auto second = std::make_unique<Encounter>("GENERAL");
    LookoutRoute firstRoute{{Vec3{2, 0, 0}, Vec3{8, 0, 0}}, {1, 0}};
    const LookoutRoute secondRoute = firstRoute;
    REQUIRE(first->resources.registerLookouts(firstRoute, "stage/critters/lookouts"));
    REQUIRE(second->resources.registerLookouts(secondRoute, "stage/critters/lookouts"));
    first->actor.startPatrol(&firstRoute, 10);
    second->actor.startPatrol(&secondRoute, 10);
    REQUIRE(first->actor.patrolling());
    REQUIRE(second->actor.patrolling());
    CHECK(first->digest() == second->digest());
    Combatant::LocalSnapshot saved;
    REQUIRE(first->actor.saveLocal(saved));
    const auto before = first->digest(saved);
    // The route owner's saved contents are visited separately by its aggregate.
    // A patrol snapshot must not read the wrong-branch live owner's contents.
    firstRoute.points[1] = {100, 0, 100};
    CHECK(first->digest(saved) == before);
    first->actor.startPatrol(&firstRoute, 11);
    CHECK(first->digest() != before);
}

TEST_CASE("combatant native replacement models resolve retained semantic bindings",
          "[native-state-digest][combatant-native-digest][assets]") {
    auto first = std::make_unique<Encounter>("GOLEM");
    auto second = std::make_unique<Encounter>("GOLEM");
    const auto node = std::ranges::find_if(first->stock.tree->nodes, [&](const auto& candidate) {
        return !candidate.object.empty() &&
               first->stock.archive.models.find(candidate.object).has_value();
    });
    REQUIRE(node != first->stock.tree->nodes.end());
    REQUIRE(first->actor.replaceNodeModel(first->device, node->name, node->object));
    REQUIRE(second->actor.replaceNodeModel(second->device, node->name, node->object));
    NativeStateDigest notRegistered(first->resources.resources());
    CHECK_FALSE(first->actor.appendNativeDigest(notRegistered));
    CHECK_FALSE(notRegistered.checkedFinish());
    REQUIRE(first->resources.registerCombatantAssets(first->stock, first->resourceKey));
    REQUIRE(second->resources.registerCombatantAssets(second->stock, second->resourceKey));
    REQUIRE(first->digest() == second->digest());
    Combatant::LocalSnapshot saved;
    REQUIRE(first->actor.saveLocal(saved));
    const auto expected = first->digest();
    REQUIRE(first->actor.replaceNodeModel(first->device, node->name, ""));
    CHECK(first->digest() != expected);
    CHECK(first->digest(saved) == expected);
    REQUIRE(first->actor.restoreLocal(saved));
    first->render();
    CHECK(first->digest() == expected);
}

TEST_CASE("combatant native digest refuses missing bindings foreign owners and stale assets",
          "[native-state-digest][combatant-native-digest][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    Combatant::LocalSnapshot saved;
    const Combatant::LocalSnapshot empty{};
    REQUIRE(encounter->actor.saveLocal(saved));
    NativeStateDigest missing;
    CHECK_FALSE(encounter->actor.appendNativeDigest(missing));
    CHECK_FALSE(missing.checkedFinish());
    NativeStateDigest unset(encounter->resources.resources());
    CHECK_FALSE(encounter->actor.appendLocalDigest(unset, empty));
    CHECK_FALSE(unset.checkedFinish());
    const Combatant foreign;
    NativeStateDigest other(encounter->resources.resources());
    CHECK_FALSE(foreign.appendLocalDigest(other, saved));
    CHECK_FALSE(other.checkedFinish());
    std::array obstacles{CombatantObstacle{}};
    encounter->actor.setObstacles(obstacles);
    NativeStateDigest midTick(encounter->resources.resources());
    CHECK_FALSE(encounter->actor.appendNativeDigest(midTick));
    CHECK_FALSE(midTick.checkedFinish());
    encounter->actor.setObstacles({});
    encounter->stock.clear();
    NativeStateDigest staleLive(encounter->resources.resources());
    CHECK_FALSE(encounter->actor.appendNativeDigest(staleLive));
    CHECK_FALSE(staleLive.checkedFinish());
    NativeStateDigest staleSaved(encounter->resources.resources());
    CHECK_FALSE(encounter->actor.appendLocalDigest(staleSaved, saved));
    CHECK_FALSE(staleSaved.checkedFinish());
    encounter->actor.clear();
}

TEST_CASE("combatant native saved digest rejects a destroyed owner at the same address",
          "[native-state-digest][combatant-native-digest]") {
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
    NativeStateDigest stale;
    CHECK_FALSE(actor->appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
    std::destroy_at(actor);
}

TEST_CASE("combatant native digest bytes stay pinned for an inactive configured fighter",
          "[native-state-digest][combatant-native-digest][native-digest-pin]") {
    Combatant actor;
    actor.takeFullHarm(true);
    actor.tint(Color{41, 52, 63, 255});
    actor.setArenaAnchors(std::array{glm::translate(Mat4{1}, Vec3{4, 0, 8})});
    actor.setArenaTargets(
        std::array{CombatArenaTarget{4, Mat4{1}, false},
                   CombatArenaTarget{9, glm::translate(Mat4{1}, Vec3{-6, 0, 2}), true}});
    NativeStateDigest digest;
    REQUIRE(actor.appendNativeDigest(digest));
    // SHA-256 of the native digest of the fighter above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "a35144b1ddade65a57f4a5a0b4ca54fed31c84d60ddf072235645d2a5be840da";
    CHECK(test::hexBytes(checked(digest)) == kDigest);
}

TEST_CASE("combatant native digest bytes stay pinned after the encounter script on a headed body",
          "[native-state-digest][combatant-native-digest][digest-golden]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA", test::headedBodyAssets(), 'A');
    REQUIRE(encounter->actor.childCount() == 2);
    for (s32 frame = 0; frame < 12; ++frame) {
        encounter->step(frame);
    }
    Combatant::LocalSnapshot saved;
    REQUIRE(encounter->actor.saveLocal(saved));
    for (s32 frame = 12; frame < 30; ++frame) {
        encounter->step(frame);
    }
    // SHA-256 of the fighter's native digest thirty frames in, its two heads with it, and of what
    // it saved twelve frames in, which netplay peers must hash alike: a literal moves only when
    // what is visited changes on purpose.
    test::checkPinnedDigests("Combatant", encounter->digest(), encounter->digest(saved),
                             "312339fb87b0e5bb1b8c57483f89f179fa685b5cafc1f6238de454c6ddf5f45c",
                             "a7d9decbb9d7702d9e053615e882f6eb4ccc9697c400b51923b0fb02a034fda3");
}

} // namespace
