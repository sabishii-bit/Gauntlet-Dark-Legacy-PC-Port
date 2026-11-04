#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr std::array kBlends{0.0f, 0.37f, 1.0f};

void sameTexture(const Texture* actual, const Texture* expected) {
    REQUIRE((actual != nullptr) == (expected != nullptr));
    if (actual == nullptr) {
        return;
    }
    const auto* left = dynamic_cast<const test::FakeTexture*>(actual);
    const auto* right = dynamic_cast<const test::FakeTexture*>(expected);
    REQUIRE(left);
    REQUIRE(right);
    CHECK(left->width() == right->width());
    CHECK(left->height() == right->height());
    CHECK((left->pixels == right->pixels));
}

void sameDraws(const std::vector<test::RecordedDraw>& actual,
               const std::vector<test::RecordedDraw>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CAPTURE(index);
        const auto& left = actual[index];
        const auto& right = expected[index];
        sameTexture(left.texture, right.texture);
        sameTexture(left.state.lightmap, right.state.lightmap);
        sameTexture(left.state.maskedTexture, right.state.maskedTexture);
        sameTexture(left.state.nextTexture, right.state.nextTexture);
        auto normalized = left.state;
        normalized.lightmap = right.state.lightmap;
        normalized.maskedTexture = right.state.maskedTexture;
        normalized.nextTexture = right.state.nextTexture;
        CHECK(normalized == right.state);
        CHECK(left.transform == right.transform);
        CHECK((left.vertices == right.vertices));
    }
}

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    ItemArchive powerups;
    ItemArchive weapons;
    PlayerRuntime player;
    NativeStateResources resources; ///< what the figure borrows, under keys every load shares
    CameraFrame camera = CameraFrame::at({10, 20, 30});

    Fixture(s32 character, s32 level, bool phoenix) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(powerups.load(root / "POWERUPS"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(resources.registerArchive(powerups, "powerups"));
        REQUIRE(resources.registerArchive(weapons, "weapons"));
        const auto* stats = classes.stats(character);
        REQUIRE(stats);
        CharacterSave save;
        save.name = "TEST";
        save.character = character;
        save.color = 2;
        save.autoAttack = false;
        save.progress().health = 10000;
        save.progress().experience = levelExperience(level);
        if (phoenix) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        }
        player.actor.spawn(3, save, stats, {7, 0, 13}, 0.4f);
        player.entrySave = save;
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        player.figure->setCompanionPowerups(device, powerups, save.progress().inventory, &weapons);
        REQUIRE(player.figure->familiarTier() == PlayerFamiliar::tierFor(level));
        REQUIRE(player.figure->phoenixActive() == phoenix);
    }
    /** Where the participant rewinds to: its simulation values and its figure's. */
    struct Saved {
        PlayerSimulation player;
        PlayerFigure::LocalSnapshot figure;
    };
    Saved save() const {
        Saved saved{player.simulation(), {}};
        player.figure->saveLocal(saved.figure);
        return saved;
    }
    void restore(const Saved& saved) {
        REQUIRE(player.figure->restoreLocal(saved.figure));
        player.simulation() = saved.player;
    }
    /** The figure's native digest, companions and familiar included. */
    NativeStateDigest::Digest digest() { return test::liveDigest(*player.figure, resources); }
    void step(s32 tick, f32 seconds, bool held = false) {
        auto& figure = *player.figure;
        figure.capturePresentation();
        if (!held) {
            const auto& inventory = player.actor.save().progress().inventory;
            figure.setCompanionPowerups(device, powerups, inventory, &weapons);
            figure.animate(0, seconds > 0.02f ? 2 : 1, seconds,
                           tick % 29 < 18 ? PlayerDeed::Attack : PlayerDeed::None);
        }
    }
    std::vector<test::RecordedDraw> draw(f32 blend) {
        device.draws.clear();
        const Mat4 body = PlayerFigure::bodyPlacement(
            player.actor.transform(), player.actor.save(),
            PowerupEffects::of(player.actor.save().progress().inventory));
        player.figure->drawCompanions(device, Mat4{1}, body, {}, 0.8f, &camera, blend,
                                      TreeModel::Pass::All);
        return device.draws;
    }
};

TEST_CASE("bound companion rewind restores native clocks previous poses and held draws",
          "[companion-local-rewind][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const s32 level = GENERATE(30, 80);
    const bool phoenix = GENERATE(false, true);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(character, level, phoenix, seconds);
    auto fixture = std::make_unique<Fixture>(character, level, phoenix);
    const auto uploads = fixture->device.texturesCreated;
    bool observedShot = false;
    bool observedAttack = false;
    bool observedHeld = false;
    for (s32 tick = 0; tick < 85; ++tick) {
        const bool held = tick % 17 == 11;
        fixture->step(tick, seconds, held);
        const auto saved = fixture->save();
        const auto authority = fixture->digest();
        const auto body = fixture->player.actor.transform();
        const auto visuals = fixture->player.figure->companionVisuals(body, 1);
        REQUIRE(visuals[0]);
        REQUIRE(visuals[1].has_value() == phoenix);
        observedShot = observedShot || fixture->player.figure->familiarReleased();
        observedAttack = observedAttack || visuals[0]->sequence != 0;
        observedHeld = observedHeld || held;
        std::array<std::vector<test::RecordedDraw>, kBlends.size()> expected;
        for (usize blend = 0; blend < kBlends.size(); ++blend) {
            expected[blend] = fixture->draw(kBlends[blend]);
            REQUIRE_FALSE(expected[blend].empty());
        }

        // A prediction carries the familiar and Phoenix on past this tick, from a phase of
        // their own; the correction rewinds them to it.
        fixture->step(tick + 4, seconds);
        fixture->step(tick + 5, seconds);
        CHECK(fixture->digest() != authority);
        fixture->restore(saved);
        CHECK(fixture->digest() == authority);
        for (usize blend = 0; blend < kBlends.size(); ++blend) {
            CAPTURE(tick, held, kBlends[blend]);
            sameDraws(fixture->draw(kBlends[blend]), expected[blend]);
        }
    }
    CHECK(observedShot);
    CHECK(observedAttack);
    CHECK(observedHeld);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("companion rewind preserves native fade instead of retaining a future alpha",
          "[companion-local-rewind][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    auto& inventory = fixture->player.actor.save().progress().inventory;
    REQUIRE(inventory.powerups[0].flags == powerup::kPhoenix);
    inventory.powerups[0].strength = 0.6f;
    fixture->step(0, 1.0f / 60);
    const auto baseline = fixture->save();
    const auto authority = fixture->digest();
    const auto visual = fixture->player.figure->companionVisuals(Mat4{1}, 1);
    REQUIRE(visual[1]);
    CHECK(visual[1]->alpha == 0.6f);
    const auto expected = fixture->draw(0.4f);
    inventory.advance(0.4f);
    fixture->step(1, 1.0f / 60);
    const auto changed = fixture->player.figure->companionVisuals(Mat4{1}, 1);
    REQUIRE(changed[1]);
    REQUIRE(changed[1]->alpha != visual[1]->alpha);
    fixture->restore(baseline);
    CHECK(fixture->digest() == authority);
    sameDraws(fixture->draw(0.4f), expected);
}

TEST_CASE("same-binding companion sync follows native warning fade and refuses expiry untouched",
          "[companion-local-rewind][assets]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    auto host = std::make_unique<Fixture>(0, 80, true);
    auto guest = std::make_unique<Fixture>(0, 80, true);
    auto& hostInventory = host->player.actor.save().progress().inventory;
    auto& guestInventory = guest->player.actor.save().progress().inventory;
    hostInventory.powerups[0].strength = 0.95f;
    guestInventory = hostInventory;
    const auto uploads = guest->device.texturesCreated;
    bool faded = false;
    bool expired = false;
    for (s32 tick = 0; tick < 65; ++tick) {
        hostInventory.advance(seconds);
        guestInventory.advance(seconds);
        const auto prior = guest->digest();
        host->player.figure->setCompanionPowerups(host->device, host->powerups, hostInventory,
                                                  &host->weapons);
        if (!host->player.figure->phoenixActive()) {
            CHECK_FALSE(guest->player.figure->syncCompanionPowerups(guestInventory));
            CHECK(guest->digest() == prior);
            expired = true;
            break;
        }
        REQUIRE(guest->player.figure->syncCompanionPowerups(guestInventory));
        host->player.figure->animate(0, seconds > 0.02f ? 2 : 1, seconds);
        guest->player.figure->animate(0, seconds > 0.02f ? 2 : 1, seconds);
        CHECK(guest->digest() == host->digest());
        CHECK(guest->player.simulation() == host->player.simulation());
        const auto visual = host->player.figure->companionVisuals(Mat4{1}, 1);
        REQUIRE(visual[1]);
        faded = faded || visual[1]->alpha < 0.5f;
    }
    CHECK(faded);
    CHECK(expired);
    CHECK(guest->device.texturesCreated == uploads);
}
} // namespace
