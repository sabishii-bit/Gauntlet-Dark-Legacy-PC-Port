#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/camera/CameraMovementLimit.h"
#include "game/config/GameConfig.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

class ImportedGameplay final : public ImportedLevel {
public:
    s32 resets = 0;
    s32 follows = 0;
    s32 partyUpdates = 0;
    s32 animationUpdates = 0;
    f64 animationSeconds = 0;
    s32 grants = 0;
    bool block = false;
    bool travel = false;
    bool protectedParty = false;
    std::optional<ImportedDeparture> customDeparture;
    bool activate = true;
    usize visitors = 0;
    std::vector<s32> supportingFloors;
    s32 supportUpdates = 0;
    WorldCamera native;
    Vec3 attention{0};
    f32 minimumFov = 0;
    std::vector<CameraSubject> subjects;
    std::vector<ImportedPickup> pickups;
    std::vector<ImportedHazardHit> hazards;
    s32 watches = 0;
    s32 soundServices = 0;
    s32 soundStops = 0;
    s32 soundPauses = 0;
    mutable std::vector<f32> samples;
    mutable std::vector<WorldCamera> drawnCameras;
    ImportedGameplay() {
        native.position = {0, 15, -20};
        native.yaw = kPi / 2;
        native.pitch = .6f;
    }
    void capturePresentation() override {}
    void update(f32 seconds, bool /*timeStopped*/) override {
        ++animationUpdates;
        animationSeconds += seconds;
    }
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& view,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {
        drawnCameras.push_back(view);
    }
    void resetCamera(std::span<const CameraSubject> input) override {
        ++resets;
        subjects.assign(input.begin(), input.end());
    }
    void followCamera(std::span<const CameraSubject> input, f32 /*seconds*/) override {
        ++follows;
        subjects.assign(input.begin(), input.end());
    }
    std::optional<WorldCamera> camera(f32 alpha) const override {
        samples.push_back(alpha);
        auto result = native;
        if (alpha >= 0) {
            result.position.x = alpha * 10;
        }
        return result;
    }
    std::optional<Vec3> cameraAttention() const override { return attention; }
    f32 horizontalFov(f32 preferred) const override { return std::max(preferred, minimumFov); }
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 /*radius*/,
                         f32 /*height*/) const override {
        return block && to.x > from.x ? Vec3{from.x, to.y, to.z} : to;
    }
    void updateParty(std::span<PlayerActor*> actors, f32 /*seconds*/, bool activateItems) override {
        ++partyUpdates;
        activate = activateItems;
        visitors = actors.size();
        if (grants == 0 && !actors.empty()) {
            const ItemOffer offer{.kind = static_cast<s32>(ItemKind::Gold), .amount = 17};
            const auto taking = takeItem(actors.front()->save(), offer);
            pickups.push_back({actors.front()->player(), offer, "Gold", taking});
            ++grants;
        }
    }
    void supportOpponents(std::span<const s32> owners) override {
        supportingFloors.assign(owners.begin(), owners.end());
        ++supportUpdates;
    }
    std::vector<ImportedPickup> takePickups() override { return std::exchange(pickups, {}); }
    std::vector<ImportedHazardHit> takeHazardHits() override { return std::exchange(hazards, {}); }
    void watch(const ViewVolume& /*view*/) override { ++watches; }
    void playSounds(SoundPlayer& /*player*/, const Vec3& /*listener*/) override { ++soundServices; }
    void pauseSounds() override { ++soundPauses; }
    void stopSounds() override { ++soundStops; }
    bool travelRequested() const override { return travel; }
    bool protectsParty() const override { return protectedParty; }
    std::optional<ImportedDeparture> departure(f32 /*alpha*/ = -1) const override {
        return travel ? customDeparture : std::nullopt;
    }
    bool cancelDeparture() override {
        customDeparture.reset();
        return std::exchange(travel, false);
    }
};

TEST_CASE("Imported maps retain ordinary gameplay while supplying native camera and contacts",
          "[legends][imported-play][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    const AssetLocator assets(root);
    GameConfig config;
    config.combat.autoActivateItems = false;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelWorld world;
    auto imported = std::make_shared<ImportedGameplay>();
    LevelRef ref;
    ref.realm = "Legends";
    ref.name = "n64:test";
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
         {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    const std::array<s32, 1> floorOwners{0};
    collision.setMovingObjects(floorOwners, 0x1000);
    auto layout = WorldLayout::fromDecoded(
        {}, {{.kind = LocatorKind::Start, .position = {0, 0, 0}}}, {-100, 0, -100}, {100, 40, 100});
    const bool loaded =
        world.loadImported(device, root, ref, std::move(layout), std::move(collision), imported);
    REQUIRE(loaded);
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.unpackedRoot = root;
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{0};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    CHECK(imported->resets == 1);
    REQUIRE(scene.actor(0));
    REQUIRE(scene.animator(0));
    CHECK(scene.animator(0)->bound());
    CHECK(scene.weaponHeld(0));
    for (s32 tick = 0; tick < 180; ++tick) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.animator(0)->entering());
    CHECK(imported->partyUpdates > 0);
    CHECK(imported->visitors == 1);
    CHECK(imported->watches > 0);
    CHECK_FALSE(imported->activate);
    CHECK(scene.actor(0)->save().gold == 17);
    CHECK(scene.viewCamera().yaw == imported->native.yaw);
    CHECK(scene.viewCamera().position == imported->native.position);

    SECTION("native pressure bridges receive retained ordinary enemy floor owners") {
        REQUIRE(scene.enemies().loadKind(kGruntKind));
        const auto enemy = scene.enemies().spawn(
            EnemySpawn{.kind = kGruntKind, .position = {10, 0, 10}, .placed = true, .asleep = true},
            {});
        REQUIRE(enemy);
        REQUIRE(scene.enemies().floorOf(*enemy) == 0);
        const auto updates = imported->supportUpdates;
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->supportUpdates > updates);
        REQUIRE(imported->supportingFloors == std::vector<s32>{0});
        scene.enemies().close();
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->supportingFloors.empty());
    }

    SECTION("pausing suspends native motion sounds without closing the native scene") {
        const auto stopped = imported->soundStops;
        const auto paused = imported->soundPauses;
        const auto animations = imported->animationUpdates;
        const Vec3 position = scene.actor(0)->position();
        scene.pauseGameplaySounds();
        CHECK(imported->soundPauses == paused + 1);
        CHECK(imported->soundStops == stopped);
        CHECK(imported->animationUpdates == animations);
        CHECK(scene.actor(0)->position() == position);
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->animationUpdates > animations);
    }
    SECTION("confirmed imported suspension preserves the outgoing narration owner") {
        scene.close();
        AudioMixer mixer(48000);
        SoundPlayer sounds(mixer);
        context.sounds = &sounds;
        REQUIRE(scene.open(device, context, world, party, options));
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        REQUIRE(imported->soundServices > 0);
        // The imported owner, not LevelSoundscape, owns this finite phrase.
        // Suspending the old scene must not flush the shared player's voice.
        SoundClip clip;
        clip.sampleRate = 48000;
        clip.channels = 1;
        clip.samples.assign(4096, .25f);
        SoundSequence phrase;
        phrase.steps.push_back({&clip, false, false});
        const auto voice = sounds.play(phrase);
        REQUIRE(voice != kNoSound);
        const auto stopped = imported->soundStops;
        const auto paused = imported->soundPauses;
        const auto serviced = imported->soundServices;
        const auto before = scene.actor(0)->save();
        scene.suspendForImportedLevel(true);
        CHECK(imported->soundStops == stopped);
        CHECK(imported->soundPauses == paused + 1);
        CHECK(sounds.isPlaying(voice));
        std::array<f32, 1024> mixed{};
        mixer.mix(mixed);
        CHECK(mixed.back() == Catch::Approx(.25f));
        scene.resumeAfterImportedLevel();
        CHECK(scene.actor(0)->save() == before);
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->soundServices == serviced + 1);
        scene.close(); // close borrowers before the section's audio objects
    }
    SECTION("failed imported destination restores play and allows a fresh departure") {
        const auto before = scene.actor(0)->save();
        CHECK_FALSE(scene.recoverImportedDeparture());
        imported->travel = true;
        PlayOutcome outcome = PlayOutcome::Running;
        for (s32 tick = 0; tick < 600 && outcome == PlayOutcome::Running; ++tick) {
            outcome = scene.update(1.0 / 30, {});
        }
        REQUIRE(outcome == PlayOutcome::Travel);
        REQUIRE(scene.leaving());
        REQUIRE(scene.transition().covering());
        REQUIRE(scene.recoverImportedDeparture());
        CHECK_FALSE(scene.leaving());
        CHECK_FALSE(imported->travel);
        CHECK(scene.actor(0)->save().gold == before.gold);
        CHECK(scene.actor(0)->save().progress().legends == before.progress().legends);
        for (s32 tick = 0; tick < 60; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        CHECK_FALSE(scene.transition().showing());
        CHECK(scene.canPause(0));
        const auto position = scene.actor(0)->position();
        PlayScene::Inputs move;
        move[0].move = {{0, 1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, move) == PlayOutcome::Running);
        }
        CHECK(glm::distance(scene.actor(0)->position(), position) > .1f);
        imported->travel = true;
        outcome = scene.update(1.0 / 30, {});
        REQUIRE(outcome == PlayOutcome::Running);
        for (s32 tick = 0; tick < 600 && outcome == PlayOutcome::Running; ++tick) {
            outcome = scene.update(1.0 / 30, {});
        }
        CHECK(outcome == PlayOutcome::Travel);
    }
    SECTION("movement and imported body contacts use the native camera yaw") {
        PlayScene::Inputs inputs{};
        inputs[0].move = {{0, 1}, 1};
        const Vec3 initial = scene.actor(0)->position();
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        const Vec3 moved = scene.actor(0)->position();
        CHECK(moved.x > initial.x + .5f);
        CHECK(moved.z == Catch::Approx(initial.z).margin(.001f));
        imported->block = true;
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x == Catch::Approx(moved.x).margin(.001f));
        REQUIRE(imported->subjects.size() == 1);
        CHECK(imported->subjects.front().feet == scene.actor(0)->position());
        REQUIRE(scene.relocate({3, 0, 4}));
        CHECK(imported->resets == 2);
    }
    SECTION("ordinary weapon attacks and HUD render without a preview-only control path") {
        for (s32 tick = 0; tick < 90 && scene.missiles().count() == 0; ++tick) {
            PlayScene::Inputs inputs{};
            inputs[0].attack = true;
            inputs[0].attackPressed = tick == 0;
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        REQUIRE(scene.missiles().count() > 0);
        CHECK(scene.missiles().missile(0).damage > 0);
        CHECK(scene.hudVisible());
        device.draws.clear();
        imported->drawnCameras.clear();
        scene.render(device, makeScreenProjection(640, 448), 640, 448, false, .25f);
        CHECK_FALSE(device.draws.empty());
        REQUIRE_FALSE(imported->drawnCameras.empty());
        // The callback's alpha sample is final, not interpolated a second time.
        CHECK(imported->drawnCameras.front().position.x == 2.5f);
    }
    SECTION("shared view limits recover toward the imported camera's own attention") {
        const std::array pair{PartyMember{0, CharacterSave{}}, PartyMember{1, CharacterSave{}}};
        // Pitch contributes to view depth: put both bodies behind the actual
        // view plane, not merely west of the camera's X coordinate.
        imported->native.position = {50, 15, 0};
        imported->attention = {-100, 0, 0};
        REQUIRE(scene.open(device, context, world, pair, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        // Keep the second body's collision on the blocked side, leaving the
        // recovery direction clear instead of walking into the other player.
        REQUIRE(scene.relocate({0, 0, 0}));
        const Vec3 before = scene.actor(0)->position();
        REQUIRE(glm::dot(before - imported->native.position, imported->native.forward()) < 0);
        REQUIRE(scene.actor(1)->position().x > before.x);
        PlayScene::Inputs inputs{};
        inputs[0].move = {{0, 1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x == Catch::Approx(before.x).margin(.001f));
        inputs[0].move = {{0, -1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.actor(0)->position().x < before.x - .5f);
    }
    SECTION("shared movement uses the imported lens without reading local video settings") {
        const std::array pair{PartyMember{0, CharacterSave{}}, PartyMember{1, CharacterSave{}}};
        imported->native.position = {0, 5, -20};
        imported->native.yaw = 0;
        imported->native.pitch = .2f;
        imported->attention = {0, 0, 0};
        imported->minimumFov = 80 * kPi / 180;
        const auto restart = [&] {
            REQUIRE(scene.open(device, context, world, pair, options));
            for (s32 tick = 0; tick < 180; ++tick) {
                REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
            }
            REQUIRE(scene.relocate({-13, 0, 0}));
        };
        restart();
        const Vec3 before = scene.actor(0)->position();
        const Vec3 offset = scene.actor(0)->followPoint() - before;
        const Vec3 attempted = before + Vec3{-1, 0, 0};
        CameraView projection;
        CHECK(CameraMovementLimit::constrain(before, attempted, imported->attention,
                                             imported->native, projection, offset) != attempted);
        projection.horizontalFov = imported->minimumFov;
        CHECK(CameraMovementLimit::constrain(before, attempted, imported->attention,
                                             imported->native, projection, offset) == attempted);
        PlayScene::Inputs inputs{};
        inputs[0].move = {{-1, 0}, 1};
        const auto walk = [&] {
            for (s32 tick = 0; tick < 5; ++tick) {
                REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
            }
            return scene.actor(0)->position();
        };
        const Vec3 moved = walk();
        CHECK(moved.x < before.x - .5f);
        CHECK(moved.z == Catch::Approx(before.z).margin(.001f));
        config.camera.horizontalFovDegrees = 110;
        // Relocating alone retains the walking action, whose first-step pace
        // differs from starting at rest. Reopen to replay identical motion state.
        restart();
        REQUIRE(scene.actor(0)->position() == before);
        const Vec3 wide = walk();
        CHECK(wide.x == Catch::Approx(moved.x).margin(.001f));
        CHECK(wide.z == Catch::Approx(moved.z).margin(.001f));
    }
    SECTION("native travel waits for the shared departure and covering transition") {
        const s32 before = imported->partyUpdates;
        imported->travel = true;
        CHECK(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.destination().realmId == -1);
        CHECK(scene.destination().name.empty());
        const s32 animationStart = imported->animationUpdates;
        const f64 secondsStart = imported->animationSeconds;
        s32 departureTicks = 0;
        PlayOutcome outcome = PlayOutcome::Running;
        for (s32 tick = 0; tick < 600 && outcome == PlayOutcome::Running; ++tick) {
            outcome = scene.update(1.0 / 30, {});
            ++departureTicks;
            // The departure branch already advances LevelWorld exactly once.
            // Native fire/portal artwork keeps moving, while pickup and portal
            // occupancy gameplay is held until the replacement scene opens.
            CHECK(imported->animationUpdates == animationStart + departureTicks);
            CHECK(imported->partyUpdates == before + 1);
        }
        CHECK(outcome == PlayOutcome::Travel);
        CHECK(departureTicks > 1);
        CHECK(imported->animationSeconds - secondsStart ==
              Catch::Approx(static_cast<f64>(departureTicks) * static_cast<f32>(1.0 / 30)));
        CHECK(imported->partyUpdates == before + 1);
    }
    SECTION("a white native departure retains the visible party without a second black cover") {
        imported->customDeparture = ImportedDeparture{};
        imported->customDeparture->keepVisible = true;
        imported->customDeparture->whiteFade = 0.0f;
        imported->travel = true;
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        REQUIRE(scene.leaving());
        const Vec3 feet = scene.actor(0)->position();
        const auto render = [&](f32 width = 640, f32 height = 448) {
            device.draws.clear();
            scene.render(device, makeScreenProjection(width, height), width, height);
        };
        // A final clip-space cover includes HUD and any aspect-ratio margins.
        const auto whiteCover = [&] {
            if (!device.draws.empty()) {
                const auto& draw = device.draws.back();
                if (draw.texture == &device.whiteTexture() && !draw.vertices.empty() &&
                    draw.transform == Mat4{1} && test::minCorner(draw) == Vec2{-1, -1} &&
                    test::maxCorner(draw) == Vec2{1, 1} && !draw.state.depthTest &&
                    !draw.state.depthWrite) {
                    return std::optional{draw.vertices.front().color};
                }
            }
            return std::optional<Color>{};
        };
        imported->customDeparture->whiteFade = .5f;
        imported->customDeparture->fog = SceneFog{Vec3{1}, -12, 1};
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        render();
        CHECK(std::ranges::any_of(
            device.draws, [](const auto& draw) { return draw.state.fog.reciprocalScale == -12; }));
        CHECK(device.draws.back().state.fog == SceneFog{});
        CHECK(device.sceneFog() == SceneFog{});
        const auto half = whiteCover();
        REQUIRE(half);
        CHECK(half->r == 255);
        CHECK(half->g == 255);
        CHECK(half->b == 255);
        CHECK(half->a == 127);
        const usize withHud = device.draws.size();
        scene.setHudHidden(true);
        render(1920, 1080);
        REQUIRE(whiteCover());
        CHECK(whiteCover()->a == 127);
        CHECK(device.draws.size() < withHud);
        render(600, 1000);
        REQUIRE(whiteCover());
        CHECK(whiteCover()->a == 127);
        scene.setHudHidden(false);
        render();
        CHECK_FALSE(scene.transition().showing());
        const auto bodyVertices = [&] {
            usize count = 0;
            for (const auto& draw : device.draws) {
                if (draw.state.depthTest && draw.state.depthWrite) {
                    count += draw.vertices.size();
                }
            }
            return count;
        };
        const usize before = bodyVertices();
        REQUIRE(before > 0);
        imported->customDeparture->finished = true;
        // A provider cannot release control before its white cover is complete.
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        imported->customDeparture->whiteFade = 1.0f;
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Travel);
        render();
        REQUIRE(whiteCover());
        CHECK(whiteCover()->a == 255);
        const usize retained = bodyVertices();
        REQUIRE(retained > 0);
        imported->customDeparture->keepVisible = false;
        render();
        CHECK(bodyVertices() < retained);
        imported->customDeparture->keepVisible = true;
        CHECK(scene.actor(0)->position() == feet);
        CHECK_FALSE(scene.transition().showing());
        REQUIRE(scene.recoverImportedDeparture());
        CHECK_FALSE(scene.leaving());
        CHECK_FALSE(imported->departure());
        render();
        CHECK_FALSE(whiteCover());
        CHECK(std::ranges::none_of(device.draws, [](const auto& draw) {
            return draw.state.fog.reciprocalScale != 0 || draw.state.fog.bias != 0;
        }));
        CHECK(scene.canPause(0));
    }
    SECTION("native pickup receipts target input seats without granting their rewards again") {
        const std::array thirdSeat{PartyMember{2, CharacterSave{}}};
        REQUIRE(scene.open(device, context, world, thirdSeat, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        auto* save = scene.liveSave(2);
        REQUIRE(save);
        const ItemOffer offer{.kind = static_cast<s32>(ItemKind::Gold), .amount = 23};
        const auto taking = takeItem(*save, offer);
        REQUIRE(taking.took());
        imported->pickups.push_back({2, offer, "Treasure", taking});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.actor(2)->save().gold == 23);
        REQUIRE(scene.hud().pickups().cards().size() == 1);
        CHECK(scene.hud().pickups().cards().front().player == 2);
        CHECK(scene.hud().pickups().cards().front().texture == "GOLD");
        CHECK(imported->pickups.empty());
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.actor(2)->save().gold == 23);
        CHECK(scene.hud().pickups().cards().size() == 1);
    }
    SECTION("imported scroll receipts explicitly distinguish localized keys from prose") {
        bool localized = false;
        SECTION("authored document text is not looked up as a key") {}
        SECTION("milestone localization keys are resolved before opening the parchment") {
            localized = true;
        }
        const std::string key = "menu.player";
        REQUIRE(strings.get(key) != key);
        imported->pickups.push_back({0, {}, {}, {}, {key}, localized});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        REQUIRE(scene.scroll().active());
        REQUIRE(scene.scroll().lines().size() == 1);
        CHECK(scene.scroll().lines().front() == (localized ? strings.get(key) : key));
        CHECK(imported->pickups.empty());
    }
    SECTION("native victory protection does not lock controls or persist after the script") {
        imported->protectedParty = true;
        const auto health = scene.liveSave(0)->health();
        const Vec3 before = scene.actor(0)->position();
        PlayScene::Inputs inputs{};
        inputs[0].move = {{0, 1}, 1};
        for (s32 tick = 0; tick < 10; ++tick) {
            imported->hazards.push_back({0, 30, true, false, {}});
            REQUIRE(scene.update(1.0 / 30, inputs) == PlayOutcome::Running);
        }
        CHECK(scene.liveSave(0)->health() == health);
        CHECK(glm::length(scene.actor(0)->position() - before) > .5f);
        imported->protectedParty = false;
        imported->hazards.push_back({0, 30, true, false, {}});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.liveSave(0)->health() < health);
    }
    SECTION("native hazard contacts use player seats and the common health dispatcher") {
        const std::array thirdSeat{PartyMember{2, CharacterSave{}}};
        REQUIRE(scene.open(device, context, world, thirdSeat, options));
        for (s32 tick = 0; tick < 180; ++tick) {
            REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        }
        const auto before = scene.liveSave(2)->health();
        imported->hazards.push_back({2, 30, true, false, {}});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.liveSave(2)->health() < before);
        CHECK(imported->hazards.empty());
        const auto after = scene.liveSave(2)->health();
        imported->hazards.push_back({0, 30, true, false, {}});
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(scene.liveSave(2)->health() == after);
    }
    SECTION("parking a scene leaves the next scene's effects audible without moving actors") {
        scene.close();
        AudioMixer mixer(48000);
        SoundPlayer sounds(mixer);
        context.sounds = &sounds;
        REQUIRE(scene.open(device, context, world, party, options));
        const CharacterSave before = scene.actor(0)->save();
        const Vec3 position = scene.actor(0)->position();
        const f32 yaw = scene.actor(0)->yaw();
        scene.holdSounds();
        scene.suspendForImportedLevel();
        SoundClip clip;
        clip.sampleRate = 48000;
        clip.channels = 1;
        clip.samples.assign(4096, .25f);
        SoundSequence nextSceneCue;
        nextSceneCue.steps.push_back({&clip, false, false});
        REQUIRE(sounds.play(nextSceneCue) != kNoSound);
        std::array<f32, 1024> mixed{};
        mixer.mix(mixed);
        CHECK(mixed.back() == Catch::Approx(.25f));
        scene.resumeAfterImportedLevel();
        CHECK(scene.actor(0)->save() == before);
        CHECK(scene.actor(0)->position() == position);
        CHECK(scene.actor(0)->yaw() == yaw);
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        CHECK(imported->soundServices > 0);
        const auto stops = imported->soundStops;
        scene.close(); // before this section's SoundPlayer and borrowed clip die
        CHECK(imported->soundStops == stops + 1);
    }
    scene.close();
}
} // namespace
