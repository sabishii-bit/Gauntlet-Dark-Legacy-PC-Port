#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/BodyGlow.h"
#include "game/effects/WeaponGlow.h"
#include "game/level/NativeStateResources.h"
#include "game/party/MikeyFigure.h"
#include "game/party/PlayerFamiliar.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PowerupCompanion.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PlayerDeeds.h"

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

/** Sequences of eight frames at thirty a second, one for each name, over which the first node
 * of their tree rises two units; the stance repeats. */
std::string risingSequences(std::initializer_list<std::string_view> names) {
    std::string list;
    for (const std::string_view name : names) {
        if (!list.empty()) {
            list += ',';
        }
        list += std::format(R"({{"name":"{}","frames":8,"frameRate":30,"repeats":{},
            "tracks":[{{"node":0,"flags":32,"frames":[0,7],"values":[0,2]}}]}})",
                            name, name == "READY");
    }
    return list;
}

/** Writes into `dir` an archive of one triangle, the object `BODY` wearing the bitmap `SKIN`,
 * holding the tree `tree`, of one node over that object, which plays `sequences`. */
void writeBodyArchive(const std::filesystem::path& dir, std::string_view tree,
                      std::initializer_list<std::string_view> sequences) {
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(dir / "animations.json", std::format(R"({{"trees":[{{"name":"{}","nodes":[
        {{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}}],"sequences":[{}]}}]}})",
                                                       tree, risingSequences(sequences)));
    test::convertModelFixture(dir);
}

/** The yellow warrior's costume, the actions it plays, its colour's effects archive with the
 * familiar earned at thirty, its class data, and Pojo among the powerups: synthetic. */
std::filesystem::path figureArchives() {
    const auto root = test::scratchDirectory("player-figure-digest-golden");
    writeBodyArchive(root / "PLAYERS/WAR/YEL", "WAR_YEL", {});
    std::filesystem::create_directories(root / "PLAYERS/WAR/ANIM");
    writeTextFile(root / "PLAYERS/WAR/ANIM/animations.json",
                  std::format(R"({{"trees":[{{"name":"WAR","nodes":[
        {{"name":"BODY","parent":-1,"position":[0,0,0]}}],"sequences":[{}]}}]}})",
                              risingSequences({"READY", "WALK1", "WALK2", "THROW1S", "THROW2S",
                                               "THROW1", "THROW2", "THROW1R", "THROW2R"})));
    writeBodyArchive(root / "PLAYERS/WAR/SFXYEL", "FAMILIAR1", {"READY", "ATTACK"});
    writeBodyArchive(root / "POWERUPS", "POJO",
                     {"READY", "RUN", "ATTACK", "ATTPWR", "HIT", "DEATH"});
    std::filesystem::create_directories(root / "pdata");
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
        "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
        "familiarOffset":[1,5,-1]})");
    return root;
}

TEST_CASE("companion native digest bytes stay pinned after Pojo's script and its save",
          "[native-state-digest][player-figure-native-digest][digest-golden]") {
    const auto root = figureArchives();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    PowerupCompanion companion;
    companion.choose(device, PowerupCompanion::Kind::Pojo, powerups, nullptr);
    REQUIRE(companion.shown());
    // Pojo runs with the body, swings as it strikes at frame 3, and recoils from frame 8.
    const auto step = [&companion](s32 frame) {
        companion.capturePresentation();
        const auto action =
            frame >= 8 ? PlayerAnimator::Action::HitReact : PlayerAnimator::Action::Run1;
        companion.update(1.0f / 30, action, frame == 3, false);
    };
    for (s32 frame = 0; frame < 6; ++frame) {
        step(frame);
    }
    PowerupCompanion::LocalSnapshot saved;
    companion.saveLocal(saved);
    for (s32 frame = 6; frame < 12; ++frame) {
        step(frame);
    }
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(powerups, "powerups"));
    // SHA-256 of the companion's native digest twelve frames in and of what it saved six frames
    // in, which netplay peers must hash alike: a literal moves only when what is visited changes
    // on purpose.
    test::checkPinnedDigests("PowerupCompanion", test::liveDigest(companion, resources),
                             test::savedDigest(companion, saved, resources),
                             "2919c85ee8633113e82165f715e8df2ad5a349e668b3fcf54eebc97ae2b9b315",
                             "7aa37797e5f7e101eaebfc23b1f02a57fe8a6752d3949d067d14b3098828b24b");
}

TEST_CASE("familiar native digest bytes stay pinned after its attack and its save",
          "[native-state-digest][player-figure-native-digest][digest-golden]") {
    const auto root = figureArchives();
    test::FakeRenderDevice device;
    ItemArchive effects;
    REQUIRE(effects.load(root / "PLAYERS/WAR/SFXYEL"));
    PlayerFamiliar familiar;
    REQUIRE(familiar.bind(device, effects, 30, Vec3{1, 5, -1}));
    // It idles, and attacks as its bearer's throw leaves at frame 4.
    const auto step = [&familiar](s32 frame) {
        familiar.capturePresentation();
        familiar.update(1.0f / 30, frame == 4);
    };
    for (s32 frame = 0; frame < 6; ++frame) {
        step(frame);
    }
    PlayerFamiliar::LocalSnapshot saved;
    familiar.saveLocal(saved);
    for (s32 frame = 6; frame < 12; ++frame) {
        step(frame);
    }
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(effects, "players/war/sfxyel"));
    // SHA-256 of the familiar's native digest twelve frames in and of what it saved six frames
    // in, mid-attack, which netplay peers must hash alike: a literal moves only when what is
    // visited changes on purpose.
    test::checkPinnedDigests("PlayerFamiliar", test::liveDigest(familiar, resources.resources()),
                             test::savedDigest(familiar, saved, resources.resources()),
                             "c8c6e89e6ae6cb3606b89f9acd53de9b8dec1f81632cde1b0f09e18eaa22850c",
                             "a60b2c88def61fa7f7c7f3305a6668f6d381b336ee21daab812191a9c6490d96");
}

TEST_CASE("player figure native digest bytes stay pinned after a walk and a throw and its save",
          "[native-state-digest][player-figure-native-digest][digest-golden]") {
    const auto root = figureArchives();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    auto& inventory = save.progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
    const auto figure = PlayerFigure::load(device, root, save, Entrance::Skip);
    REQUIRE(figure);
    REQUIRE(figure->familiarTier() == 1);
    figure->setCompanionPowerups(device, powerups, inventory);
    REQUIRE(figure->pojoActive());
    // The warrior walks for three frames, then throws from the walk while the attack is held
    // from frame 3 to frame 13; its familiar and Pojo answer the release.
    bool familiarShot = false;
    const auto step = [&](s32 frame) {
        figure->capturePresentation();
        const bool attacking = frame >= 3 && frame <= 13;
        figure->animate(frame < 3 ? 0.6f : 0.0f, 2, 1.0f / 30,
                        attacking ? PlayerDeed::Attack : PlayerDeed::None);
        familiarShot |= figure->familiarReleased();
    };
    for (s32 frame = 0; frame < 9; ++frame) {
        step(frame);
    }
    PlayerFigure::LocalSnapshot saved;
    PowerupCompanion::LocalSnapshot companion;
    figure->saveLocal(saved);
    figure->companion().saveLocal(companion);
    for (s32 frame = 9; frame < 18; ++frame) {
        step(frame);
    }
    CHECK(familiarShot);
    NativeStateResources resources;
    REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    REQUIRE(resources.registerArchive(powerups, "powerups"));
    // SHA-256 of the figure's native digest eighteen frames in and of what it saved nine frames
    // in, its familiar and Pojo within it and Pojo beside it, which netplay peers must hash
    // alike: a literal moves only when what is visited changes on purpose.
    test::checkPinnedDigests("PlayerFigure", test::liveDigest(*figure, resources),
                             test::savedDigest(*figure, saved, resources),
                             "e63c0a2d1137eddd35a0e61a2462a4952b180a678725d7b54e7775cda3d4ebd2",
                             "1f2a7e0bdcb0b8f251f47a8638494ea33825bb65a843dff77ac3b82f48c84f57");
    test::checkPinnedDigests("PlayerFigure's PowerupCompanion",
                             test::liveDigest(figure->companion(), resources),
                             test::savedDigest(figure->companion(), companion, resources),
                             "75ac45df1949812e014a0bef0a47d6ae392ed2173433dfaecba141a9dfb6e11c",
                             "508b8a194da2a87985881b60943c2aee58f2542b35a96f174aae226c5f7e1179");
}
} // namespace
