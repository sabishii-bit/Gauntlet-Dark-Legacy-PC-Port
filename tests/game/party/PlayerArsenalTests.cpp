#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "fixtures/NativeSoundBank.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/WeaponGlow.h"
#include "game/level/ItemFigure.h"
#include "game/party/PlayerArsenal.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    ItemArchive weapons;
    WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerArsenal arsenal;
    PlayerActor actor;
    Fixture() {
        actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
    }
};

TEST_CASE("weapon preparation samples live inventory without launches or resource effects",
          "[game][world][player-arsenal][weapon-replay]") {
    Fixture f;
    const auto plain = f.arsenal.weaponThrow(f.actor, 0.34f);
    REQUIRE(plain);
    CHECK(plain->owner == f.actor.player());
    CHECK(plain->character == f.actor.save().character);
    CHECK(plain->position == f.actor.position());
    CHECK(plain->center == f.actor.followPoint());
    CHECK(plain->direction == f.actor.facing());
    CHECK(plain->hand == Vec3{0});
    CHECK(plain->speed == PlayerMissiles::kSlowest);
    CHECK(plain->damage == PlayerMissiles::kLeastDamage);
    CHECK(plain->attackSeconds == 0.34f);
    CHECK(plain->shots == 1);
    CHECK(plain->flags == 0);
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 12, 30, true);
    inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot | 4U, 7, 30, true);
    const auto before = inventory.powerups;
    const auto enchanted = f.arsenal.weaponThrow(f.actor, 0.5f);
    REQUIRE(enchanted);
    CHECK(enchanted->hand == Vec3{0, -0.5f, -1.25f});
    CHECK(enchanted->shots == 5);
    CHECK(enchanted->flags == (damage::kFiveWayShot | 4U));
    CHECK(enchanted->attackSeconds == 0.5f);
    CHECK(f.arsenal.missiles().count() == 0);
    CHECK(f.arsenal.missiles().visuals().count() == 0);
    CHECK(f.effects.count() == 0);
    CHECK(inventory.powerups == before);
    f.arsenal.clear();
    CHECK_FALSE(f.arsenal.weaponThrow(f.actor, 0));
}

TEST_CASE("complete weapon preparation is resource-free and keeps single-volley sound flags",
          "[game][world][player-arsenal][weapon-prepare]") {
    Fixture fixture;
    const PlayerFigure figure;
    const u32 flags = GENERATE(0U, damage::kRapidFire | damage::kFiveWayShot | 4U,
                               damage::kThreeWayShot | damage::kSuperShot);
    const bool spreads = GENERATE(false, true);
    const bool aimed = GENERATE(false, true);
    auto& inventory = fixture.actor.save().progress().inventory;
    if (flags != 0) {
        inventory.addPowerup(powerup::kWeapon, flags, 0, 30, true);
    }
    const auto inventoryBefore = inventory.powerups;
    const auto animatorBefore = figure.animator().checkpoint();
    const u32 texturesBefore = fixture.device.texturesCreated;
    const Vec3 direction{0.6f, 0, 0.8f};
    const auto target = aimed ? std::optional<Vec3>{{20, 8, 30}} : std::nullopt;
    const auto prepared =
        fixture.arsenal.prepareWeapon(fixture.actor, figure, direction, 2, spreads, target);
    REQUIRE(prepared);
    CHECK(prepared->soundFlags == flags);
    CHECK_FALSE(prepared->pojo);
    CHECK(prepared->weapon.flags == (flags & ~damage::kSuperShot));
    CHECK(prepared->weapon.direction == direction);
    CHECK(prepared->weapon.scale == 2);
    CHECK(prepared->weapon.target == target);
    const s32 count = spreads ? PowerupEffects::of(inventory).shots() : 1;
    CHECK(prepared->weapon.shots == count);
    REQUIRE(prepared->missiles.size() == static_cast<usize>(count));
    for (const auto& missile : prepared->missiles) {
        CHECK(missile.spec == &MissileSpec::of(fixture.actor.save().character));
        CHECK(missile.scale == 2);
        CHECK(missile.damage == 2 * prepared->weapon.damage);
        CHECK(missile.flags == prepared->weapon.flags);
        CHECK(missile.owner == fixture.actor.player());
        CHECK(missile.velocity.has_value() == aimed);
        CHECK(missile.model == nullptr);
        CHECK(missile.archive == nullptr);
        CHECK(missile.textureLender == nullptr);
        CHECK(missile.riderArchive == nullptr);
        CHECK(missile.streak.texture == nullptr);
        CHECK(missile.tree.empty());
        CHECK(missile.riderTree.empty());
        CHECK_FALSE(missile.release);
    }
    CHECK(inventory.powerups == inventoryBefore);
    CHECK(figure.animator().checkpoint() == animatorBefore);
    CHECK(fixture.device.texturesCreated == texturesBefore);
    CHECK(fixture.arsenal.missiles().count() == 0);
    CHECK(fixture.arsenal.missiles().visuals().count() == 0);
    CHECK(fixture.effects.count() == 0);
    fixture.arsenal.clear();
    CHECK_FALSE(
        fixture.arsenal.prepareWeapon(fixture.actor, figure, direction, 2, spreads, target));
}

TEST_CASE("cursor weapon preparation passes height assistance without changing player facing",
          "[game][player-arsenal][cursor-projectile-aim]") {
    const Fixture fixture;
    const PlayerFigure figure;
    const Vec3 facing{0, 0, 1};
    const Vec3 target{11, 1.5f, 30};
    const auto prepared = fixture.arsenal.prepareWeapon(fixture.actor, figure, facing, 1, true,
                                                        target, std::nullopt, true);
    REQUIRE(prepared);
    REQUIRE(prepared->weapon.fixedFacing);
    REQUIRE(prepared->missiles.size() == 1);
    const auto& shot = prepared->missiles.front();
    REQUIRE(shot.velocity);
    CHECK(shot.direction == facing);
    CHECK(shot.velocity->x == 0);
    CHECK(shot.velocity->z == Approx(shot.speed));
    CHECK(fixture.actor.facing() == facing);
}

TEST_CASE("explicit prepared provenance is canonical and never inferred from a turbo animation",
          "[game][world][player-arsenal][weapon-prepare][assets]") {
    Fixture fixture;
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    REQUIRE(fixture.weapons.load(root / "WEAPONS"));
    fixture.actor.save().character = 1;
    auto figure = PlayerFigure::load(fixture.device, root, fixture.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    figure->animate(0, 1, 1.0f / 60, PlayerDeed::TurboStrong);
    REQUIRE(figure->animator().action() == PlayerAnimator::Action::TurboStrong);
    REQUIRE_FALSE(figure->animator().released());
    REQUIRE_FALSE(figure->animator().strongReleased());
    auto& inventory = fixture.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, damage::kRapidFire | damage::kFiveWayShot, 0, 30);
    const MissileRelease identity{1, figure->animator().player().generation(), false, 0, 0x10000};
    REQUIRE(identity.generation != 0);
    auto requested = identity;
    bool eligible = true;
    bool spreads = false;
    f32 scale = 1;
    std::optional<Vec3> target;
    SECTION("canonical turbo event is single even with five-way equipped") {}
    SECTION("another character cannot be attributed") {
        requested.character = 0;
        eligible = false;
    }
    SECTION("another animation generation cannot be attributed") {
        ++requested.generation;
        eligible = false;
    }
    SECTION("a noncanonical fan member is not a release identity") {
        requested.ordinal = 1;
        eligible = false;
    }
    SECTION("ordinary metadata cannot replace a turbo event identity") {
        requested.emission = 0;
        eligible = false;
    }
    SECTION("a zero encoded row slot is invalid") {
        requested.emission = 0xFFFF;
        eligible = false;
    }
    SECTION("an overflowing encoded row slot is invalid") {
        requested.emission = 0x810000;
        eligible = false;
    }
    SECTION("turbo cannot claim a doubled strong throw") {
        requested.strong = true;
        eligible = false;
    }
    SECTION("a spread physical request is not a native turbo volley") {
        spreads = true;
        eligible = false;
    }
    SECTION("a doubled physical request is not a native turbo volley") {
        scale = 2;
        eligible = false;
    }
    SECTION("an auto-aimed physical request is not a native turbo volley") {
        target = Vec3{20, 8, 30};
        eligible = false;
    }
    SECTION("a reaction does not admit a turbo event identity") {
        figure->animate(0, 1, 1.0f / 60, PlayerDeed::Flinch);
        requested.generation = figure->animator().player().generation();
        eligible = false;
    }
    SECTION("unsupported inventory preserves flight but not replay provenance") {
        inventory.addPowerup(powerup::kWeapon, damage::kReflect, 0, 30);
        eligible = false;
    }
    SECTION("Pojo substitution preserves flight but not replay provenance") {
        inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 30);
        eligible = false;
    }
    const auto position = fixture.actor.position();
    const auto inventoryBefore = inventory.powerups;
    const u32 texturesBefore = fixture.device.texturesCreated;
    const auto prepared = fixture.arsenal.prepareWeapon(
        fixture.actor, *figure, fixture.actor.facing(), scale, spreads, target, requested);
    REQUIRE(prepared);
    REQUIRE(prepared->missiles.size() == (spreads ? 5 : 1));
    for (usize ordinal = 0; ordinal < prepared->missiles.size(); ++ordinal) {
        const auto& launch = prepared->missiles[ordinal];
        CHECK(launch.release.has_value() == eligible);
        if (eligible) {
            auto expected = identity;
            expected.ordinal = static_cast<u8>(ordinal);
            CHECK(launch.release == expected);
        }
    }
    CHECK(fixture.actor.position() == position);
    CHECK(inventory.powerups == inventoryBefore);
    CHECK(fixture.device.texturesCreated == texturesBefore);
    CHECK(fixture.arsenal.missiles().count() == 0);
    const auto untagged =
        fixture.arsenal.prepareWeapon(fixture.actor, *figure, fixture.actor.facing(), 1, false);
    REQUIRE(untagged);
    REQUIRE(untagged->missiles.size() == 1);
    CHECK_FALSE(untagged->missiles.front().release);
    fixture.arsenal.launchWeapon(fixture.actor, figure.get(), fixture.actor.facing(), scale,
                                 spreads, target, requested);
    REQUIRE(fixture.arsenal.missiles().count() == prepared->missiles.size());
    for (usize index = 0; index < prepared->missiles.size(); ++index) {
        CHECK(fixture.arsenal.missiles().missile(index).release ==
              prepared->missiles[index].release);
    }
    fixture.arsenal.clear();
}

TEST_CASE("prepared ordinary and single-volley releases match live native missile physics",
          "[game][world][player-arsenal][weapon-prepare][assets]") {
    Fixture fixture;
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    REQUIRE(fixture.classes.load(root / "PDATA"));
    REQUIRE(fixture.weapons.load(root / "WEAPONS"));
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CharacterSave save;
    save.character = character;
    save.progress().experience = levelExperience(35);
    fixture.actor.spawn(2, save, fixture.classes.stats(character), {10, 3, 20}, 0.4f);
    auto figure = PlayerFigure::load(fixture.device, root, save, Entrance::Skip);
    REQUIRE(figure);
    for (const u32 flags : {0U, damage::kFiveWayShot | damage::kRapidFire}) {
        if (flags != 0) {
            fixture.actor.save().progress().inventory.addPowerup(powerup::kWeapon, flags, 0, 30);
        }
        for (const bool spreads : {false, true}) {
            for (const f32 scale : {1.0f, 2.0f}) {
                for (const bool aimed : {false, true}) {
                    CAPTURE(character, flags, spreads, scale, aimed);
                    const auto target = aimed ? std::optional<Vec3>{{21, 9, 30}} : std::nullopt;
                    const Vec3 direction{0.6f, 0, 0.8f};
                    const auto prepared = fixture.arsenal.prepareWeapon(
                        fixture.actor, *figure, direction, scale, spreads, target);
                    REQUIRE(prepared);
                    fixture.arsenal.missiles().clear();
                    fixture.arsenal.launchWeapon(fixture.actor, figure.get(), direction, scale,
                                                 spreads, target);
                    REQUIRE(fixture.arsenal.missiles().count() == prepared->missiles.size());
                    for (usize index = 0; index < prepared->missiles.size(); ++index) {
                        const auto& expected = prepared->missiles[index];
                        const auto& actual = fixture.arsenal.missiles().missile(index);
                        CHECK(actual.position == expected.position);
                        CHECK(actual.velocity ==
                              expected.velocity.value_or(PlayerMissiles::launchVelocity(
                                  expected.direction, expected.speed, expected.reach,
                                  expected.spec->weight)));
                        CHECK(actual.spec == expected.spec);
                        CHECK(actual.damage == expected.damage);
                        CHECK(actual.scale == expected.scale);
                        CHECK(actual.flags == expected.flags);
                        CHECK(actual.wallSound == expected.wallSound);
                        CHECK(actual.release == expected.release);
                    }
                }
            }
        }
    }
    fixture.arsenal.clear();
}

TEST_CASE("plain replay preparation matches the live arsenal for every class and strength",
          "[game][world][player-arsenal][weapon-replay][assets]") {
    Fixture f;
    const auto directory = test::assetOrSkip("PDATA");
    REQUIRE(f.classes.load(directory));
    PlayerFigure figure;
    for (s32 character = 0; character <= kSumnerClass; ++character) {
        for (const s32 level : {1, 50, 99}) {
            for (const f32 scale : {1.0f, 2.0f}) {
                CAPTURE(character, level, scale);
                CharacterSave save;
                save.character = character;
                save.progress().experience = levelExperience(level);
                const auto* stats = f.classes.stats(character);
                f.actor.spawn(2, save, stats, {10, 3, 20}, 0.4f);
                auto prepared = f.arsenal.weaponThrow(f.actor, 0);
                REQUIRE(prepared);
                s32 stat = 0;
                Vec3 hand{0};
                if (stats != nullptr) {
                    // The master's stats are all 999 whatever record he plays on.
                    const auto block = characterStats(*stats, save);
                    stat = MissileSpec::byMagic(character) ? block.magic() : block.strength();
                    hand = stats->weaponOffset;
                }
                CHECK(prepared->speed == PlayerMissiles::speedFor(stat));
                CHECK(prepared->damage == PlayerMissiles::damageFor(stat));
                CHECK(prepared->hand == hand);
                prepared->scale = scale;
                prepared->target = Vec3{15, 7, 40};
                PlayerMissiles replay;
                REQUIRE(replay.launch(PlayerMissiles::weaponLaunches(*prepared).front()));
                f.arsenal.launchWeapon(f.actor, &figure, f.actor.facing(), scale, false,
                                       prepared->target);
                REQUIRE(f.arsenal.missiles().count() == 1);
                const auto& live = f.arsenal.missiles().missile(0);
                CHECK(PlayerMissiles::flightCheckpoint(live) ==
                      PlayerMissiles::flightCheckpoint(replay.missile(0)));
                CHECK(live.damage == replay.missile(0).damage);
                CHECK(live.spec == replay.missile(0).spec);
                for (s32 tick = 0; tick < 15; ++tick) {
                    replay.update(1.0f / 60, nullptr);
                    f.arsenal.missiles().update(1.0f / 60, nullptr);
                    CHECK(PlayerMissiles::flightCheckpoint(f.arsenal.missiles().missile(0)) ==
                          PlayerMissiles::flightCheckpoint(replay.missile(0)));
                }
                f.arsenal.missiles().clear();
            }
        }
    }
}

TEST_CASE("plain weapon clock queries use the native visual bank without starting it",
          "[game][world][player-arsenal][weapon-replay][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto character = GENERATE(1, 2, 6);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.actor.save().character = character;
    auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    CHECK_FALSE(f.arsenal.weaponTextureFrame(*figure));
    f.arsenal.missiles().update(0.75f, nullptr);
    CHECK_FALSE(f.arsenal.weaponTextureFrame(*figure));
    CHECK(f.arsenal.missiles().visuals().count() == 0);
    f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), 1, false);
    REQUIRE(f.arsenal.missiles().visuals().count() == 1);
    REQUIRE(f.arsenal.weaponTextureFrame(*figure));
    CHECK(*f.arsenal.weaponTextureFrame(*figure) == 0);
    f.arsenal.missiles().update(0.125f, nullptr);
    CHECK(*f.arsenal.weaponTextureFrame(*figure) == Approx(3.75f));
    const auto& effects = f.arsenal.missiles().visuals();
    CHECK(*f.arsenal.weaponTextureFrame(*figure) == effects.textureFrame(effects.effect(0)));
    f.arsenal.clear(); // The projectile pool borrows the figure's archive.
    CHECK_FALSE(f.arsenal.weaponTextureFrame(*figure));
}

TEST_CASE("plain weapon birth retains release provenance after the player is interrupted",
          "[game][world][player-arsenal][weapon-replay][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const bool strong = GENERATE(false, true);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.actor.save().character = 1;
    auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    // An idle/scripted launch has no action release to acknowledge.
    f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), 1, false);
    REQUIRE(f.arsenal.missiles().count() == 1);
    CHECK_FALSE(f.arsenal.missiles().missile(0).release);
    for (s32 tick = 0;
         tick < 180 && !figure->animator().released() && !figure->animator().strongReleased();
         ++tick) {
        figure->animate(0, 1, 1.0f / 60, strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack);
    }
    REQUIRE((figure->animator().released() || figure->animator().strongReleased()));
    const MissileRelease expected{1, figure->animator().player().generation(), strong};
    bool eligible = true;
    auto multiplayer = MultiplayerMode::Normal;
    auto& inventory = f.actor.save().progress().inventory;
    SECTION("ordinary or strong native throw") {}
    SECTION("amulet throws retain native release provenance") {
        inventory.addPowerup(powerup::kWeapon, 1, 0, 30);
    }
    SECTION("rapid and spread throws keep native fan provenance") {
        const u32 flags =
            GENERATE(damage::kRapidFire, damage::kThreeWayShot, damage::kFiveWayShot,
                     damage::kRapidFire | damage::kThreeWayShot, PlayerMissiles::kPlainModifiers);
        inventory.addPowerup(powerup::kWeapon, flags, 0, 30);
    }
    SECTION("spread plus an unsupported weapon remains authoritative") {
        inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot | damage::kReflect, 0, 30);
        eligible = false;
    }
    SECTION("Pojo substitutions remain authoritative") {
        inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 30);
        eligible = false;
    }
    SECTION("competitive throws remain authoritative") {
        multiplayer = MultiplayerMode::Stun;
        eligible = false;
    }
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    nullptr,
                    {},
                    false,
                    false,
                    nullptr,
                    &multiplayer});
    f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), strong ? 2.0f : 1.0f, true);
    REQUIRE(f.arsenal.missiles().count() > 0);
    const auto before = f.arsenal.missiles().missile(0).release;
    CHECK(before.has_value() == eligible);
    if (eligible) {
        CHECK(*before == expected);
    }
    const auto prepared = f.arsenal.weaponThrow(f.actor, figure->animator().attackSeconds());
    REQUIRE(prepared);
    CHECK(f.arsenal.missiles().count() == static_cast<usize>(prepared->shots));
    const auto launches = PlayerMissiles::weaponLaunches(*prepared);
    for (usize ordinal = 0; ordinal < f.arsenal.missiles().count(); ++ordinal) {
        const auto& shot = f.arsenal.missiles().missile(ordinal);
        CHECK(shot.flags == (prepared->flags | (strong ? PlayerMissiles::kStrongThrowFlags : 0U)));
        CHECK(shot.wallSound == launches[ordinal].wallSound);
        CHECK(shot.release.has_value() == eligible);
        if (eligible) {
            auto identity = expected;
            identity.ordinal = static_cast<u8>(ordinal);
            CHECK(*shot.release == identity);
        }
    }
    figure->animate(0, 1, 1.0f / 60, PlayerDeed::Flinch);
    figure->animate(0, 1, 1.0f / 60, PlayerDeed::None);
    CHECK_FALSE(figure->animator().released());
    CHECK_FALSE(figure->animator().strongReleased());
    CHECK(f.arsenal.missiles().missile(0).release == before);
    f.arsenal.missiles().update(1.0f / 60, nullptr);
    CHECK(f.arsenal.missiles().missile(0).release == before);
    f.arsenal.clear();
}

TEST_CASE("native weapon sound capture reports the played cue once per released fan",
          "[game][world][player-arsenal][weapon-sound][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto flags = GENERATE(0U, 1U, 2U, 3U, 4U, damage::kRapidFire, damage::kThreeWayShot,
                                damage::kRapidFire | damage::kFiveWayShot | 4U);
    const bool strong = GENERATE(false, true);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds, {}});
    f.actor.save().character = 1;
    auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    if (flags != 0) {
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, flags, 0, 30);
    }
    for (s32 tick = 0;
         tick < 180 && !figure->animator().released() && !figure->animator().strongReleased();
         ++tick) {
        figure->animate(0, 1, 1.0f / 60, strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack);
    }
    REQUIRE((figure->animator().released() || figure->animator().strongReleased()));
    const MissileRelease release{1, figure->animator().player().generation(), strong, 0};
    SoundSet common;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    constexpr std::array<std::string_view, 5> kThrowCues{"", "S_AMULETFIRE", "S_AMULETLIGHTNI",
                                                         "S_AMULETLIGHT", "S_AMULETACID"};
    const bool fan = (flags & (damage::kThreeWayShot | damage::kFiveWayShot)) != 0;
    SoundSet& expectedBank = fan || (flags & 0xFU) != 0 ? common : figure->voice();
    const auto name = fan                   ? std::string{"S_SUPERSHOT"}
                      : (flags & 0xFU) != 0 ? std::string{kThrowCues[flags & 0xFU]}
                                            : std::format("S_{}THROW", classCode(1));
    const auto expectedCall = expectedBank.find(name);
    REQUIRE(expectedCall);
    const auto expectedSequence = expectedBank.sequence(*expectedCall);
    std::vector<PlayerArsenal::WeaponSound> outer;
    std::vector<PlayerArsenal::WeaponSound> inner;
    std::vector<SoundPlayer::Event> played;
    const SoundPlayer::Capture audioCapture(&sounds, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            played.push_back(event);
        }
    });
    const auto launch = [&] {
        f.arsenal.missiles().clear();
        f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), strong ? 2.0f : 1.0f, true);
        const auto worn = PowerupEffects::of(f.actor.save().progress().inventory);
        CHECK(f.arsenal.missiles().count() == static_cast<usize>(worn.shots()));
    };
    const auto check = [&](const PlayerArsenal::WeaponSound& observed) {
        REQUIRE_FALSE(played.empty());
        CHECK(observed.owner == f.actor.player());
        CHECK(observed.release == release);
        CHECK(observed.handle != kNoSound);
        CHECK(observed.handle == played.back().handle);
        CHECK(played.back().bank == expectedSequence.bank);
        CHECK(played.back().call == *expectedCall);
        CHECK(sounds.isPlaying(observed.handle));
    };
    {
        const PlayerArsenal::SoundCapture capture(
            &f.arsenal, [&](const auto& event) { outer.push_back(event); });
        launch();
        REQUIRE(outer.size() == 1);
        REQUIRE(played.size() == 1);
        check(outer.back());
        {
            const PlayerArsenal::SoundCapture nested(
                &f.arsenal, [&](const auto& event) { inner.push_back(event); });
            launch();
            REQUIRE(inner.size() == 1);
            CHECK(outer.size() == 1);
            REQUIRE(played.size() == 2);
            check(inner.back());
        }
        launch();
        REQUIRE(outer.size() == 2);
        REQUIRE(played.size() == 3);
        check(outer.back());
    }
    launch(); // Leaving the capture scope changes no offline sound dispatch.
    CHECK(outer.size() == 2);
    CHECK(inner.size() == 1);
    CHECK(played.size() == 4);
    {
        const PlayerArsenal::SoundCapture absent(
            nullptr, [&](const auto& event) { outer.push_back(event); });
        launch();
    }
    CHECK(outer.size() == 2);
    CHECK(played.size() == 5);
    f.arsenal.clear();
    f.audio.close();
    sounds.stopAll();
}

TEST_CASE("native weapon sound capture excludes scripted unsupported and blocked releases",
          "[game][world][player-arsenal][weapon-sound][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.actor.save().character = 1;
    auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    bool scripted = false;
    bool output = true;
    bool blocked = false;
    auto multiplayer = MultiplayerMode::Normal;
    auto& inventory = f.actor.save().progress().inventory;
    SECTION("scripted launch without a native release latch") {
        scripted = true;
    }
    SECTION("reflective rider remains authoritative") {
        inventory.addPowerup(powerup::kWeapon, damage::kReflect, 0, 30);
    }
    SECTION("Pojo substitution remains authoritative") {
        inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 30);
    }
    SECTION("competitive projectile remains authoritative") {
        multiplayer = MultiplayerMode::Stun;
    }
    SECTION("a silent native launch has no handle to observe") {
        output = false;
    }
    SECTION("an obstructed muzzle only dispatches its impact") {
        blocked = true;
    }
    if (!scripted) {
        for (s32 tick = 0; tick < 180 && !figure->animator().released(); ++tick) {
            figure->animate(0, 1, 1.0f / 60, PlayerDeed::Attack);
        }
        REQUIRE(figure->animator().released());
    }
    f.audio.open(root, output ? &sounds : nullptr, nullptr);
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    output ? &sounds : nullptr,
                    {},
                    false,
                    false,
                    nullptr,
                    &multiplayer});
    if (blocked) {
        const auto throwing = f.arsenal.weaponThrow(f.actor, figure->animator().attackSeconds());
        REQUIRE(throwing);
        const auto muzzle = PlayerMissiles::weaponLaunches(*throwing).front().position;
        const f32 z = muzzle.z + 0.1f;
        const std::vector walls{
            CollisionTriangle{
                {0, 0, -1}, {Vec3{-100, -100, z}, Vec3{100, 100, z}, Vec3{100, -100, z}}, 0},
            CollisionTriangle{
                {0, 0, -1}, {Vec3{-100, -100, z}, Vec3{-100, 100, z}, Vec3{100, 100, z}}, 0}};
        f.collision.build(walls);
    }
    std::vector<PlayerArsenal::WeaponSound> observed;
    {
        const PlayerArsenal::SoundCapture capture(
            &f.arsenal, [&](const auto& event) { observed.push_back(event); });
        f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), 1, true);
    }
    CHECK(observed.empty());
    CHECK(f.arsenal.missiles().count() == (blocked ? 0 : 1));
    f.arsenal.clear();
    f.audio.close();
    sounds.stopAll();
}

usize visibleFlameTriangles(const test::FakeRenderDevice& device, const Texture* texture) {
    REQUIRE(texture != nullptr);
    REQUIRE(texture != &device.whiteTexture());
    usize visible = 0;
    for (const auto& draw : device.draws) {
        if (draw.texture != texture) {
            continue;
        }
        CHECK(draw.state.blend == BlendMode::Additive);
        for (usize vertex = 0; vertex + 2 < draw.vertices.size(); vertex += 3) {
            const auto& a = draw.vertices[vertex];
            const auto& b = draw.vertices[vertex + 1];
            const auto& c = draw.vertices[vertex + 2];
            if (a.color.a > 0 &&
                glm::length(glm::cross(b.position - a.position, c.position - a.position)) > 0) {
                ++visible;
            }
        }
    }
    return visible;
}

TEST_CASE("placed red potion bottles submit their authored neck flame",
          "[game][player-arsenal][alpha-potion-flames][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    const auto tree = powerups.trees.find("POT_RED");
    REQUIRE(tree);
    CHECK(std::ranges::count_if(powerups.trees.tree(*tree).nodes,
                                [](const auto& node) { return node.particle >= 0; }) == 1);
    const auto slot = powerups.textures.find("DRAGONBREATH");
    REQUIRE(slot);
    const auto* texture = &powerups.textures.texture(device, *slot);
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        ItemFigure bottle;
        ItemInstance instance;
        instance.position = {10, 0, 20};
        REQUIRE(bottle.place(device, powerups, "POT_RED", instance, nullptr));
        for (s32 tick = 0; tick < hz; ++tick) {
            bottle.update(1.0f / static_cast<f32>(hz));
        }
        device.draws.clear();
        bottle.draw(device, Mat4{1}, {}, 1, 1);
        CHECK(visibleFlameTriangles(device, texture) > 0);
    }
}

TEST_CASE("thrown red potions submit their authored three-emitter flame trail",
          "[game][player-arsenal][alpha-potion-flames][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        // clear() releases the borrowed renderer too, so each run needs a fresh binding.
        f.arsenal.bind(
            {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
        f.actor.save().progress().inventory.addPotions(1, 1);
        f.arsenal.throwPotion(f.actor, 30);
        REQUIRE(f.arsenal.missiles().count() == 1);
        REQUIRE(f.arsenal.missiles().visuals().count() == 1);
        const auto& effect = f.arsenal.missiles().visuals().effect(0);
        REQUIRE(effect.name == "POT_RED_TW");
        const auto& field = effect.particles.field();
        REQUIRE(field.size() == 3);
        for (usize emitter = 0; emitter < field.size(); ++emitter) {
            CHECK(field.emitter(emitter).descriptor().texture == "FBALLX");
        }
        for (s32 tick = 0; tick < hz / 2; ++tick) {
            f.arsenal.missiles().update(1.0f / static_cast<f32>(hz), nullptr);
        }
        REQUIRE(f.arsenal.missiles().count() == 1);
        CHECK(field.particleCount() > 0);
        f.device.draws.clear();
        f.arsenal.missiles().draw(f.device, Mat4{1}, {});
        CHECK(visibleFlameTriangles(f.device, field.textureOf(0)) > 0);
        f.arsenal.missiles().clear();
    }
    f.arsenal.clear();
}

TEST_CASE("red potion bursts retain all eighteen authored flame emitters",
          "[game][player-arsenal][alpha-potion-flames][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    REQUIRE(f.classes.load(root / "pdata"));
    const auto treeIndex = f.weapons.trees.find("MP_FIRE");
    REQUIRE(treeIndex);
    const auto& tree = f.weapons.trees.tree(*treeIndex);
    CHECK(std::ranges::count_if(tree.nodes, [](const auto& node) { return node.particle >= 0; }) ==
          18);
    // StartMagicFX (80092DF4) starts MP_FIRE itself: its eighteen PARTCLE children
    // use template 8 / DRAGONBREATH, rather than a separate code-created fire effect.
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        f.effects.clear();
        f.arsenal.burstPotion(1, f.actor.position(), 32, false);
        REQUIRE(f.effects.count() == 1);
        const auto& effect = f.effects.effect(0);
        REQUIRE(effect.name == "MP_FIRE");
        const auto& field = effect.particles.field();
        REQUIRE(field.size() == 18);
        for (usize emitter = 0; emitter < field.size(); ++emitter) {
            CHECK(field.emitter(emitter).descriptor().texture == "DRAGONBREATH");
            CHECK(field.textureOf(emitter) != &f.device.whiteTexture());
        }
        // Check the actual submitted flame quads, not just the mesh rings/sparks.
        for (s32 tick = 0; tick < hz; ++tick) {
            f.effects.update(1.0f / static_cast<f32>(hz));
            if (tick != hz / 2 - 1 && tick != hz - 1) {
                continue;
            }
            CAPTURE(tick);
            REQUIRE(field.particleCount() > 0);
            f.device.draws.clear();
            f.effects.draw(f.device, Mat4{1}, {});
            CHECK(visibleFlameTriangles(f.device, field.textureOf(0)) > 0);
        }
        f.effects.update(4);
        CHECK(f.effects.count() == 0);
    }
    f.arsenal.clear();
}

TEST_CASE("every Super Shot volley renders the authored weapon streak until expiry",
          "[game][player-arsenal][super-shot-streak][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    REQUIRE(f.classes.load(root / "pdata"));
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, damage::kSuperShot, 2, -1);
    inventory.addPowerup(powerup::kWeapon, damage::kThreeWayShot, 0, 30);
    PlayerFigure figure;
    const auto slot = f.weapons.textures.find("WEP_STREAK");
    REQUIRE(slot);
    const auto* texture = &f.weapons.textures.texture(f.device, *slot);
    const auto& image = f.weapons.textures.image(*slot);
    REQUIRE(std::ranges::any_of(image.pixels, [](u8 value) { return value != 0; }));
    WorldCamera view;
    view.pitch = 0.6f;
    const auto camera = CameraFrame::of(view);
    for (s32 volley = 0; volley < 2; ++volley) {
        f.arsenal.launchSuperShot(f.actor, &figure);
        REQUIRE(f.arsenal.missiles().count() == 3);
        f.arsenal.missiles().update(0.1f, nullptr);
        f.device.draws.clear();
        f.arsenal.missiles().draw(f.device, Mat4{1}, {}, &camera);
        usize streaks = 0;
        for (const auto& draw : f.device.draws) {
            if (draw.texture != texture) {
                continue;
            }
            ++streaks;
            REQUIRE(draw.vertices.size() == 6);
            CHECK(draw.state.depthTest);
            CHECK(draw.vertices.front().color == Color::rgba(255, 255, 255, 190));
            CHECK(glm::length(glm::cross(draw.vertices[1].position - draw.vertices[0].position,
                                         draw.vertices[2].position - draw.vertices[0].position)) >
                  0);
        }
        CHECK(streaks == 3);
        CHECK(f.arsenal.missiles().missile(0).streak.forward ==
              f.classes.stats(f.actor.save().character)->streakForward);
        f.arsenal.missiles().update(PlayerMissiles::kLifeSeconds, nullptr);
        f.device.draws.clear();
        f.arsenal.missiles().draw(f.device, Mat4{1}, {}, &camera);
        CHECK(f.device.draws.empty());
    }
    f.arsenal.clear();
}

TEST_CASE("amulet throws retain their player streak alongside all four authored elemental effects",
          "[game][player-arsenal][weapon-streak][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    for (s32 character = 0; character < kStartingClassCount; ++character) {
        CAPTURE(character);
        Fixture f;
        REQUIRE(f.weapons.load(root / "WEAPONS"));
        REQUIRE(f.classes.load(root / "pdata"));
        f.actor.save().character = character;
        f.actor.save().color = 3; // Green costume; deliberately not the fire/lightning/light tint.
        auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
        REQUIRE(figure);
        f.arsenal.bind(
            {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
        auto& inventory = f.actor.save().progress().inventory;
        for (u32 element = 1; element <= 4; ++element) {
            CAPTURE(element);
            inventory.powerups = {};
            inventory.addPowerup(powerup::kWeapon, element, 0, 60);
            const auto count = f.arsenal.missiles().visuals().count();
            const auto preparedRider = f.arsenal.weaponRider(f.actor, *figure);
            REQUIRE(preparedRider);
            REQUIRE(preparedRider->archive == figure->effects());
            REQUIRE(preparedRider->tree != nullptr);
            CHECK(preparedRider->tree->name == WeaponGlow::throwTree(element));
            REQUIRE(preparedRider->lenders.size() == 1);
            CHECK(preparedRider->lenders.front() == &f.weapons.textures);
            CHECK(f.arsenal.missiles().visuals().count() == count);
            f.arsenal.launchWeapon(f.actor, figure.get(), {0, 0, 1}, 1, true);
            REQUIRE(f.arsenal.missiles().count() == 1);
            const auto& shot = f.arsenal.missiles().missile(0);
            REQUIRE(shot.streak.texture != nullptr);
            REQUIRE(shot.rider != 0);
            CHECK(shot.streak.forward == f.classes.stats(character)->streakForward);
            const bool magicWeapon = character == 2 || character == 6;
            const auto color = Color::rgba(0, 255, 0, magicWeapon ? 62 : 190);
            CHECK(shot.streak.color == color);
            CHECK((shot.effect == 0) == magicWeapon);
            f.arsenal.missiles().update(0.1f, nullptr);
            const auto& visuals = f.arsenal.missiles().visuals();
            bool riderFound = false;
            for (usize i = 0; i < visuals.count(); ++i) {
                const auto& effect = visuals.effect(i);
                if (effect.id == shot.rider) {
                    riderFound = true;
                    const auto currentRider = f.arsenal.weaponRider(f.actor, *figure);
                    REQUIRE(currentRider);
                    REQUIRE(currentRider->textureFrame);
                    CHECK(*currentRider->textureFrame == visuals.textureFrame(effect));
                    CHECK(effect.lenders == currentRider->lenders);
                    CHECK(effect.name == WeaponGlow::throwTree(element));
                    CHECK(effect.position == shot.position);
                    REQUIRE(effect.particles.field().size() > 0);
                    CHECK(effect.particles.field().particleCount() > 0);
                    for (usize emitter = 0; emitter < effect.particles.field().size(); ++emitter) {
                        CHECK(effect.particles.field().textureOf(emitter) !=
                              &f.device.whiteTexture());
                    }
                }
            }
            REQUIRE(riderFound);
            WorldCamera view;
            view.pitch = 0.6f;
            const auto camera = CameraFrame::of(view);
            f.device.draws.clear();
            f.arsenal.missiles().draw(f.device, Mat4{1}, {}, &camera);
            usize streaks = 0;
            for (const auto& draw : f.device.draws) {
                if (draw.texture == shot.streak.texture) {
                    ++streaks;
                    REQUIRE(draw.vertices.size() == 6);
                    CHECK(draw.vertices.front().color == color);
                    CHECK(draw.state.depthTest);
                }
            }
            CHECK(streaks == 1);
            CHECK(f.device.draws.size() > streaks); // The elemental rider is also rendered.
            f.arsenal.missiles().update(PlayerMissiles::kLifeSeconds, nullptr);
            CHECK(f.arsenal.missiles().count() == 0);
        }
        f.arsenal.clear(); // Figures own the borrowed amulet trees.
    }
}

TEST_CASE("level 75 potion casts wear the authored healing hearts without granting free health",
          "[game][player-arsenal][magic-hearts][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    REQUIRE(f.classes.load(root / "pdata"));
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    nullptr,
                    {},
                    false,
                    false,
                    &powerups});
    s32 level = 75;
    bool thrown = false;
    SECTION("burst") {}
    SECTION("thrown") {
        thrown = true;
    }
    SECTION("level 74 has no hearts") {
        level = 74;
    }
    f.actor.save().character = 5; // Knight; the casting perk is shared by every class.
    f.actor.save().progress().experience = levelExperience(level);
    f.actor.save().progress().health = 200;
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPotions(1, 1);
    const auto cast = [&] {
        if (thrown) {
            f.arsenal.throwPotion(f.actor);
        } else {
            f.arsenal.usePotion(f.actor);
        }
    };
    cast();
    usize hearts = 0;
    u32 id = 0;
    for (usize i = 0; i < f.effects.count(); ++i) {
        const auto& effect = f.effects.effect(i);
        if (effect.name == "MAGICHEALTH") {
            ++hearts;
            id = effect.id;
            CHECK(effect.scale == Approx(std::min(1.0f, f.arsenal.potionPowerOf(f.actor, 1) / 32)));
            REQUIRE(effect.attachment);
            CHECK(*effect.attachment == f.actor.transform());
        }
    }
    REQUIRE(hearts == (level >= 75 ? 1 : 0));
    CHECK(f.actor.save().health() == 200); // Actual damage, not the visual, awards healing.
    const auto count = f.effects.count();
    cast(); // Empty inventory must not create another cast effect.
    CHECK(f.effects.count() == count);
    f.actor.place({20, 0, 40});
    f.arsenal.followCaster(f.actor);
    for (usize i = 0; i < f.effects.count(); ++i) {
        if (f.effects.effect(i).id == id) {
            CHECK(*f.effects.effect(i).attachment == f.actor.transform());
        }
    }
    for (s32 i = 0; i < 600; ++i) {
        f.effects.update(1.0f / 30);
        f.arsenal.followCaster(f.actor);
    }
    CHECK_FALSE(f.effects.playing(id));
    f.arsenal.clear();
    f.effects.clear();
}

TEST_CASE("Phoenix fires fixed fire damage without a permanent familiar or weapon enchantments",
          "[game][items][player-arsenal][phoenix][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture f;
    f.arsenal.clear();
    REQUIRE(f.classes.load(root / "pdata"));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    PlayerFigure figure; // no earned familiar and no class SFX archive required
    bool boss = false;
    SECTION("ordinary level") {}
    SECTION("boss has no gravity") {
        boss = true;
    }
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    nullptr,
                    {},
                    false,
                    boss});
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
    inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot | 4, 0, 60);
    const Vec3 aim{10, 5, 40};
    f.arsenal.launchFamiliar(f.actor, &figure, aim);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto& shot = f.arsenal.missiles().missile(0);
    CHECK(shot.damage == 10);
    CHECK(shot.flags == 0x11);
    CHECK(shot.owner == 3);
    CHECK_FALSE(shot.breaksPotions);
    CHECK(shot.spec->weight == (boss ? 0 : 10));
    REQUIRE(shot.effect != 0);
    CHECK(f.arsenal.missiles().visuals().effect(0).name == "PHOENIX_FBALL");
    inventory.powerups[0].on = false;
    f.arsenal.launchFamiliar(f.actor, &figure, aim);
    CHECK(f.arsenal.missiles().count() == 1);
    f.arsenal.missiles().update(0.1f, nullptr);
    f.arsenal.missiles().draw(f.device, Mat4{1}, {});
    REQUIRE_FALSE(f.device.draws.empty()); // disabling does not invalidate a flying shot
    const std::array targets{MissileTarget{1000, f.arsenal.missiles().missile(0).position, 3, 8}};
    f.arsenal.missiles().update(0.01f, nullptr, targets);
    const auto impacts = f.arsenal.missiles().takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts.front().target == 1000);
    CHECK(impacts.front().damage == 10);
    CHECK(impacts.front().flags == 0x11);
    f.arsenal.clear();
}

TEST_CASE("both cursor-aimed familiars correct height without steering toward an offset target",
          "[game][player-arsenal][cursor-projectile-aim][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture fixture;
    REQUIRE(fixture.classes.load(root / "pdata"));
    REQUIRE(fixture.weapons.load(root / "WEAPONS"));
    fixture.actor.save().progress().experience = levelExperience(80);
    auto figure = PlayerFigure::load(fixture.device, root, fixture.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    REQUIRE(figure->familiarTier() > 0);
    fixture.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0,
                                                         60);
    const Vec3 target{11, 1.5f, 40};
    const auto shots = fixture.arsenal.prepareFamiliar(fixture.actor, *figure, target, true);
    REQUIRE(shots);
    REQUIRE(shots->size() == 2);
    CHECK(shots->front().position != shots->back().position);
    for (const auto& shot : *shots) {
        REQUIRE(shot.velocity);
        CHECK(shot.velocity->x == 0);
        CHECK(shot.velocity->z == Approx(35));
        CHECK(shot.velocity->y < 0);
    }
}

TEST_CASE("a ranged volley fires the player's weapon and both earned and Phoenix familiars",
          "[game][player-arsenal][phoenix][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    for (const s32 level : {30, 80, 99}) {
        CAPTURE(character);
        CAPTURE(level);
        Fixture f;
        f.arsenal.clear();
        REQUIRE(f.classes.load(root / "pdata"));
        REQUIRE(f.weapons.load(root / "WEAPONS"));
        f.actor.save().character = character;
        f.actor.turnTo(0.7f);
        f.actor.save().progress().experience = levelExperience(level);
        auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
        REQUIRE(figure);
        REQUIRE(figure->familiarTier() == (level < 80 ? 1 : 2));
        REQUIRE(figure->familiarMissile().bound());
        const std::array<TextureSet*, 1> lenders{&f.weapons.textures};
        f.arsenal.bind(
            {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}},
            lenders);
        auto& inventory = f.actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        const Vec3 aim{10, 5, 40};
        f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), 1, true, aim);
        f.arsenal.launchFamiliar(f.actor, figure.get(), aim);
        REQUIRE(f.arsenal.missiles().count() == 3);
        CHECK(f.arsenal.missiles().missile(0).breaksPotions);
        const auto& familiar = f.arsenal.missiles().missile(1);
        const auto& phoenix = f.arsenal.missiles().missile(2);
        CHECK_FALSE(familiar.breaksPotions);
        CHECK_FALSE(phoenix.breaksPotions);
        CHECK(familiar.damage == Catch::Approx(0.1f * static_cast<f32>(level)));
        CHECK(familiar.flags == 0);
        CHECK(phoenix.damage == 10);
        CHECK(phoenix.flags == 0x11);
        CHECK(familiar.owner == f.actor.player());
        CHECK(phoenix.owner == f.actor.player());
        const auto* stats = f.classes.stats(character);
        REQUIRE(stats);
        const f32 scale = PlayerFigure::bodyScale(f.actor.save(), {});
        CHECK(glm::distance(familiar.position, phoenix.position) ==
              Approx(2.0f * std::abs(stats->familiarShotOffset.x) * scale));
        CHECK(glm::distance(familiar.position, phoenix.position) > 0.1f);
        CHECK(familiar.age == phoenix.age);
        for (const auto* shot : {&familiar, &phoenix}) {
            const Vec3 toTarget = aim - shot->position;
            CHECK(glm::dot(glm::normalize(Vec2{shot->velocity.x, shot->velocity.z}),
                           glm::normalize(Vec2{toTarget.x, toTarget.z})) == Approx(1));
            CHECK(std::hypot(shot->velocity.x, shot->velocity.z) == Approx(35));
        }
        REQUIRE(familiar.effect != 0);
        REQUIRE(phoenix.effect != 0);
        CHECK(familiar.effect != phoenix.effect);

        // Disabling the timed item must leave the earned familiar's next volley intact.
        f.arsenal.missiles().clear();
        inventory.powerups[0].on = false;
        f.arsenal.launchWeapon(f.actor, figure.get(), f.actor.facing(), 1, true, aim);
        f.arsenal.launchFamiliar(f.actor, figure.get(), aim);
        REQUIRE(f.arsenal.missiles().count() == 2);
        CHECK(f.arsenal.missiles().missile(1).flags == 0);
        CHECK(f.arsenal.missiles().missile(1).damage ==
              Catch::Approx(0.1f * static_cast<f32>(level)));
        f.arsenal.clear();
    }
}

TEST_CASE("equipped gauntlets route the shooter's textures into complete projectile playback",
          "[game][player-arsenal][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/YEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    Fixture f;
    f.arsenal.clear();
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    auto figure = PlayerFigure::load(f.device, root, f.actor.save(), Entrance::Skip);
    REQUIRE(figure);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
    f.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kLeftGauntlet, 0,
                                                   60);
    f.arsenal.launchGauntlet(f.actor, figure.get(), true);
    REQUIRE(f.arsenal.missiles().count() == 1);
    REQUIRE(f.arsenal.missiles().missile(0).effect != 0);
    REQUIRE(f.arsenal.missiles().missile(0).streak.texture != nullptr);
    CHECK(f.arsenal.missiles().missile(0).streak.color == Color::rgba(255, 255, 0, 190));
    f.arsenal.missiles().update(0.1f, nullptr);
    const auto& field = f.arsenal.missiles().visuals().effect(0).particles.field();
    REQUIRE(field.size() == 2);
    REQUIRE(field.particleCount() > 0);
    REQUIRE(figure->effects());
    const auto slot = figure->effects()->textures.find("ELEC_SPARK");
    REQUIRE(slot);
    CHECK(field.textureOf(1) == &figure->effects()->textures.texture(f.device, *slot));
    f.arsenal.clear(); // borrowed figure archives must outlive every missile and tail
}

TEST_CASE("super shot spends one charge per volley and preserves the last charged shot",
          "[game][items][player-arsenal]") {
    Fixture f;
    PlayerFigure figure;
    auto& inventory = f.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, damage::kSuperShot, 1, -1);
    inventory.addPowerup(powerup::kWeapon, damage::kThreeWayShot, 0, 30);
    bool tower = false;
    bool boss = false;
    SECTION("ordinary level") {}
    SECTION("tower conserves charges") {
        tower = true;
    }
    SECTION("boss uses smaller damage multiplier") {
        boss = true;
    }
    f.arsenal.bind({f.device,
                    f.classes,
                    f.weapons,
                    f.collision,
                    f.effects,
                    f.audio,
                    nullptr,
                    {},
                    tower,
                    boss});
    f.arsenal.launchSuperShot(f.actor, &figure);
    REQUIRE(f.arsenal.missiles().count() == 3);
    for (usize i = 0; i < 3; ++i) {
        const auto& shot = f.arsenal.missiles().missile(i);
        CHECK(shot.spec == &MissileSpec::superShot());
        CHECK((shot.flags & damage::kSuperShot) != 0);
        CHECK(shot.damage == Approx(boss ? 7.5f : 10.0f));
        CHECK(shot.velocity.y == 0);
    }
    CHECK((inventory.powerup(powerup::kWeapon, damage::kSuperShot) != nullptr) == tower);
    if (!tower) {
        f.arsenal.launchSuperShot(f.actor, &figure);
        REQUIRE(f.arsenal.missiles().count() == 6);
        CHECK((f.arsenal.missiles().missile(3).flags & damage::kSuperShot) == 0);
    }
}

TEST_CASE("super shot ignores assisted targets while normal and depleted shots retain them",
          "[game][player-arsenal][alpha-combat]") {
    Fixture f;
    PlayerFigure figure;
    const Vec3 target = f.actor.position() + Vec3{8, 10, 20};
    auto& inventory = f.actor.save().progress().inventory;
    f.arsenal.launchWeapon(f.actor, &figure, f.actor.facing(), 1, true, target);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const Vec3 normal = f.arsenal.missiles().missile(0).velocity;
    CHECK(normal.x > 0);
    CHECK(normal.y > 0);

    inventory.addPowerup(powerup::kWeapon, damage::kSuperShot, 1, -1);
    f.arsenal.launchSuperShot(f.actor, &figure, target);
    REQUIRE(f.arsenal.missiles().count() == 2);
    const auto& charged = f.arsenal.missiles().missile(1);
    CHECK((charged.flags & damage::kSuperShot) != 0);
    // ModifyPlayerDpos (0x80085fa0) keeps the supplied heading for Super Shot.
    CHECK(charged.velocity.x == Approx(0).margin(0.0001f));
    CHECK(charged.velocity.y == Approx(0).margin(0.0001f));
    CHECK(charged.velocity.z > 0);

    f.arsenal.launchSuperShot(f.actor, &figure, target);
    REQUIRE(f.arsenal.missiles().count() == 3);
    const auto& depleted = f.arsenal.missiles().missile(2);
    CHECK((depleted.flags & damage::kSuperShot) == 0);
    CHECK(depleted.velocity.x == Approx(normal.x));
    CHECK(depleted.velocity.y == Approx(normal.y));
    CHECK(depleted.velocity.z == Approx(normal.z));
}

TEST_CASE("Skorne gauntlets use their own elemental projectiles without consuming super shot",
          "[game][items]") {
    for (const bool left : {false, true}) {
        Fixture f;
        PlayerFigure figure;
        auto& inventory = f.actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial,
                             left ? powerup::kLeftGauntlet : powerup::kRightGauntlet, 0, 60);
        inventory.addPowerup(powerup::kWeapon, damage::kSuperShot, 5, -1);
        f.arsenal.launchGauntlet(f.actor, &figure, left);
        REQUIRE(f.arsenal.missiles().count() == 1);
        const auto& missile = f.arsenal.missiles().missile(0);
        CHECK(missile.spec->model == (left ? "BOSSG_ELEC" : "BOSSG_ACID"));
        CHECK((missile.flags & 0xFU) == (left ? 2U : 4U));
        CHECK(missile.damage == 5);
        CHECK(inventory.powerup(powerup::kWeapon, damage::kSuperShot)->charge == 5);
    }
}

/** A small visible effect and looping tone keep wall feedback covered without game data. */
std::filesystem::path impactAssets() {
    const auto root = test::scratchDirectory("player-wall-impact");
    const auto weapons = root / "WEAPONS";
    std::filesystem::create_directories(weapons);
    writeTextFile(weapons / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(weapons / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(weapons / "white.png", test::kTinyPng);
    writeTextFile(weapons / "textures.json",
                  R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2},
                  {"name":"WEP_STREAK","file":"white.png","width":2,"height":2}]})");
    writeTextFile(weapons / "animations.json", R"({"trees":[
      {"name":"SPARKS","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":10,"frameRate":30}]},
      {"name":"EXPSMALL","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":10,"frameRate":30}]}]})");
    const auto bank = root / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {pcm.begin(), pcm.end()}}}};
    test::writeNativeSoundBank(bank, R"({"sounds":[
      {"index":0,"name":"S_WEAPONHITWOOD","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
      {"index":1,"name":"S_3WAYAXE","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
      {"index":2,"name":"S_5WAYAXE","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
      {"index":3,"name":"S_SPLASH","duration":-1,"volume":127,
       "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}]})",
                               bankSamples);
    test::convertModelFixture(weapons);
    return root;
}

TEST_CASE("normal and spread throws render saturated costume streaks for every class",
          "[game][player-arsenal][weapon-streak]") {
    Fixture f;
    REQUIRE(f.weapons.load(impactAssets() / "WEAPONS"));
    PlayerFigure figure;
    WorldCamera view;
    view.pitch = 0.6f;
    const auto camera = CameraFrame::of(view);
    constexpr std::array kColors{Color::rgba(255, 255, 0, 190), Color::rgba(0, 0, 255, 190),
                                 Color::rgba(255, 0, 0, 190), Color::rgba(0, 255, 0, 190)};
    for (s32 character = 0; character < kClassCount; ++character) {
        for (s32 color = 0; color < kColorCount; ++color) {
            for (const u32 spread : {0U, damage::kThreeWayShot, damage::kFiveWayShot}) {
                CAPTURE(character, color, spread);
                f.arsenal.bind(
                    {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
                f.actor.save().character = character;
                f.actor.save().color = color;
                auto& inventory = f.actor.save().progress().inventory;
                inventory.powerups = {};
                if (spread != 0) {
                    inventory.addPowerup(powerup::kWeapon, spread, 0, 30);
                }
                f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, true);
                const usize count = static_cast<usize>(PowerupEffects::of(inventory).shots());
                REQUIRE(f.arsenal.missiles().count() == count);
                const auto* texture = f.arsenal.missiles().missile(0).streak.texture;
                REQUIRE(texture != nullptr);
                Color expected = kColors[static_cast<usize>(color)];
                if (character == 2 || character == 6 || character == 10 || character == 14 ||
                    character == 16) {
                    expected.a = 62;
                }
                f.arsenal.missiles().update(0.1f, nullptr);
                f.device.draws.clear();
                f.arsenal.missiles().draw(f.device, Mat4{1}, {}, &camera);
                REQUIRE(f.device.draws.size() == count);
                for (const auto& draw : f.device.draws) {
                    CHECK(draw.texture == texture);
                    REQUIRE(draw.vertices.size() == 6);
                    CHECK(draw.vertices.front().color == expected);
                    CHECK(draw.state.depthTest);
                    CHECK(glm::length(glm::cross(
                              draw.vertices[1].position - draw.vertices[0].position,
                              draw.vertices[2].position - draw.vertices[0].position)) > 0);
                }
                f.arsenal.missiles().update(PlayerMissiles::kLifeSeconds, nullptr);
                f.device.draws.clear();
                f.arsenal.missiles().draw(f.device, Mat4{1}, {}, &camera);
                CHECK(f.device.draws.empty());
            }
        }
    }
    f.arsenal.clear();
}

TEST_CASE("weapon wall impacts render once and use the level-selected sound",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds,
                    "S_WEAPONHITWOOD"});
    MissileImpact impact{.position = {4, 5, 6}};
    SECTION("wall sparks and their sound outlive the projectile") {
        f.arsenal.presentImpact(impact);
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == "SPARKS");
        CHECK(f.effects.effect(0).position == impact.position);
        CHECK(f.effects.effect(0).tint.a == 96);
        CHECK(f.effects.effect(0).unlit);
        CHECK_FALSE(f.effects.effect(0).depthWrite);
        CHECK(sounds.voiceCount() == 1);
        std::array<f32, 256> output{};
        mixer.mix(output);
        CHECK(output.back() > 0);
        f.effects.draw(f.device, Mat4{1}, {});
        CHECK_FALSE(f.device.draws.empty());
        f.effects.update(1);
        CHECK(f.effects.count() == 0);
    }
    SECTION("bombs use their own effect") {
        impact.effect = MissileSpec::of(7).impactTree;
        f.arsenal.presentImpact(impact);
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == "EXPSMALL");
        CHECK(f.effects.effect(0).tint.a == 255);
        CHECK(sounds.voiceCount() == 1);
    }
    SECTION("water replaces even a silent side shot's wall sound without changing its effect") {
        impact.liquid = true;
        impact.wallSound = MissileWallSound::Silent;
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 1);
        CHECK(sounds.voiceCount() == 1);
        std::array<f32, 256> output{};
        mixer.mix(output);
        CHECK(output.back() > 0);
    }
    SECTION("a distant water impact stays inaudible") {
        impact.liquid = true;
        f.arsenal.presentImpact(impact, 100);
        CHECK(f.effects.count() == 1);
        CHECK(sounds.voiceCount() == 0);
    }
    SECTION("target hits do not acquire extra wall feedback") {
        impact.target = 1000;
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 0);
        CHECK(sounds.voiceCount() == 0);
    }
    SECTION("missing artwork does not suppress audio") {
        f.weapons.clear();
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 0);
        CHECK(sounds.voiceCount() == 1);
    }
    SECTION("an unspecified sound is silent rather than an invented fallback") {
        f.arsenal.bind(
            {f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds, {}});
        f.arsenal.presentImpact(impact);
        CHECK(f.effects.count() == 1);
        CHECK(sounds.voiceCount() == 0);
    }
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("ordinary impact descriptors retain native artwork and cue priority without outputs",
          "[player-arsenal][projectile-impact]") {
    Fixture f;
    REQUIRE(f.weapons.load(impactAssets() / "WEAPONS"));
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr,
                    "S_WEAPONHITWOOD"});
    const auto sound =
        GENERATE(MissileWallSound::Level, MissileWallSound::ThreeWay, MissileWallSound::FiveWay,
                 MissileWallSound::Silent, MissileWallSound::Ricochet);
    const bool liquid = GENERATE(false, true);
    const s32 target = GENERATE(-1, 1000, 7500);
    MissileImpact impact{
        .position = {4, 5, 6}, .target = target, .wallSound = sound, .liquid = liquid};
    const auto uploads = f.device.texturesCreated;
    const auto emission = f.arsenal.impactPresentation(impact, 17);
    CHECK(f.effects.count() == 0);
    CHECK(f.device.texturesCreated == uploads);
    if (target >= 0) {
        CHECK(emission.archive == nullptr);
        CHECK_FALSE(emission.cue);
        return;
    }
    REQUIRE(emission.archive == &f.weapons);
    CHECK(emission.tree == "SPARKS");
    CHECK(emission.position == impact.position);
    CHECK(emission.setting.unlit);
    CHECK_FALSE(emission.setting.depthWrite);
    CHECK(emission.setting.tint.a == 96);
    if (liquid) {
        REQUIRE(emission.cue);
        CHECK(emission.cue->name == "S_SPLASH");
        CHECK(emission.cue->playerDistance == 17.0f);
        CHECK(emission.cue->volume == LevelSoundscape::kSplashLevel / 255.0f);
    } else if (sound == MissileWallSound::Silent) {
        CHECK_FALSE(emission.cue);
    } else {
        REQUIRE(emission.cue);
        constexpr std::array<std::string_view, 5> kNames{"S_WEAPONHITWOOD", "S_3WAYAXE",
                                                         "S_5WAYAXE", "", "S_RICOCHET"};
        CHECK(emission.cue->name == kNames[static_cast<usize>(sound)]);
        CHECK_FALSE(emission.cue->playerDistance);
        CHECK(emission.cue->volume == 1);
    }
}

TEST_CASE("an obstructed muzzle still produces impact feedback without launching a weapon",
          "[game][world][player-arsenal][projectile-impact][native-scenery-projectiles]") {
    const s32 bridge = GENERATE(0, 1, 2, 3);
    const bool spread = GENERATE(false, true);
    CAPTURE(bridge, spread);
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    PlayerArsenal::Resources resources{f.device,  f.classes, f.weapons, f.collision,
                                       f.effects, f.audio,   &sounds,   "S_WEAPONHITWOOD"};
    s32 calls = 0;
    if (bridge != 0) {
        resources.strikeMuzzleObject = [&](s32 object, f32 power, u32 flags, s32 player) {
            ++calls;
            CHECK(object == 77);
            CHECK(power == PlayerMissiles::kLeastDamage);
            CHECK(flags == (spread ? damage::kFiveWayShot : 0));
            CHECK(player == f.actor.player());
            return bridge == 1; // unrelated owners may refuse without bypassing the wall
        };
    }
    f.arsenal.bind(resources);
    if (spread) {
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot, 0,
                                                       30, true);
    }
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-30, -5, 22}, Vec3{30, -5, 22}, Vec3{0, 40, 22}};
    wall.object = bridge == 3 ? -1 : 77;
    f.collision.build({wall});
    PlayerFigure figure;
    f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, spread);
    CHECK(f.arsenal.missiles().count() == 0);
    CHECK(calls == (bridge == 1 || bridge == 2 ? 1 : 0));
    CHECK(f.arsenal.missiles().takeImpacts().empty());
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "SPARKS");
    CHECK(sounds.voiceCount() == 1);
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("a fence only the party walks against lets a weapon out of the muzzle",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr,
                    "S_WEAPONHITWOOD"});
    // PlayerStartMissile clears the muzzle with WeaponWallCollide's mask, which leaves out the
    // 0x100 fences that hold the party back: B5's lava ledge is one, and shots cross it.
    CollisionTriangle fence;
    fence.normal = {0, 0, -1};
    fence.vertices = {Vec3{-30, -5, 22}, Vec3{30, -5, 22}, Vec3{0, 40, 22}};
    fence.objectFlags = 0x100;
    f.collision.build({fence});
    PlayerFigure figure;
    f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, false);
    CHECK(f.arsenal.missiles().count() == 1);
    CHECK(f.effects.count() == 0);
    f.arsenal.clear();
    f.effects.clear();
}

TEST_CASE("spread volleys leave every impact but sound only the centre shot",
          "[game][world][player-arsenal][projectile-impact]") {
    const auto root = impactAssets();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, nullptr);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, &sounds,
                    "S_WEAPONHITWOOD"});
    s32 shots = 3;
    SECTION("three-way") {
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, damage::kThreeWayShot, 0,
                                                       30);
    }
    SECTION("five-way") {
        shots = 5;
        f.actor.save().progress().inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot, 0,
                                                       30);
    }
    PlayerFigure figure;
    f.arsenal.launchWeapon(f.actor, &figure, {0, 0, 1}, 1, true);
    REQUIRE(f.arsenal.missiles().count() == static_cast<usize>(shots));
    CHECK(f.arsenal.missiles().missile(0).wallSound ==
          (shots == 3 ? MissileWallSound::ThreeWay : MissileWallSound::FiveWay));
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-100, -10, 30}, Vec3{100, -10, 30}, Vec3{0, 100, 30}};
    f.collision.build({wall});
    f.arsenal.missiles().update(1, &f.collision);
    REQUIRE(f.arsenal.missiles().count() == 0);
    const auto impacts = f.arsenal.missiles().takeImpacts();
    REQUIRE(impacts.size() == static_cast<usize>(shots));
    for (const auto& impact : impacts) {
        f.arsenal.presentImpact(impact);
    }
    CHECK(f.effects.count() == static_cast<usize>(shots));
    CHECK(sounds.voiceCount() == 1);
    CHECK(f.arsenal.missiles().takeImpacts().empty());
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("player arsenal throws the next potion from its owner's hand",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.actor.save().progress().inventory.addPotions(2, 1);
    f.actor.save().progress().inventory.addPotions(4, 1);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto& missile = f.arsenal.missiles().missile(0);
    REQUIRE(missile.owner == 3);
    REQUIRE(missile.potion == 4);
    REQUIRE(missile.position == Vec3{10, 4, 22});
    REQUIRE(missile.velocity.y == Approx(5 * 0.707f));
    REQUIRE(missile.velocity.z == Approx(5 * 0.707f));
    REQUIRE(missile.potency == Approx(0.75f * f.arsenal.magicPowerOf(f.actor)));
    REQUIRE(missile.model != nullptr);
    REQUIRE(f.actor.save().progress().inventory.nextPotion() == 2);
    const auto used = f.arsenal.usePotion(f.actor);
    REQUIRE(used.has_value());
    REQUIRE(f.actor.save().progress().inventory.potions.empty());
    REQUIRE(f.arsenal.missiles().count() == 1); // Immediate potion doesn't launch a bottle.
    // A caster under 25 casts without DMG_HEAL; from 25 the magic carries it, thrown or used.
    CHECK(missile.flags == 0);
    CHECK(used->flags == 0);
    f.actor.save().progress().experience = levelExperience(25);
    f.actor.save().progress().inventory.addPotions(1, 2);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 2);
    CHECK((f.arsenal.missiles().missile(1).flags & damage::kHeal) != 0);
    const auto usedLater = f.arsenal.usePotion(f.actor);
    REQUIRE(usedLater.has_value());
    CHECK((usedLater->flags & damage::kHeal) != 0);
}

TEST_CASE("held potion throws fly forward on the retail ballistic arc",
          "[game][world][player-arsenal][potion-throw]") {
    Fixture f;
    f.actor.save().progress().inventory.addPotions(1, 1);
    f.arsenal.throwPotion(f.actor, 30);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto bottle = f.arsenal.missiles().missile(0);
    CHECK(bottle.velocity == Vec3(0, 0.707f * 50, 0.707f * 50));
    CHECK(bottle.spec->weight == 100);
    CHECK(bottle.spec->radius == 0.5f);
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 200}};
    f.collision.build({floor});
    f.arsenal.missiles().update(0.25f, &f.collision);
    REQUIRE(f.arsenal.missiles().count() == 1);
    CHECK(f.arsenal.missiles().missile(0).position.z > bottle.position.z + 8);
    CHECK(f.arsenal.missiles().missile(0).position.y > bottle.position.y + 5);
    for (s32 frame = 0; frame < 120 && f.arsenal.missiles().count() != 0; ++frame) {
        f.arsenal.missiles().update(1.0f / 60, &f.collision);
    }
    const auto impacts = f.arsenal.missiles().takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts.front().potion == 1);
    CHECK(impacts.front().position.z > bottle.position.z + 25);
    CHECK(impacts.front().position.z < bottle.position.z + 30);
}

TEST_CASE("unspecified carried potions cast and throw without consuming empty inventory",
          "[game][world][player-arsenal][cheats][multiplayer]") {
    Fixture f;
    f.actor.save().color = 2; // Red: cycling to fire must not retroactively award a colour bonus.
    auto& inventory = f.actor.save().progress().inventory;
    CHECK_FALSE(f.arsenal.usePotion(f.actor));
    f.arsenal.throwPotion(f.actor);
    CHECK(f.arsenal.missiles().count() == 0);
    inventory.addPotions(0, 2);
    const auto burst = f.arsenal.usePotion(f.actor);
    REQUIRE(burst);
    CHECK(burst->potion == 1);
    CHECK(burst->damage == Approx(40));
    CHECK(burst->potency == Approx(f.arsenal.magicPowerOf(f.actor)));
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    CHECK(f.arsenal.missiles().missile(0).potion == 2);
    CHECK(inventory.potions.empty());

    PlayerActor partner;
    partner.spawn(1, {}, nullptr, Vec3{0}, 0);
    partner.save().progress().inventory.addPotions(0, 1);
    const auto shared = f.arsenal.usePotion(partner);
    REQUIRE(shared);
    CHECK(shared->potion == 3);
    CHECK(f.arsenal.resolvePotionKind(2) == 2); // Explicit colours do not advance the cycle.
    CHECK(f.arsenal.resolvePotionKind(0) == 4);
    CHECK(f.arsenal.resolvePotionKind(0) == 1);
}

TEST_CASE("a potion of the caster's own colour goes off a tenth stronger",
          "[game][world][player-arsenal][damage-types]") {
    Fixture f;
    // The first colour is yellow, whose element is light (potion kind 3).
    f.actor.save().color = 0;
    const f32 power = f.arsenal.magicPowerOf(f.actor);
    f.actor.save().progress().inventory.addPotions(1, 1);
    f.actor.save().progress().inventory.addPotions(3, 1);
    const auto own = f.arsenal.usePotion(f.actor);
    REQUIRE(own.has_value());
    CHECK(own->potion == 3);
    CHECK(own->potency == Approx(1.1f * power));
    CHECK(own->damage == Approx(44.0f));
    const auto other = f.arsenal.usePotion(f.actor);
    REQUIRE(other.has_value());
    CHECK(other->potion == 1);
    CHECK(other->potency == Approx(power));
    CHECK(other->damage == Approx(40.0f));
    f.actor.save().progress().inventory.addPotions(3, 1);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    CHECK(f.arsenal.missiles().missile(0).potency == Approx(0.75f * 1.1f * power));
    CHECK(f.arsenal.missiles().missile(0).damage == Approx(44.0f));
}

TEST_CASE("assisted weapon launch aims from its actual muzzle without retaining a lock",
          "[game][world][player-arsenal][target-assist]") {
    Fixture f;
    PlayerFigure figure;
    const Vec3 target{16, 7, 40};
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false, target);
    REQUIRE(f.arsenal.missiles().count() == 1);
    const auto first = f.arsenal.missiles().missile(0);
    const Vec3 offset = target - first.position;
    const f32 time =
        std::hypot(offset.x, offset.z) / std::hypot(first.velocity.x, first.velocity.z);
    const Vec3 reached = first.position + first.velocity * time -
                         Vec3{0, 0.5f * first.spec->weight * time * time, 0};
    REQUIRE(glm::distance(reached, target) == Approx(0).margin(0.0001f));
    REQUIRE(first.velocity.x > 0);
    // No selected target preserves the ordinary forward throw (no sticky lock).
    f.arsenal.launchWeapon(f.actor, &figure, Vec3{0, 0, 1}, 1, false);
    REQUIRE(f.arsenal.missiles().count() == 2);
    REQUIRE(f.arsenal.missiles().missile(1).velocity.x == 0);
}

TEST_CASE("player arsenal tolerates missing artwork and clears projectiles before rebinding",
          "[game][world][player-arsenal]") {
    Fixture f;
    f.arsenal.launchWeapon(f.actor, nullptr, Vec3{0, 0, 1}, 1, true);
    REQUIRE(f.arsenal.missiles().count() == 0);
    f.actor.save().progress().inventory.addPotions(1, 2);
    f.arsenal.throwPotion(f.actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    f.arsenal.bind({f.device, f.classes, f.weapons, f.collision, f.effects, f.audio, nullptr, {}});
    REQUIRE(f.arsenal.missiles().count() == 0);
    REQUIRE(f.arsenal.missiles().takeImpacts().empty());
    f.arsenal.clear();
    f.arsenal.clear();
    f.arsenal.throwPotion(f.actor);
    f.arsenal.usePotion(f.actor);
    REQUIRE(f.actor.save().progress().inventory.potions.size() == 1);
    REQUIRE(f.arsenal.missiles().count() == 0);
}
} // namespace
