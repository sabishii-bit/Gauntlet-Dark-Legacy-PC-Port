#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelTriggers.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("unkeyed trigger targets translate their geometry to signed height endpoints",
          "[game][world][triggers]") {
    const auto dir = test::sampleLevel("trigger-height-endpoints");
    writeTextFile(dir / "world.json", R"({
      "objects": [{"name":"WALL", "position":[0,10,0], "flags":4096}],
      "itemInfos": [{"type":5,"subtype":23,"name":"BRIDGEPAD","radius":1}],
      "itemInstances": [{"info":0,"position":[0,0,0],
        "params":[0,0,0,0,2,255,0,0,0,0,236,255]}]
    })");
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    REQUIRE(layout.load(dir));
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    WorldScene scene;
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(scene.moving(0));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    const std::array visitors{TriggerVisitor{.position = Vec3{0}}};
    triggers.update(0.25f, visitors, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(9));
    CHECK(triggers.takeSettled().empty());
    triggers.update(0.25f, {}, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(8));
    const auto settled = triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    CHECK(settled.front().target == 0);
    triggers.update(1, {}, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(8));
    CHECK(triggers.takeSettled().empty());
}

TEST_CASE("Temple bridge pads are visible and activate their authored world targets",
          "[game][world][triggers][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE/animations.json");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELE1"));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELE"));
    triggers.bindFigures(device, layout, items);
    triggers.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    ModelSet models;
    TextureSet textures;
    REQUIRE(models.load(root / "LEVELS/LEVELE1"));
    REQUIRE(textures.load(root / "LEVELS/LEVELE1"));
    WorldScene scene;
    const std::array<TextureSet*, 1> lenders{&items.textures};
    REQUIRE(scene.build(layout, models, textures, device, {}, lenders));
    bool tested = false;
    for (usize i = 0; i < triggers.size(); ++i) {
        const auto& trigger = triggers.trigger(i);
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        if (info.subtype == 26) {
            CHECK((trigger.flags & 0xFF) == 2);
            REQUIRE(trigger.chained);
            Vec3 activatingSpot = trigger.spot;
            for (usize parent = 0; parent < triggers.size(); ++parent) {
                if (triggers.trigger(parent).next == static_cast<s32>(i)) {
                    activatingSpot = triggers.trigger(parent).spot;
                }
            }
            const std::array visitors{TriggerVisitor{.position = activatingSpot}};
            // E1ELEV99 has no keyed animation: params[10..11] raise it by five units.
            REQUIRE_FALSE(animator.trackOf(trigger.target).has_value());
            const auto target = static_cast<usize>(trigger.target);
            REQUIRE(scene.moving(target));
            const f32 startY = scene.worldTransform(target)[3].y;
            triggers.update(kStep, visitors, animator, scene, nullptr);
            CHECK(trigger.fired);
            CHECK(triggers.opened(trigger.target));
            CHECK(scene.worldTransform(target)[3].y == Approx(startY + 4.0f * kStep));
            triggers.update(2.0f, {}, animator, scene, nullptr);
            CHECK(scene.worldTransform(target)[3].y == Approx(startY + 5.0f));
            tested = true;
        } else if (info.subtype == 23 || info.subtype == 29) {
            CHECK((trigger.flags & 0xFF) == 10);
            if (info.subtype == 29) {
                REQUIRE(animator.trackOf(trigger.target).has_value());
                const std::array visitors{TriggerVisitor{.position = trigger.spot}};
                triggers.update(kStep, visitors, animator, scene, nullptr);
                CHECK(trigger.fired);
                CHECK(triggers.opened(trigger.target));
            }
        }
    }
    REQUIRE(tested);
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldAnimator animator;
    WorldCollision collision;
    LevelTriggers triggers;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
        animator.bind(layout);
        // A wall of the torch's own, so its blocking can be watched.
        CollisionTriangle wall;
        wall.object = 6;
        wall.normal = Vec3{0.0f, 0.0f, -1.0f};
        wall.vertices = {Vec3{0.0f, 0.0f, 30.0f}, Vec3{20.0f, 0.0f, 30.0f},
                         Vec3{20.0f, 5.0f, 30.0f}};
        collision.build({wall});
        triggers.bind(layout, animator, &collision);
    }

    static TriggerVisitor visitor(const Vec3& position, s32 crystals) {
        TriggerVisitor out;
        out.position = position;
        out.crystals[1] = crystals;
        return out;
    }
};

TEST_CASE("triggers name their targets, chain, and hold animated ones at their start",
          "[game][world][triggers]") {
    Fixture f("level-triggers");
    REQUIRE(f.triggers.size() == 3);
    const LevelTrigger& field = f.triggers.trigger(0);
    REQUIRE(field.target == 6);
    REQUIRE(field.id == 1);
    REQUIRE(field.needsCrystals());
    REQUIRE(field.radius == 2.0f);
    REQUIRE((field.kind & LevelTrigger::kFades) != 0);
    REQUIRE(field.next == -1);
    const LevelTrigger& gate = f.triggers.trigger(1);
    REQUIRE(gate.target == 8);
    REQUIRE_FALSE(gate.needsCrystals());
    REQUIRE(gate.nextId == 6);
    REQUIRE(gate.next == 2); // chained to the far pane's trigger
    REQUIRE_FALSE(gate.chained);
    REQUIRE(f.triggers.trigger(2).target == 9);
    REQUIRE(f.triggers.trigger(2).chained);
    REQUIRE(LevelTriggers::crystalsNeeded(1) == 15);
    REQUIRE(LevelTriggers::crystalsNeeded(0) == 0);
    REQUIRE(LevelTriggers::crystalsNeeded(40) == 0);
    // The spinning group waits at its first frame instead of looping.
    REQUIRE(f.animator.held(0));
    f.animator.step(kStep, f.scene);
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == 0.0f);
}

TEST_CASE("a visitor sets off a trigger, opening its chain, once", "[game][world][triggers]") {
    Fixture f("level-triggers-gate");
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 55.0f}, 0)};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(1).fired); // still out of reach
    // Standing on the far pane's own trigger does nothing: chained after the gate's, it is
    // set off through that alone.
    party[0].position = Vec3{10.0f, 0.0f, 100.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(2).fired);
    REQUIRE_FALSE(f.triggers.opened(9));
    party[0].position = Vec3{10.0f, 0.0f, 52.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.trigger(2).fired); // the chain
    REQUIRE(f.triggers.opened(8));
    REQUIRE_FALSE(f.animator.held(0));
    // The gate plays its turn once and stays open.
    for (s32 i = 0; i < 10; ++i) {
        f.animator.step(kStep, f.scene);
    }
    REQUIRE(f.animator.frame(0) == 3.0f);
    REQUIRE(f.animator.finished(0));
    // Run to its end, the animated target is reported settled once; the plain one never is.
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    const std::vector<TriggerOpening> settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    REQUIRE(settled[0].target == 8);
    REQUIRE_FALSE(settled[0].fades);
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeSettled().empty());
}

TEST_CASE("a trigger the party starts inside waits for them to leave it and come back",
          "[game][world][triggers]") {
    Fixture f("level-triggers-gate");
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 52.0f}, 0)};
    f.triggers.openMet(party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(1).occupied);
    for (s32 i = 0; i < 10; ++i) {
        f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    }
    REQUIRE_FALSE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.takeOpenings().empty());
    // Out of it and back in, it goes off as ever.
    party[0].position = Vec3{10.0f, 0.0f, 70.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(1).occupied);
    party[0].position = Vec3{10.0f, 0.0f, 52.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.takeOpenings().size() == 2);
}

TEST_CASE("a field wants the realm's crystals, then fades and stops blocking",
          "[game][world][triggers]") {
    Fixture f("level-triggers-field");
    REQUIRE(f.collision.solid(6));
    // Short of crystals, the spot reaches its own radius only: nothing from 3.5 units off.
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 26.5f}, 3)};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().empty());
    party[0].position = Vec3{10.0f, 0.0f, 29.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(0).fired);
    REQUIRE(f.collision.solid(6));
    REQUIRE(f.scene.objectAlpha(6) == 1.0f);
    // Standing there short of crystals is refused, once, then again after the cooldown.
    std::vector<TriggerRefusal> refusals = f.triggers.takeRefusals();
    REQUIRE(refusals.size() == 1);
    REQUIRE(refusals[0].trigger == 0);
    REQUIRE(refusals[0].id == 1);
    REQUIRE(refusals[0].crystals);
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().empty());
    f.triggers.update(LevelTriggers::kRefusalCooldown, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().size() == 1);
    REQUIRE(f.triggers.takeOpenings().empty());
    // With every member carrying enough, the field goes, and from twice as far.
    party[0].crystals[1] = 15;
    party[0].position = Vec3{10.0f, 0.0f, 26.5f};
    party.push_back(Fixture::visitor(Vec3{50.0f, 0.0f, 50.0f}, 2));
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(0).fired); // the second member has too few
    party[1].crystals[1] = 15;
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(0).fired);
    REQUIRE_FALSE(f.collision.solid(6));
    // Its opening is reported once: the fading field before the party.
    std::vector<TriggerOpening> openings = f.triggers.takeOpenings();
    REQUIRE(openings.size() == 1);
    REQUIRE(openings[0].target == 6);
    REQUIRE(openings[0].fades);
    REQUIRE_FALSE(openings[0].atOnce);
    REQUIRE(openings[0].spot == f.triggers.trigger(0).spot);
    REQUIRE(f.triggers.takeOpenings().empty());
    REQUIRE(f.triggers.takeRefusals().empty());
    REQUIRE(f.scene.objectAlpha(6) == Approx(1.0f - LevelTriggers::kFadeRate));
    REQUIRE(f.triggers.takeSettled().empty()); // still thinning
    for (s32 i = 0; i < 20; ++i) {
        f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    }
    REQUIRE(f.scene.objectAlpha(6) == 0.0f);
    REQUIRE(f.triggers.alphaOf(6) == 0.0f);
    // Gone, the field is reported settled once, with its trigger's sound slot.
    const std::vector<TriggerOpening> settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    REQUIRE(settled[0].target == 6);
    REQUIRE(settled[0].fades);
    REQUIRE(settled[0].sound == 0);
    REQUIRE(f.triggers.takeSettled().empty());
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    for (const auto& draw : f.device.draws) {
        REQUIRE(draw.vertices[0].position != Vec3{10.0f, 0.0f, 30.0f}); // the torch is gone
    }
}

TEST_CASE("what the party already qualifies for opens at once when the level starts",
          "[game][world][triggers]") {
    Fixture f("level-triggers-start");
    const std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{0.0f, 0.0f, 0.0f}, 15)};
    f.triggers.openMet(party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(0).fired);
    REQUIRE(f.triggers.alphaOf(6) == 0.0f);
    REQUIRE_FALSE(f.collision.solid(6));
    REQUIRE_FALSE(f.triggers.trigger(1).fired); // gates want a visitor
    f.triggers.clear();
    REQUIRE(f.triggers.size() == 0);
}

} // namespace
