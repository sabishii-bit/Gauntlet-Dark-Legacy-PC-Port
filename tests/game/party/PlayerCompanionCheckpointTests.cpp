#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerRuntimeCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
    std::unique_ptr<PlayerRuntimeCheckpoint> owner;
    CameraFrame camera = CameraFrame::at({10, 20, 30});

    Fixture(s32 character, s32 level, bool phoenix) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(powerups.load(root / "POWERUPS"));
        REQUIRE(weapons.load(root / "WEAPONS"));
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
        owner = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
    }
    PlayerRuntimeCheckpoint::State state() const {
        const auto value = owner->checkpoint();
        REQUIRE(value);
        return *value;
    }
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

TEST_CASE("bound companion correction restores native clocks previous poses and held draws",
          "[player-companion-checkpoint][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const s32 level = GENERATE(30, 80);
    const bool phoenix = GENERATE(false, true);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(character, level, phoenix, seconds);
    auto host = std::make_unique<Fixture>(character, level, phoenix);
    auto guest = std::make_unique<Fixture>(character, level, phoenix);
    const auto hostUploads = host->device.texturesCreated;
    const auto guestUploads = guest->device.texturesCreated;
    bool observedShot = false;
    bool observedAttack = false;
    bool observedHeld = false;
    for (s32 tick = 0; tick < 85; ++tick) {
        const bool held = tick % 17 == 11;
        host->step(tick, seconds, held);
        const auto authority = host->state();
        const auto body = host->player.actor.transform();
        const auto visuals = host->player.figure->companionVisuals(body, 1);
        REQUIRE(visuals[0]);
        REQUIRE(visuals[1].has_value() == phoenix);
        observedShot = observedShot || host->player.figure->familiarReleased();
        observedAttack = observedAttack || visuals[0]->sequence != 0;
        observedHeld = observedHeld || held;

        // Receipt starts at a nonzero phase, then repeatedly rewinds an owner
        // whose familiar and Phoenix have already moved beyond the authority.
        guest->step(tick + 4, seconds);
        guest->step(tick + 5, seconds);
        REQUIRE(guest->owner->restore(authority));
        CHECK(guest->state() == authority);
        for (const f32 blend : {0.0f, 0.37f, 1.0f}) {
            CAPTURE(tick, held, blend);
            const auto expected = host->draw(blend);
            REQUIRE_FALSE(expected.empty());
            sameDraws(guest->draw(blend), expected);
        }
    }
    CHECK(observedShot);
    CHECK(observedAttack);
    CHECK(observedHeld);
    CHECK(host->device.texturesCreated == hostUploads);
    CHECK(guest->device.texturesCreated == guestUploads);
}

TEST_CASE("companion correction preserves native fade instead of retaining a future alpha",
          "[player-companion-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    auto& inventory = fixture->player.actor.save().progress().inventory;
    REQUIRE(inventory.powerups[0].flags == powerup::kPhoenix);
    inventory.powerups[0].strength = 0.6f;
    fixture->step(0, 1.0f / 60);
    const auto baseline = fixture->state();
    const auto visual = fixture->player.figure->companionVisuals(Mat4{1}, 1);
    REQUIRE(visual[1]);
    CHECK(visual[1]->alpha == 0.6f);
    const auto expected = fixture->draw(0.4f);
    inventory.advance(0.4f);
    fixture->step(1, 1.0f / 60);
    const auto changed = fixture->player.figure->companionVisuals(Mat4{1}, 1);
    REQUIRE(changed[1]);
    REQUIRE(changed[1]->alpha != visual[1]->alpha);
    REQUIRE(fixture->owner->restore(baseline));
    CHECK(fixture->state() == baseline);
    sameDraws(fixture->draw(0.4f), expected);
}

TEST_CASE("companion restore refuses a changed trusted tier or binding atomically",
          "[player-companion-checkpoint][assets]") {
    auto source = std::make_unique<Fixture>(0, 80, true);
    auto wrongTier = std::make_unique<Fixture>(0, 30, true);
    auto wrongCompanion = std::make_unique<Fixture>(0, 80, false);
    for (s32 tick = 0; tick < 14; ++tick) {
        source->step(tick, 1.0f / 60);
    }
    const auto state = source->state();
    const auto tierBaseline = wrongTier->state();
    const auto companionBaseline = wrongCompanion->state();
    const auto tierUploads = wrongTier->device.texturesCreated;
    const auto companionUploads = wrongCompanion->device.texturesCreated;
    CHECK_FALSE(wrongTier->owner->prepareRestore(state));
    CHECK_FALSE(wrongCompanion->owner->prepareRestore(state));
    CHECK(wrongTier->state() == tierBaseline);
    CHECK(wrongCompanion->state() == companionBaseline);
    CHECK(wrongTier->device.texturesCreated == tierUploads);
    CHECK(wrongCompanion->device.texturesCreated == companionUploads);
}

TEST_CASE("companion preparations retain single use and reject an intervening rebind",
          "[player-companion-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    fixture->step(0, 1.0f / 60);
    const auto state = fixture->state();
    auto first = fixture->owner->prepareRestore(state);
    auto sibling = fixture->owner->prepareRestore(state);
    REQUIRE(first);
    REQUIRE(sibling);
    REQUIRE(fixture->owner->commitRestore(std::move(*first)));
    CHECK_FALSE(fixture->owner->canCommit(*sibling));
    CHECK_FALSE(fixture->owner->commitRestore(std::move(*sibling)));
    auto prepared = fixture->owner->prepareRestore(state);
    REQUIRE(prepared);
    auto& inventory = fixture->player.actor.save().progress().inventory;
    inventory.powerups[0].on = false;
    fixture->player.figure->setCompanionPowerups(fixture->device, fixture->powerups, inventory,
                                                 &fixture->weapons);
    REQUIRE_FALSE(fixture->player.figure->phoenixActive());
    CHECK_FALSE(fixture->owner->canCommit(*prepared));
    const auto after = fixture->player.figure->companionVisuals(Mat4{1}, 1);
    REQUIRE(after[0]);
    CHECK_FALSE(after[1]);
}

TEST_CASE("same-binding companion sync follows native warning fade and refuses expiry untouched",
          "[player-companion-checkpoint][assets]") {
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
        const auto prior = guest->player.figure->gameplayCheckpoint();
        REQUIRE(prior);
        host->player.figure->setCompanionPowerups(host->device, host->powerups, hostInventory,
                                                  &host->weapons);
        if (!host->player.figure->phoenixActive()) {
            CHECK_FALSE(guest->player.figure->syncCompanionPowerups(guestInventory));
            CHECK(guest->player.figure->gameplayCheckpoint() == prior);
            expired = true;
            break;
        }
        REQUIRE(guest->player.figure->syncCompanionPowerups(guestInventory));
        host->player.figure->animate(0, seconds > 0.02f ? 2 : 1, seconds);
        guest->player.figure->animate(0, seconds > 0.02f ? 2 : 1, seconds);
        CHECK(guest->state() == host->state());
        const auto visual = host->player.figure->companionVisuals(Mat4{1}, 1);
        REQUIRE(visual[1]);
        faded = faded || visual[1]->alpha < 0.5f;
    }
    CHECK(faded);
    CHECK(expired);
    CHECK(guest->device.texturesCreated == uploads);
}

TEST_CASE("companion packet rejects malformed clocks and trusted pose mismatches atomically",
          "[player-companion-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    fixture->step(4, 1.0f / 60);
    const auto baseline = fixture->state();
    REQUIRE(baseline.figure);
    auto changed = baseline;
    changed.figure->companionAlpha = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(changed.valid());
    changed = baseline;
    changed.figure->powerupCompanion.previousGeneration =
        changed.figure->powerupCompanion.animation.generation + 1;
    CHECK_FALSE(changed.valid());
    changed = baseline;
    REQUIRE(changed.figure->familiar.pose);
    REQUIRE_FALSE(changed.figure->familiar.pose->nodes.empty());
    changed.figure->familiar.pose->nodes.front().rest.x += 1;
    REQUIRE(changed.valid());
    CHECK_FALSE(fixture->owner->restore(changed));
    CHECK(fixture->state() == baseline);
}
} // namespace
