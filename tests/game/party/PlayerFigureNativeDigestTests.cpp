#include <array>
#include <filesystem>
#include <memory>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/BodyGlow.h"
#include "game/effects/WeaponGlow.h"
#include "game/level/NativeStateResources.h"
#include "game/party/MikeyFigure.h"
#include "game/party/PlayerFigure.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digest = NativeStateDigest::Digest;
constexpr f32 kStep = 1.0f / 60;

template <class T> Digest nativeValue(const T& value, const NativeStateResources& resources) {
    NativeStateDigest sink(resources.resources());
    if constexpr (requires { value.appendNativeDigest(sink); }) {
        REQUIRE(value.appendNativeDigest(sink));
    } else {
        sink.value(value);
    }
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

template <class T>
Digest savedValue(const T& value, const typename T::LocalSnapshot& saved,
                  const NativeStateResources& resources) {
    NativeStateDigest sink(resources.resources());
    REQUIRE(value.appendLocalDigest(sink, saved));
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

struct Figure {
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    std::unique_ptr<PlayerFigure> figure;
    NativeStateResources resources;

    explicit Figure(const std::filesystem::path& root) {
        REQUIRE(powerups.load(root / "POWERUPS"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        CharacterSave save;
        save.character = 3;
        save.color = 1;
        save.progress().experience = levelExperience(80);
        figure = PlayerFigure::load(device, root, save, Entrance::Skip);
        REQUIRE(figure);
        REQUIRE(figure->familiarTier() == 2);
        companion(powerup::kPhoenix);
        REQUIRE(figure->phoenixActive());
        refresh();
    }

    void companion(u32 flags, f32 strength = 60) {
        Inventory inventory;
        inventory.addPowerup(powerup::kSpecial, flags, 0, strength);
        inventory.powerups[0].on = true;
        figure->setCompanionPowerups(device, powerups, inventory, &weapons);
    }

    void refresh() {
        const auto created = device.texturesCreated;
        const auto uploaded = device.textureUpdates;
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(resources.registerArchive(powerups, "powerups"));
        REQUIRE(resources.registerArchive(weapons, "weapons"));
        REQUIRE(figure->registerNativeResources(resources));
        CHECK(device.texturesCreated == created);
        CHECK(device.textureUpdates == uploaded);
    }

    Digest digest() const { return nativeValue(*figure, resources); }
    Digest digest(const PlayerFigure::LocalSnapshot& saved) const {
        return savedValue(*figure, saved, resources);
    }
};

TEST_CASE("player native digest includes latent animator and glow controls",
          "[native-state-digest][player-figure-native-digest]") {
    const NativeStateResources resources;
    PlayerAnimator animator;
    const auto initial = animator;
    const auto baseline = nativeValue(animator, resources);
    const auto action = animator.action();
    const auto frame = animator.player().frame();
    animator.setGuardArmor(2);
    CHECK(animator.action() == action);
    CHECK(animator.player().frame() == frame);
    CHECK(nativeValue(animator, resources) != baseline);
    animator = initial;
    animator.setCombo(4, true);
    CHECK(nativeValue(animator, resources) != baseline);
    animator = initial;
    animator.setMelee({MeleeRange::Beyond, true, 0.25f});
    CHECK(nativeValue(animator, resources) != baseline);
    animator = initial;
    animator.setAttackSpeed(true, false);
    CHECK(nativeValue(animator, resources) != baseline);
    animator = initial;
    CHECK(nativeValue(animator, resources) == baseline);

    BodyGlow body;
    body.raise(BodyGlow::kStrike);
    const auto savedBody = body;
    const auto glowing = nativeValue(body, resources);
    body.step(kStep);
    CHECK(nativeValue(body, resources) != glowing);
    body = savedBody;
    CHECK(nativeValue(body, resources) == glowing);

    test::FakeRenderDevice device;
    EffectTrees effects;
    WeaponGlow weapon;
    const auto savedWeapon = weapon;
    const auto unlit = nativeValue(weapon, resources);
    // Even without artwork this selected element affects the next native update.
    weapon.update(device, effects, nullptr, 2, Mat4{1}, Vec3{0}, Vec3{1});
    REQUIRE(weapon.element() == 2);
    REQUIRE(weapon.effect() == 0);
    CHECK(nativeValue(weapon, resources) != unlit);
    weapon = savedWeapon;
    CHECK(nativeValue(weapon, resources) == unlit);
}

TEST_CASE("independent player figures hash native values not render cadence or addresses",
          "[native-state-digest][player-figure-native-digest][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    Figure first(root);
    Figure second(root);
    REQUIRE(first.figure->actionTree() != second.figure->actionTree());
    REQUIRE(first.figure->missileArchive() != second.figure->missileArchive());
    CHECK(first.digest() == second.digest());
    PlayerFigure::LocalSnapshot saved;
    first.figure->saveLocal(saved);
    const auto baseline = first.digest();
    CHECK(first.digest(saved) == baseline);

    const NativeStateResources missing;
    NativeStateDigest unknown(missing.resources());
    CHECK_FALSE(first.figure->appendNativeDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
    NativeStateDigest foreign(second.resources.resources());
    CHECK_FALSE(second.figure->appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());

    // The class missile and earned familiar missile are borrowed by the native
    // projectile store. Their identities must survive independent allocations.
    const auto missileBinding = [](const Figure& fixture) {
        NativeStateDigest sink(fixture.resources.resources());
        sink.resource(&fixture.figure->missile());
        sink.resource(&fixture.figure->familiarMissile());
        const auto result = sink.checkedFinish();
        REQUIRE(result);
        return *result;
    };
    REQUIRE(first.figure->missile().bound());
    REQUIRE(first.figure->familiarMissile().bound());
    CHECK(missileBinding(first) == missileBinding(second));

    const auto camera = CameraFrame::at({10, 20, 30});
    for (s32 tick = 0; tick < 12; ++tick) {
        for (Figure* fixture : {&first, &second}) {
            fixture->figure->capturePresentation();
            fixture->figure->setMelee({MeleeRange::Beyond, false, 0});
            fixture->figure->animate(0.8f, tick % 2, kStep,
                                     tick < 7 ? PlayerDeed::Attack : PlayerDeed::None);
        }
        const auto native = first.digest();
        REQUIRE(native == second.digest());
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            CAPTURE(tick, alpha);
            first.device.draws.clear();
            first.figure->draw(first.device, Mat4{1}, Mat4{1}, {}, 1, false, &camera, alpha);
            REQUIRE_FALSE(first.device.draws.empty());
            CHECK(first.digest() == native);
            CHECK(first.digest(saved) == baseline);
        }
    }
    REQUIRE(first.figure->restoreLocal(saved));
    CHECK(first.digest() == baseline);
}

TEST_CASE("native player figure suffix rewinds retain companion kinds clocks and attachments",
          "[native-state-digest][player-figure-native-digest][local-rewind][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    Figure corrected(root);
    Figure uninterrupted(root);
    std::array<PlayerFigure::LocalSnapshot, 2> initial;
    const std::array<Figure*, 2> fixtures{&corrected, &uninterrupted};
    for (usize index = 0; index < fixtures.size(); ++index) {
        fixtures[index]->figure->setMelee({MeleeRange::Beyond, false, 0});
        fixtures[index]->figure->animate(0, 1, 1.0f / 30, PlayerDeed::Attack);
        fixtures[index]->figure->saveLocal(initial[index]);
    }
    const auto baseline = corrected.digest();
    REQUIRE(baseline == uninterrupted.digest());
    const auto step = [](Figure& fixture, s32 tick) {
        fixture.figure->capturePresentation();
        fixture.companion(tick >= 4 ? powerup::kPojo : powerup::kPhoenix, tick >= 4 ? 0.7f : 60);
        fixture.figure->setAttackSpeed(tick >= 2, false);
        fixture.figure->animate(tick % 3 == 0 ? 0.7f : 0, tick % 2, kStep,
                                tick == 4 ? PlayerDeed::Breathe : PlayerDeed::Attack);
    };
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(corrected.figure->restoreLocal(initial[0]));
        REQUIRE(uninterrupted.figure->restoreLocal(initial[1]));
        for (s32 tick = 0; tick < distance; ++tick) {
            step(uninterrupted, tick);
            corrected.figure->capturePresentation();
            corrected.companion(powerup::kLevitation, 0.3f);
            corrected.figure->setGuardArmor(0.5f);
            corrected.figure->setCombo(4, true);
            corrected.figure->animate(1, 1, 1.0f / 30, PlayerDeed::Flinch);
        }
        corrected.refresh();
        uninterrupted.refresh();
        CHECK(corrected.digest() != uninterrupted.digest());
        CHECK(corrected.digest(initial[0]) == baseline);
        REQUIRE(corrected.figure->restoreLocal(initial[0]));
        CHECK(corrected.digest() == baseline);
        for (s32 tick = 0; tick < distance; ++tick) {
            step(corrected, tick);
        }
        corrected.refresh();
        CHECK(corrected.digest() == uninterrupted.digest());
        CHECK(corrected.figure->handPosition(Mat4{1}) ==
              uninterrupted.figure->handPosition(Mat4{1}));
        CHECK(corrected.figure->familiarReleased() == uninterrupted.figure->familiarReleased());
    }
}

TEST_CASE("companion native history retains dormant pose and rejects expired bindings",
          "[native-state-digest][player-figure-native-digest][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    PowerupCompanion companion;
    companion.choose(device, PowerupCompanion::Kind::Pojo, powerups, nullptr);
    REQUIRE(companion.shown());
    companion.update(kStep, PlayerAnimator::Action::Run1, true, false);
    NativeStateResources resources;
    REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    REQUIRE(resources.registerArchive(powerups, "powerups"));
    REQUIRE(companion.registerNativeResources(resources));
    PowerupCompanion::LocalSnapshot saved;
    companion.saveLocal(saved);
    const auto baseline = nativeValue(companion, resources);
    companion.clear();
    REQUIRE_FALSE(companion.shown());
    const PowerupCompanion neverBound;
    CHECK(nativeValue(companion, resources) != nativeValue(neverBound, resources));
    CHECK(savedValue(companion, saved, resources) == baseline);
    REQUIRE(companion.restoreLocal(saved));
    CHECK(nativeValue(companion, resources) == baseline);
    NativeStateDigest foreign(resources.resources());
    CHECK_FALSE(neverBound.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    // The registry's caller must invalidate before archive teardown; a saved
    // companion must also reject an unloaded lender rather than dereference it.
    resources.clear();
    powerups.clear();
    NativeStateDigest unloaded(resources.resources());
    CHECK_FALSE(companion.appendLocalDigest(unloaded, saved));
    CHECK_FALSE(unloaded.checkedFinish());
}

TEST_CASE("familiar native snapshots reject rebinding and include hidden interpolation state",
          "[native-state-digest][player-figure-native-digest][assets]") {
    const auto path = test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    TextureSet shared;
    REQUIRE(shared.load(path.parent_path().parent_path().parent_path() / "WEAPONS"));
    const std::array<TextureSet*, 1> lenders{&shared};
    PlayerFamiliar familiar;
    REQUIRE(familiar.bind(device, archive, 30, Vec3{1, 5, -1}, lenders));
    NativeStateResources resources;
    REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    REQUIRE(resources.registerArchive(archive, "players/war/sfxyel"));
    REQUIRE(resources.registerTextures(shared, "weapons"));
    familiar.update(kStep, true);
    PlayerFamiliar::LocalSnapshot saved;
    familiar.saveLocal(saved);
    const auto baseline = nativeValue(familiar, resources);
    familiar.capturePresentation();
    CHECK(nativeValue(familiar, resources) != baseline);
    CHECK(savedValue(familiar, saved, resources) == baseline);
    REQUIRE(familiar.restoreLocal(saved));
    CHECK(nativeValue(familiar, resources) == baseline);
    REQUIRE(familiar.bind(device, archive, 80, Vec3{1, 5, -1}, lenders));
    NativeStateDigest stale(resources.resources());
    CHECK_FALSE(familiar.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("Mikey native digest retains the random stream after the visible decoy is gone",
          "[native-state-digest][player-figure-native-digest][assets]") {
    const auto path = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    std::array<Inventory, 2> inventories;
    std::array<MikeyDecoy, 2> controllers;
    std::array<MikeyFigure, 2> figures;
    std::array<EffectTrees, 2> effects;
    for (usize index = 0; index < figures.size(); ++index) {
        inventories[index].addPowerup(powerup::kSpecial, powerup::kMikey, 0, -1);
        inventories[index].powerups[0].on = true;
        controllers[index].update(1.0f / 30, inventories[index], Vec3{2, 3, 4});
        figures[index].update(device, archive, controllers[index], 1.0f / 30, effects[index]);
        REQUIRE(figures[index].shown());
    }
    for (s32 frame = 0; frame < 60; ++frame) {
        for (usize index = 0; index < figures.size(); ++index) {
            controllers[index].update(1.0f / 30, inventories[index], Vec3{50});
            if (index == 1) {
                controllers[index].takeSparkles();
            }
            figures[index].update(device, archive, controllers[index], 1.0f / 30, effects[index]);
        }
    }
    REQUIRE(effects[0].count() > 0);
    REQUIRE(effects[1].count() == 0);
    REQUIRE(controllers[0].generation() == controllers[1].generation());
    figures[0].clear();
    figures[1].clear();
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(archive, "powerups"));
    REQUIRE(figures[0].registerNativeResources(resources));
    REQUIRE(figures[1].registerNativeResources(resources));
    REQUIRE_FALSE(figures[0].shown());
    REQUIRE_FALSE(figures[1].shown());
    CHECK(nativeValue(figures[0], resources) != nativeValue(figures[1], resources));
    const auto saved = figures[0];
    const auto baseline = nativeValue(figures[0], resources);
    figures[0] = figures[1];
    CHECK(nativeValue(figures[0], resources) != baseline);
    figures[0] = saved;
    CHECK(nativeValue(figures[0], resources) == baseline);
}

/** A stance and the breath an item gives, each a still pose of one node. */
TreeInfo breathTree() {
    TreeInfo tree;
    tree.name = "BREATH";
    TreeNodeInfo root;
    root.name = "ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    for (const std::string_view name : {"READY", "ATTBREATHE"}) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 6;
        sequence.frameRate = 30;
        sequence.repeats = name == "READY";
        tree.sequences.push_back(sequence);
    }
    return tree;
}

TEST_CASE("native animator digest bytes stay pinned for a released item",
          "[native-state-digest][player-figure-native-digest][native-digest-pin]") {
    const auto tree = breathTree();
    NativeStateResources resources;
    REQUIRE(resources.registerBuiltinTree(tree, "pinned-breath"));
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, Entrance::Skip));
    animator.update(PlayerMotion::Stand, 1, kStep, PlayerDeed::Breathe);
    REQUIRE(animator.action() == PlayerAnimator::Action::Breathe);
    REQUIRE(animator.itemReleased() == PlayerDeed::Breathe);
    // SHA-256 of the native digest of the animator above, which netplay peers must hash alike: a
    // literal moves only when what is visited, or the number a deed is stored as, changes.
    constexpr std::string_view kDigest =
        "3b0c267f81e724a5e09a4a831511b6598eef479b5d7b0c4cfd41fe97f1a134dc";
    CHECK(test::hexBytes(nativeValue(animator, resources)) == kDigest);
}
} // namespace
