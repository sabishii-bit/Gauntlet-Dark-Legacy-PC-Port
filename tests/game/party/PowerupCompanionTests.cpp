#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PowerupCompanion.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Kind = PowerupCompanion::Kind;
using Action = PlayerAnimator::Action;

TEST_CASE("native companion rewind retains interpolation and held frame boundaries",
          "[game][world][companion][powerup-companion-local-rewind][native-local-rewind][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    const auto camera = CameraFrame::at({10, 20, 30});
    constexpr std::array<f32, 3> kBlends{0, 0.5f, 1};
    constexpr f32 kSeconds = 1.0f / 60;
    struct Observation {
        NativeStateDigest::Digest state;
        std::array<std::vector<test::RecordedDraw>, 3> draws;
    };
    // Both archives under fixed keys, which the companion's banks refresh.
    NativeStateResources registry;
    REQUIRE(registry.registerArchive(powerups, "powerups"));
    REQUIRE(registry.registerArchive(weapons, "weapons"));
    for (const Kind kind : {Kind::Pojo, Kind::FireShield, Kind::Phoenix, Kind::FireBreath,
                            Kind::AcidBreath, Kind::ElecBreath, Kind::Wings}) {
        CAPTURE(PowerupCompanion::treeOf(kind));
        PowerupCompanion companion;
        companion.choose(device, kind, powerups, &weapons);
        REQUIRE(companion.shown());
        for (s32 tick = 0; tick < 24; ++tick) {
            companion.capturePresentation();
            companion.update(kSeconds, Action::Run1, false, false);
        }
        PowerupCompanion::LocalSnapshot initial;
        companion.saveLocal(initial);
        const auto observe = [&] {
            Observation result;
            result.state = test::liveDigest(companion, registry);
            for (usize blend = 0; blend < kBlends.size(); ++blend) {
                device.draws.clear();
                companion.draw(device, Mat4{1}, Mat4{1}, {}, 0.8f, &camera, kBlends[blend]);
                REQUIRE_FALSE(device.draws.empty());
                result.draws[blend] = device.draws;
            }
            CHECK(test::liveDigest(companion, registry) == result.state);
            return result;
        };
        const auto check = [&](const Observation& expected) {
            const auto actual = observe();
            CHECK(actual.state == expected.state);
            for (usize blend = 0; blend < kBlends.size(); ++blend) {
                CAPTURE(kBlends[blend]);
                REQUIRE(actual.draws[blend].size() == expected.draws[blend].size());
                for (usize draw = 0; draw < actual.draws[blend].size(); ++draw) {
                    CAPTURE(draw);
                    test::checkRecordedDraw(actual.draws[blend][draw], expected.draws[blend][draw]);
                }
            }
        };
        const auto step = [&](s32 tick) {
            companion.capturePresentation();
            if (tick % 3 != 1) {
                companion.update(kSeconds, tick % 4 == 0 ? Action::Run1 : Action::Ready,
                                 tick % 4 == 2, tick % 4 == 2);
            }
        };
        for (s32 length = 1; length <= 8; ++length) {
            for (const bool held : {false, true}) {
                CAPTURE(length, held);
                REQUIRE(companion.restoreLocal(initial));
                if (held) {
                    companion.capturePresentation();
                }
                PowerupCompanion::LocalSnapshot before;
                companion.saveLocal(before);
                const auto baseline = observe();
                // Holding clears the update's presentation advance, and only holding does.
                CHECK((baseline.state == test::savedDigest(companion, initial, registry)) == !held);

                // Run the uninterrupted branch first, against this same resource owner.
                std::vector<Observation> expected;
                for (s32 tick = 0; tick < length; ++tick) {
                    step(tick);
                    expected.push_back(observe());
                }
                REQUIRE(companion.restoreLocal(before));
                for (s32 tick = 0; tick < length; ++tick) {
                    companion.capturePresentation();
                    companion.update(kSeconds, Action::Death, true, true);
                }
                device.draws.clear();
                const auto uploads = device.texturesCreated;
                const auto updates = device.textureUpdates;
                REQUIRE(companion.restoreLocal(before));
                CHECK(device.draws.empty());
                CHECK(device.texturesCreated == uploads);
                CHECK(device.textureUpdates == updates);
                // Exercise immediate drawing before any corrected update can conceal
                // lost previous-frame fields by overwriting them.
                check(baseline);
                for (s32 tick = 0; tick < length; ++tick) {
                    CAPTURE(tick);
                    step(tick);
                    check(expected[static_cast<usize>(tick)]);
                }
                CHECK(device.texturesCreated == uploads);
                CHECK(device.textureUpdates == updates);
            }
        }
    }
}

TEST_CASE("native companion snapshots reject missing foreign and expired owners",
          "[game][world][companion][powerup-companion-local-rewind][native-local-rewind]") {
    PowerupCompanion owner;
    PowerupCompanion foreign;
    const PowerupCompanion::LocalSnapshot missing;
    PowerupCompanion::LocalSnapshot saved;
    owner.saveLocal(saved);
    CHECK_FALSE(owner.canRestoreLocal(missing));
    CHECK_FALSE(owner.restoreLocal(missing));
    CHECK_FALSE(foreign.canRestoreLocal(saved));
    CHECK_FALSE(foreign.restoreLocal(saved));
    CHECK(owner.canRestoreLocal(saved));
    CHECK(owner.restoreLocal(saved));
    {
        const PowerupCompanion temporary;
        temporary.saveLocal(saved);
        REQUIRE(temporary.canRestoreLocal(saved));
    }
    CHECK_FALSE(owner.canRestoreLocal(saved));
    CHECK_FALSE(owner.restoreLocal(saved));
}

TEST_CASE(
    "native levitation wings follow the animated back without render calls advancing its pose",
    "[game][world][companion][levitation][netplay][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    const auto treeIndex = powerups.trees.find("WINGS");
    REQUIRE(treeIndex);
    const auto& tree = powerups.trees.tree(*treeIndex);
    // WINGS is static feather artwork carried by the player's animated back,
    // not a missing flap sequence or a texture flipbook.
    REQUIRE(tree.sequences.size() == 1);
    CHECK(tree.sequences.front().frames == 0);
    CHECK(tree.sequences.front().tracks.empty());
    CHECK(tree.sequences.front().textureAnimationCount == 0);
    REQUIRE(tree.nodes.size() == 2);
    CHECK(tree.nodes[1].object == "WINGSXN_WINGS");
    CHECK(tree.nodes[1].objectFrames.empty());
    CHECK(tree.nodes[1].textureAnimation == -1);
    const auto meshIndex = powerups.models.find(tree.nodes[1].object);
    REQUIRE(meshIndex);
    const auto& mesh = powerups.models.mesh(*meshIndex);
    REQUIRE(mesh.parts.size() == 1);
    for (const auto& animation : powerups.trees.textureAnimations()) {
        CHECK(animation.texture != static_cast<s32>(mesh.parts.front().texture));
    }

    CharacterSave save;
    save.character = 5;
    save.color = 3;
    auto& inventory = save.progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 60);
    const auto figure = PlayerFigure::load(device, root, save, Entrance::Skip);
    REQUIRE(figure);
    figure->setCompanionPowerups(device, powerups, inventory, &weapons);
    REQUIRE(figure->companion().shown());
    REQUIRE(figure->companion().kind() == Kind::Wings);
    REQUIRE(figure->familiarTier() == 0);
    const Mat4 body = PlayerFigure::bodyPlacement(Mat4{1}, save, PowerupEffects::of(inventory));
    const auto camera = CameraFrame::at({0, 15, 20});
    const auto first = figure->companionVisuals(body, 1)[1];
    REQUIRE(first);
    bool backMoved = false;
    for (s32 tick = 0; tick < 60; ++tick) {
        figure->animate(1, 1, 1.0f / 60);
        const auto visual = figure->companionVisuals(body, 1)[1];
        REQUIRE(visual);
        backMoved = backMoved || visual->placement != first->placement;
        CHECK(visual->sequence == 0);
        CHECK(visual->frame == 0);
        CHECK(visual->generation == first->generation);
        const auto playerFrame = figure->animator().player().frame();
        device.draws.clear();
        figure->drawCompanions(device, Mat4{1}, body, {}, 1, &camera, 1, TreeModel::Pass::All);
        const auto local = device.draws;
        REQUIRE_FALSE(local.empty());
        device.draws.clear();
        figure->drawCompanions(device, Mat4{1}, body, {}, 1, &camera, 1, TreeModel::Pass::All);
        REQUIRE(device.draws.size() == local.size());
        for (usize draw = 0; draw < local.size(); ++draw) {
            REQUIRE(device.draws[draw].vertices.size() == local[draw].vertices.size());
            CHECK(device.draws[draw].texture == local[draw].texture);
            for (usize vertex = 0; vertex < local[draw].vertices.size(); ++vertex) {
                CHECK(device.draws[draw].vertices[vertex].position ==
                      local[draw].vertices[vertex].position);
            }
        }
        CHECK(figure->animator().player().frame() == playerFrame);
        CHECK(figure->companionVisuals(body, 1)[1]->placement == visual->placement);
    }
    CHECK(backMoved);
    const auto held = figure->companionVisuals(body, 1)[1];
    figure->capturePresentation();
    CHECK(figure->companionVisuals(body, 1)[1]->placement == held->placement);
    const Vec3 displacement{3, 5, 7};
    const auto carried =
        figure->companionVisuals(glm::translate(Mat4{1}, displacement) * body, 1)[1];
    REQUIRE(carried);
    CHECK(glm::length(Vec3{carried->placement[3] - held->placement[3]} - displacement) < 0.0001f);
    figure->setCompanionPowerups(device, powerups, Inventory{}, &weapons);
    CHECK_FALSE(figure->companionVisuals(body, 1)[1]);
}

TEST_CASE("successive Pojo shots restart their one-shot without restarting held actions",
          "[pojo][companion][animation][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    PowerupCompanion pojo;
    pojo.choose(device, Kind::Pojo, powerups, nullptr);
    REQUIRE(pojo.shown());
    pojo.update(1.0f / 60, Action::Ready, true, false);
    const auto first = pojo.visual(Mat4{1}, 1);
    REQUIRE(first);
    REQUIRE(first->sequence == PowerupCompanion::kAttack);
    pojo.update(3.0f / 60, Action::Ready, false, false);
    const auto playing = pojo.visual(Mat4{1}, 1);
    REQUIRE(playing);
    CHECK(playing->generation == first->generation);
    CHECK(playing->frame > first->frame);
    pojo.update(1.0f / 60, Action::Ready, true, false);
    const auto second = pojo.visual(Mat4{1}, 1);
    REQUIRE(second);
    CHECK(second->sequence == first->sequence);
    CHECK(second->generation > first->generation);
    CHECK(second->frame == Approx(first->frame));
    pojo.update(1.0f / 60, Action::Death, false, false);
    const auto death = pojo.visual(Mat4{1}, 1);
    REQUIRE(death);
    pojo.update(1.0f / 60, Action::Death, false, false);
    CHECK(pojo.visual(Mat4{1}, 1)->generation == death->generation);
    CHECK(pojo.visual(Mat4{1}, 1)->frame > death->frame);
}

TEST_CASE("fire shield flames retain world depth testing without writing depth",
          "[game][world][companion][fire-shield][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ItemArchive powerups;
    REQUIRE(weapons.load(root));
    PowerupCompanion flame;
    flame.choose(device, Kind::FireShield, powerups, &weapons);
    REQUIRE(flame.shown());
    flame.update(1.0f / 30, Action::ShieldRun, false, false);
    flame.draw(device, Mat4{1}, Mat4{1}, {}, 1, nullptr, -1, TreeModel::Pass::DepthWriting);
    CHECK(device.draws.empty());
    flame.draw(device, Mat4{1}, Mat4{1}, {}, 1, nullptr, -1, TreeModel::Pass::Effects);
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        CHECK(draw.blend() == BlendMode::Alpha);
        CHECK(draw.state.depthTest);
        CHECK_FALSE(draw.state.depthWrite);
    }
}

TEST_CASE("one powerup companion at a time, in the original's order", "[game][world][companion]") {
    PowerupEffects worn;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::None);
    worn.special = powerup::kLevitation;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::Wings);
    worn.special |= powerup::kLightningBreath;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::ElecBreath);
    worn.special |= powerup::kAcidBreath;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::AcidBreath);
    worn.special |= powerup::kFireBreath;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::FireBreath);
    worn.special |= powerup::kPhoenix;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::Phoenix);
    // The fire shield blazes only while its bearer runs with it.
    worn.armor = armor::kFireArmor;
    CHECK(PowerupCompanion::choose(worn, false) == Kind::Phoenix);
    CHECK(PowerupCompanion::choose(worn, true) == Kind::FireShield);
    worn.special |= powerup::kPojo;
    CHECK(PowerupCompanion::choose(worn, true) == Kind::Pojo);

    CHECK(PowerupCompanion::treeOf(Kind::FireShield) == "FW_SHLD_ACTIVE");
    CHECK(PowerupCompanion::fromWeapons(Kind::FireShield));
    CHECK_FALSE(PowerupCompanion::fromWeapons(Kind::Pojo));
    CHECK(PowerupCompanion::treeOf(Kind::AcidBreath) == "HEAD_BREATHEA");
    CHECK(PowerupCompanion::mountOf(Kind::FireBreath) == PowerupCompanion::Mount::Head);
    CHECK(PowerupCompanion::mountOf(Kind::Wings) == PowerupCompanion::Mount::Back);
    CHECK(PowerupCompanion::mountOf(Kind::Pojo) == PowerupCompanion::Mount::Body);
}

TEST_CASE("Pojo acts out what the body does", "[game][world][companion]") {
    CHECK(PowerupCompanion::pojoSequenceOf(Action::Ready) == PowerupCompanion::kReady);
    CHECK(PowerupCompanion::pojoSequenceOf(Action::Idle1) == PowerupCompanion::kReady);
    for (const Action moving : {Action::Shove, Action::Walk1, Action::Walk2, Action::Run1,
                                Action::Run2, Action::ShieldRun, Action::Pushed}) {
        CHECK(PowerupCompanion::pojoSequenceOf(moving) == PowerupCompanion::kRun);
    }
    CHECK(PowerupCompanion::pojoSequenceOf(Action::Breathe) == PowerupCompanion::kPower);
    for (const Action hit : {Action::HitReact, Action::Stun, Action::WebReact, Action::FallBack,
                             Action::FallForward, Action::Whirled, Action::Grabbed}) {
        CHECK(PowerupCompanion::pojoSequenceOf(hit) == PowerupCompanion::kHit);
    }
    CHECK(PowerupCompanion::pojoSequenceOf(Action::Death) == PowerupCompanion::kDeath);
    // Getting up is not a hit (GETUP and GETUP2 are not in the original's list).
    CHECK(PowerupCompanion::pojoSequenceOf(Action::GetUpBack) == PowerupCompanion::kReady);
}

TEST_CASE("a companion fades over the last second of what brings it", "[game][world][companion]") {
    Inventory inventory;
    CHECK(PowerupCompanion::fadeOf(inventory) == 1.0f);
    inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0.0f, 0.25f);
    CHECK(PowerupCompanion::fadeOf(inventory) == Approx(0.25f));
    // The fire shield's time counts too, and the longest wins.
    inventory.addPowerup(powerup::kArmor, armor::kFireArmor, 0.0f, 0.5f);
    CHECK(PowerupCompanion::fadeOf(inventory) == Approx(0.5f));
    inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0.0f, 20.0f);
    CHECK(PowerupCompanion::fadeOf(inventory) == 1.0f);
}

TEST_CASE("the powerup companions come from their archives and play as asked",
          "[game][world][companion][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    for (const Kind kind : {Kind::Pojo, Kind::FireShield, Kind::Phoenix, Kind::FireBreath,
                            Kind::AcidBreath, Kind::ElecBreath, Kind::Wings}) {
        CAPTURE(PowerupCompanion::treeOf(kind));
        PowerupCompanion companion;
        companion.choose(device, kind, powerups, &weapons);
        CHECK(companion.kind() == kind);
        CHECK(companion.shown());
        device.draws.clear();
        companion.update(1.0f / 30.0f, Action::Ready, false, false);
        companion.draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, 1.0f, nullptr);
        CHECK_FALSE(device.draws.empty());
        const usize complete = device.draws.size();
        device.draws.clear();
        companion.draw(device, Mat4{1}, Mat4{1}, {}, 1, nullptr, -1, TreeModel::Pass::DepthWriting);
        for (const auto& draw : device.draws) {
            CHECK(draw.state.depthWrite);
        }
        const usize solid = device.draws.size();
        companion.draw(device, Mat4{1}, Mat4{1}, {}, 1, nullptr, -1, TreeModel::Pass::Effects);
        CHECK(device.draws.size() == complete);
        for (usize i = solid; i < device.draws.size(); ++i) {
            CHECK_FALSE(device.draws[i].state.depthWrite);
        }
    }
    // Without the weapon archive the blaze has nothing to show.
    PowerupCompanion blaze;
    blaze.choose(device, Kind::FireShield, powerups, nullptr);
    CHECK(blaze.kind() == Kind::FireShield);
    CHECK_FALSE(blaze.shown());

    // Pojo runs with the body, swings with it, and plays its hit through before running on.
    PowerupCompanion pojo;
    pojo.choose(device, Kind::Pojo, powerups, &weapons);
    pojo.update(1.0f / 30.0f, Action::Run1, false, false);
    CHECK(pojo.sequence() == PowerupCompanion::kRun);
    pojo.update(1.0f / 30.0f, Action::Run2, true, false);
    CHECK(pojo.sequence() == PowerupCompanion::kAttack);
    pojo.update(1.0f / 30.0f, Action::HitReact, false, false);
    CHECK(pojo.sequence() == PowerupCompanion::kHit);
    pojo.update(1.0f / 30.0f, Action::Run1, false, false);
    CHECK(pojo.sequence() == PowerupCompanion::kHit);
    for (s32 i = 0; i < 30 && pojo.sequence() == PowerupCompanion::kHit; ++i) {
        pojo.update(1.0f / 30.0f, Action::Run1, false, false);
    }
    CHECK(pojo.sequence() == PowerupCompanion::kRun);
    // Dead, the death plays and holds.
    for (s32 i = 0; i < 200; ++i) {
        pojo.update(1.0f / 30.0f, Action::Death, false, false);
    }
    CHECK(pojo.sequence() == PowerupCompanion::kDeath);

    // The phoenix plays its attack as it spits, its body drawn as well as its glow.
    PowerupCompanion phoenix;
    phoenix.choose(device, Kind::Phoenix, powerups, &weapons);
    CameraFrame camera;
    camera.right = {0, 0, -1};
    camera.forward = {-1, 0, 0};
    s32 attacked = 0;
    for (s32 frame = 0; frame < 60; ++frame) {
        device.draws.clear();
        phoenix.update(1.0f / 30.0f, Action::Ready, false, frame == 20);
        attacked += phoenix.sequence() == 1 ? 1 : 0;
        phoenix.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, 1.0f, &camera);
        REQUIRE(device.draws.size() >= 2); // not only the glow with the bird missing
    }
    CHECK(attacked > 0);
    CHECK(phoenix.sequence() == PowerupCompanion::kReady);
    weapons.release();
    powerups.release();
}

} // namespace
