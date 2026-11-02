#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/EffectFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/DynamicLights.h"
#include "game/effects/WeaponGlow.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/TurboEmission.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void vectorMatches(const Vec3& actual, const Vec3& expected) {
    CHECK(actual.x == Approx(expected.x).margin(0.00001f));
    CHECK(actual.y == Approx(expected.y).margin(0.00001f));
    CHECK(actual.z == Approx(expected.z).margin(0.00001f));
}

void matrixMatches(const Mat4& actual, const Mat4& expected) {
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            CHECK(actual[column][row] == Approx(expected[column][row]).margin(0.00001f));
        }
    }
}

void carrierMatches(const MoveStrikes::Strike& actual, const MoveStrikes::Strike& expected) {
    // Pool identities belong to the authoritative dispatcher, not the preparation.
    CHECK(actual.flies == expected.flies);
    CHECK(actual.expanding == expected.expanding);
    CHECK(actual.collidesWorld == expected.collidesWorld);
    vectorMatches(actual.position, expected.position);
    vectorMatches(actual.facing, expected.facing);
    CHECK(actual.speed == expected.speed);
    CHECK(actual.radius == expected.radius);
    CHECK(actual.arc == expected.arc);
    CHECK(actual.damage == expected.damage);
    CHECK(actual.delayLeft == expected.delayLeft);
    CHECK(actual.launchIn == expected.launchIn);
    CHECK(actual.secondsLeft == expected.secondsLeft);
    CHECK(actual.damageTime == expected.damageTime);
}

ClassStats authoredStrike() {
    ClassStats stats;
    auto& strike = stats.moveStrikes.emplace_back();
    strike.type = MoveStrike::kFlies;
    strike.hitRadius = 2;
    strike.radius = 4;
    strike.delay = 0.25f;
    strike.maxTime = 3;
    strike.arc = 0.4f;
    strike.offset = {1, 2, 3};
    strike.amount = -2;
    strike.speed = 9;
    strike.angle = 0.3f;
    strike.effect = 0;
    strike.loopEffect = 1;
    strike.flags = 0x840;
    stats.moveEffects.resize(2);
    auto& birth = stats.moveEffects[0];
    birth.tree = "BIRTH";
    birth.sound = "BIRTH_SOUND";
    birth.offset = {4, 5, 6};
    birth.lifetime = 1.5f;
    birth.scale = 2;
    birth.flags = 0x10002;
    birth.color = 0xAB123456;
    birth.next = 1;
    stats.moveEffects[1].tree = "LOOP";
    stats.moveEffects[1].offset = {-1, 3, 2};
    return stats;
}

TEST_CASE("turbo effect lifetime keeps native missing-tree and empty-frame semantics",
          "[game][turbo-emission]") {
    MoveEffect effect;
    effect.lifetime = 4;
    TreeInfo tree;
    CHECK(TurboEmission::effectSeconds(effect, nullptr) == 0);
    CHECK(TurboEmission::effectSeconds(effect, &tree) == 4);
    effect.lifetime = 0;
    CHECK(TurboEmission::effectSeconds(effect, &tree) == 0);
    auto& sequence = tree.sequences.emplace_back();
    sequence.frames = 60;
    sequence.frameRate = 30;
    CHECK(TurboEmission::effectSeconds(effect, &tree) == Approx(2));
    sequence.frames = 0;
    sequence.frameRate = 0;
    CHECK(TurboEmission::effectSeconds(effect, &tree) == Approx(1));
    effect.lifetime = -1;
    CHECK(TurboEmission::effectSeconds(effect, &tree) == Approx(1));
}

TEST_CASE("turbo preparation preserves the native flying carrier and chained effect math",
          "[game][turbo-emission]") {
    const auto stats = authoredStrike();
    const auto& strike = stats.moveStrikes.front();
    const TurboEmission::Context context{6, {10, 20, 30}, {1, 0, 0}};
    const auto prepared = TurboEmission::prepare(stats, 0, context, 7, 1.5f);
    REQUIRE(prepared);
    REQUIRE(prepared->carrier);
    vectorMatches(prepared->volume.offset, {5, 7, 9});
    MoveStrikes reference;
    auto volume = strike;
    volume.offset += stats.moveEffects.front().offset;
    reference.start(volume, 3, context.position, context.facing, 7, 1.5f);
    carrierMatches(*prepared->carrier, reference.strike(0));
    CHECK(prepared->carrier->damage == 14);
    CHECK(prepared->carrier->launchIn == Approx(1.5f - 0.03332f));
    CHECK(prepared->carrier->secondsLeft == Approx(4.5f - 0.03332f));
    REQUIRE(prepared->effects.size() == 2);
    const auto& primary = prepared->effects[0];
    CHECK(primary.row == 0);
    CHECK(primary.primary);
    CHECK(primary.shake);
    CHECK(primary.carriesFlight);
    CHECK_FALSE(primary.attachment);
    CHECK_FALSE(primary.particle);
    vectorMatches(primary.position, {19, 27, 25});
    CHECK(primary.setting.yaw == Approx(kPi / 2 + 0.3f));
    vectorMatches(primary.setting.velocity, prepared->carrier->facing * 9.0f);
    CHECK(primary.setting.seconds == prepared->carrier->secondsLeft);
    CHECK(primary.setting.scale == 2);
    CHECK(primary.setting.tint.r == stats.moveEffects[0].tint().r);
    CHECK(primary.setting.tint.g == stats.moveEffects[0].tint().g);
    CHECK(primary.setting.tint.b == stats.moveEffects[0].tint().b);
    CHECK(primary.setting.shrinks);
    CHECK(primary.setting.then == "LOOP");
    REQUIRE(primary.setting.morphIn);
    CHECK(*primary.setting.morphIn == Approx(1.5f - 0.03332f));
    CHECK(primary.setting.holdForMorph);
    REQUIRE(primary.setting.light);
    vectorMatches(primary.setting.light->color, DynamicLights::ofClass(6));
    CHECK(primary.setting.light->radius == 8);
    CHECK_FALSE(primary.setting.light->swells);
    const auto& secondary = prepared->effects[1];
    CHECK_FALSE(secondary.primary);
    CHECK_FALSE(secondary.carriesFlight);
    CHECK_FALSE(secondary.setting.light);
    CHECK(secondary.setting.then.empty());
    vectorMatches(secondary.position, {15, 25, 30});
    vectorMatches(secondary.setting.velocity, Vec3{0});
}

TEST_CASE("a harming row that stays put hands its effect to its loop effect for its time",
          "[game][turbo-emission]") {
    // PlyrSfxDoDamageSub morphs any harming row's effect into its loop effect once it has
    // played (psfx.c 650): the dwarf's charge flame plays its three frames and then loops on
    // the charger for the ride.
    ClassStats stats = authoredStrike();
    auto& strike = stats.moveStrikes.front();
    strike.type = MoveStrike::kBursts;
    strike.speed = 0;
    strike.maxTime = 4.25f;
    const TurboEmission::Context context{4, {0, 0, 0}, {0, 0, 1}};
    const auto prepared = TurboEmission::prepare(stats, 0, context, 7, 0.1f);
    REQUIRE(prepared);
    REQUIRE(prepared->carrier);
    REQUIRE(prepared->effects.size() == 2);
    const auto& primary = prepared->effects[0];
    CHECK(primary.primary);
    CHECK_FALSE(primary.carriesFlight);
    CHECK(primary.setting.then == "LOOP");
    CHECK(primary.setting.seconds == Approx(0.1f + 4.25f));
    REQUIRE(primary.setting.morphIn);
    CHECK(*primary.setting.morphIn == Approx(0.1f));
    CHECK(primary.setting.holdForMorph);
    CHECK(prepared->effects[1].setting.then.empty());
    // A row without a loop effect plays its effect once, as before.
    strike.loopEffect = -1;
    const auto plain = TurboEmission::prepare(stats, 0, context, 7, 0.1f);
    REQUIRE(plain);
    CHECK(plain->effects[0].setting.then.empty());
    CHECK(plain->effects[0].setting.seconds == stats.moveEffects[0].lifetime);
}

TEST_CASE("turbo attachment descriptors retain native body root partner and detached policies",
          "[game][turbo-emission]") {
    auto stats = authoredStrike();
    auto& strike = stats.moveStrikes.front();
    strike.type = MoveStrike::kBursts;
    strike.speed = 0;
    const u32 flags = GENERATE(0U, 1U, 0x2000U, 0x2001U, 0x10U, 0x40U, 0x80U, 0x800U, 0x40000U);
    const bool detached = GENERATE(false, true);
    const bool partner = GENERATE(false, true);
    stats.moveEffects[0].flags = flags;
    const auto prepared =
        TurboEmission::prepare(stats, 0, {0, {}, {0, 0, 1}, detached, partner}, 8, 2);
    REQUIRE(prepared);
    REQUIRE(prepared->effects.size() == 2);
    const bool attached = !detached && (flags & (0x10U | 0x40U | 0x80U | 0x800U | 0x40000U)) == 0;
    CHECK(prepared->effects[0].attachment.has_value() == attached);
    if (attached) {
        const auto& attachment = *prepared->effects[0].attachment;
        const bool body = (flags & 1U) != 0;
        CHECK(attachment.bodyParent == body);
        CHECK(attachment.carrier);
        CHECK((attachment.actor == TurboEmission::Actor::Partner) ==
              ((flags & 0x2000U) != 0 && partner));
        const Vec3 offset = stats.moveEffects[0].offset + (body ? Vec3{0} : strike.offset);
        matrixMatches(attachment.local,
                      glm::rotate(glm::translate(Mat4{1}, offset), strike.angle, Vec3{0, 1, 0}));
    }
    if (!detached) {
        REQUIRE(prepared->effects[1].attachment);
        CHECK_FALSE(prepared->effects[1].attachment->carrier);
        matrixMatches(prepared->effects[1].attachment->local,
                      glm::translate(Mat4{1}, strike.offset + stats.moveEffects[1].offset));
    }
}

TEST_CASE("turbo particle descriptors preserve node names phases and previous-effect fallback",
          "[game][turbo-emission]") {
    MoveEffect effect;
    effect.tree = "RAIN";
    effect.sound = "L_HAND"; // A node, never an audio cue for this row type.
    effect.offset = {2, 3, 4};
    effect.scale = 6;
    effect.lifetime = 0.19f;
    effect.radius = 2.5f;
    effect.alphaMod = -45;
    effect.flags = GENERATE(0x01000000U, 0x02000000U, 0x03000000U);
    effect.flags |= 0x42000U;
    const auto prepared = TurboEmission::particle(effect);
    CHECK(prepared.node == "L_HAND");
    CHECK(prepared.offset == effect.offset);
    CHECK(prepared.previousEffect);
    CHECK(prepared.partnerFallback);
    const auto& descriptor = prepared.descriptor;
    CHECK(descriptor.texture == "RAIN");
    CHECK(descriptor.emitFrames == 5);
    CHECK(descriptor.fadeFrames == 1);
    CHECK(descriptor.angle == ParticleDescriptor::kSphere);
    CHECK(descriptor.rate == std::array<f32, 4>{2.5f, 2.5f, 2.5f, 2.5f});
    CHECK(descriptor.speed == Approx(-0.015f));
    CHECK(descriptor.particleLife ==
          ((effect.flags & effect_flags::kParticleMask) == 0x02000000U ? 6U : 15U));
    CHECK(descriptor.particleFade ==
          ((effect.flags & effect_flags::kParticleMask) == 0x02000000U ? 0U : 15U));
    CHECK(descriptor.width.lifeStart == 3);
    CHECK(descriptor.width.fadeEnd == 3);
    CHECK(descriptor.alpha.lifeStart == 255);
    CHECK(descriptor.alpha.fadeEnd == 255);
    CHECK_FALSE(descriptor.dynamic);
    CHECK_FALSE(descriptor.additive);
    CHECK(descriptor.depthWrite);
    effect.lifetime = -1;
    CHECK(TurboEmission::particle(effect).descriptor.emitFrames == ParticleDescriptor::kEndless);
    effect.lifetime = 0;
    CHECK(TurboEmission::particle(effect).descriptor.emitFrames == 1);
    effect.lifetime = 100000;
    CHECK(TurboEmission::particle(effect).descriptor.emitFrames == 65535);
}

TEST_CASE("turbo chain preparation preserves ordered cycles and non-damaging windows",
          "[game][turbo-emission]") {
    auto stats = authoredStrike();
    stats.moveStrikes[0].type = GENERATE(MoveStrike::kWindow, MoveStrike::kVolley);
    stats.moveEffects.emplace_back();
    stats.moveEffects[1].next = 0;
    stats.moveEffects[1].flags = 0x01040002U;
    const auto prepared = TurboEmission::prepare(stats, 0, {}, 10, 1);
    REQUIRE(prepared);
    CHECK_FALSE(prepared->carrier);
    REQUIRE(prepared->effects.size() == 3);
    CHECK(prepared->effects[0].row == 0);
    CHECK(prepared->effects[1].row == 1);
    CHECK(prepared->effects[2].row == 0);
    CHECK(prepared->effects[2].primary);
    CHECK(prepared->effects[1].particle.has_value());
    CHECK(prepared->effects[1].shake);
    CHECK_FALSE(prepared->effects[1].attachment);
    CHECK_FALSE(TurboEmission::prepare(stats, -1, {}, 10, 1));
    CHECK_FALSE(TurboEmission::prepare(stats, 1, {}, 10, 1));
}

TEST_CASE("turbo volleys retain ordinary native weapon modifiers but never fan or auto-aim",
          "[game][turbo-emission]") {
    PlayerMissiles::WeaponThrow weapon;
    weapon.owner = 3;
    weapon.character = 6;
    weapon.position = {1, 2, 3};
    weapon.center = {4, 5, 6};
    weapon.hand = {1, 2, 4};
    weapon.speed = 22;
    weapon.damage = 11;
    weapon.attackSeconds = 0.3f;
    weapon.scale = 2;
    weapon.shots = 5;
    weapon.flags = damage::kFiveWayShot | damage::kRapidFire | 4;
    weapon.target = Vec3{90, 3, 10};
    const u32 soundFlags = weapon.flags | damage::kSuperShot;
    const Vec3 direction{0.6f, 0, 0.8f};
    const auto prepared = TurboEmission::volley(weapon, direction, soundFlags);
    CHECK(prepared.soundFlags == soundFlags);
    CHECK_FALSE(prepared.spreads);
    CHECK(prepared.weapon.owner == weapon.owner);
    CHECK(prepared.weapon.character == weapon.character);
    CHECK(prepared.weapon.position == weapon.position);
    CHECK(prepared.weapon.center == weapon.center);
    CHECK(prepared.weapon.hand == weapon.hand);
    CHECK(prepared.weapon.speed == weapon.speed);
    CHECK(prepared.weapon.damage == weapon.damage);
    CHECK(prepared.weapon.attackSeconds == weapon.attackSeconds);
    CHECK(prepared.weapon.flags == weapon.flags);
    CHECK(prepared.weapon.multiplayer == weapon.multiplayer);
    CHECK(prepared.weapon.direction == direction);
    CHECK(prepared.weapon.scale == 1);
    CHECK(prepared.weapon.shots == 1);
    CHECK_FALSE(prepared.weapon.target);
    REQUIRE(PlayerMissiles::weaponLaunches(prepared.weapon).size() == 1);
    CHECK(weapon.shots == 5);
    CHECK(weapon.target.has_value());
}

struct NativeFixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 1> players;

    explicit NativeFixture(s32 character) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(classes.stats(character));
        const std::array<TextureSet*, 1> lenders{&weapons.textures};
        effects.setTextureLenders(lenders);
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        CharacterSave save;
        save.character = character;
        save.progress().experience = levelExperience(25);
        auto& player = players[0];
        player.actor.spawn(2, save, classes.stats(character), {3, 1, 5}, 0.37f);
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        REQUIRE(player.figure->effects());
        player.turbo.add(100);
    }
    NativeFixture(const NativeFixture&) = delete;
    NativeFixture& operator=(const NativeFixture&) = delete;
    ~NativeFixture() {
        attacks.clear();
        arsenal.clear();
        effects.clear();
    }
    ItemArchive* archive(std::string_view tree) {
        auto* own = players[0].figure->effects();
        if (own != nullptr && own->trees.find(tree)) {
            return own;
        }
        return weapons.trees.find(tree) ? &weapons : nullptr;
    }
    bool particleAvailable(std::string_view name) {
        const auto pictured = [&](const auto* candidate) {
            if (candidate == nullptr) {
                return false;
            }
            const auto slot = candidate->textures.find(name);
            return slot.has_value() && !candidate->textures.entry(*slot).external() &&
                   !candidate->textures.entry(*slot).noPicture;
        };
        return pictured(players[0].figure->effects()) || pictured(&weapons);
    }
};

TEST_CASE("Archer turbo arrows retain Super Shot and elemental artwork without spending charges",
          "[game][turbo-emission][turbo-item-visuals][assets]") {
    const auto deed = GENERATE(PlayerDeed::TurboStrong, PlayerDeed::TurboFull);
    const u32 flags = GENERATE(1U, 2U, 3U, 4U, damage::kSuperShot);
    NativeFixture fixture(3);
    auto& player = fixture.players[0];
    auto& inventory = player.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, flags, 7, -1);
    inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot, 0, 30);
    const auto held = inventory.powerups;
    usize observed = 0;
    for (s32 tick = 0; tick < 150; ++tick) {
        player.figure->animate(0, 2, 1.0f / 30, tick == 0 ? deed : PlayerDeed::None);
        const usize before = fixture.arsenal.missiles().count();
        fixture.attacks.updateTurbo(0, 2, 1.0f / 30, fixture.players, [](s32, usize) {});
        for (usize i = before; i < fixture.arsenal.missiles().count(); ++i) {
            ++observed;
            const auto& shot = fixture.arsenal.missiles().missile(i);
            CHECK(shot.flags == flags); // No five-way fan on each bonus arrow.
            REQUIRE(shot.streak.texture != nullptr);
            const auto& visuals = fixture.arsenal.missiles().visuals();
            const bool superShot = flags == damage::kSuperShot;
            CHECK(shot.spec == (superShot ? &MissileSpec::superShot() : &MissileSpec::of(3)));
            const u32 effect = superShot ? shot.effect : shot.rider;
            REQUIRE(effect != 0);
            bool found = false;
            for (usize n = 0; n < visuals.count(); ++n) {
                if (visuals.effect(n).id == effect) {
                    found = true;
                    CHECK(visuals.effect(n).name ==
                          (superShot ? "SUPERARROW" : WeaponGlow::throwTree(flags)));
                }
            }
            REQUIRE(found);
            if (superShot) {
                CHECK(shot.rider == 0);
                CHECK(shot.velocity.y == Approx(0));
                CHECK(shot.streak.color == Color::rgba(255, 255, 255, 190));
            }
        }
    }
    CHECK(observed > 0);
    CHECK(inventory.powerups == held);
}

TEST_CASE("authored turbo emissions match actual native dispatcher ordering and placement",
          "[game][turbo-emission][turbo-emission-native][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const auto deed = GENERATE(PlayerDeed::TurboStrong, PlayerDeed::TurboFull);
    NativeFixture fixture(character);
    auto& player = fixture.players[0];
    const auto* stats = fixture.classes.stats(character);
    REQUIRE(stats);
    const auto block = displayStats(*stats, experienceLevel(player.actor.save().experience()),
                                    player.actor.save().progress());
    const f32 damage = PlayerMissiles::damageFor(block.strength());
    usize observed = 0;
    for (s32 step = 0; step < 150; ++step) {
        CAPTURE(character, static_cast<s32>(deed), step);
        player.figure->animate(0, 2, 1.0f / 30, step == 0 ? deed : PlayerDeed::None);
        const auto& animator = player.figure->animator();
        auto timeline = player.move;
        auto meter = player.turbo;
        if (animator.turboBegan()) {
            timeline.begin(animator.action(), stats, meter);
        }
        const usize effectsBefore = fixture.effects.count();
        const usize strikesBefore = fixture.attacks.strikes().count();
        const usize shotsBefore = fixture.arsenal.missiles().count();
        const u32 texturesBefore = fixture.device.texturesCreated;
        const auto inventoryBefore = player.actor.save().progress().inventory.powerups;
        std::vector<TurboEmission::Strike> plans;
        std::vector<MissileLaunch> volleys;
        timeline.advance(
            animator.action(), animator.player().frame(), player.actor.facing(), stats, meter,
            {.announce = [](s32) {},
             .dim = [](f32) {},
             .volley =
                 [&](const Vec3& direction) {
                     const auto weapon =
                         fixture.arsenal.weaponThrow(player.actor, animator.attackSeconds());
                     REQUIRE(weapon);
                     auto emission = TurboEmission::volley(*weapon, direction, 0);
                     emission.weapon.hand = Vec3{0}; // PlayerStartMissile mode zero.
                     const auto launches = PlayerMissiles::weaponLaunches(emission.weapon);
                     REQUIRE(launches.size() == 1);
                     volleys.push_back(launches.front());
                 },
             .strike =
                 [&](s32 row) {
                     const auto& strike = stats->moveStrikes[static_cast<usize>(row)];
                     f32 duration = 0;
                     if (strike.effect >= 0 &&
                         static_cast<usize>(strike.effect) < stats->moveEffects.size()) {
                         const auto& effect = stats->moveEffects[static_cast<usize>(strike.effect)];
                         if (const auto* archive = fixture.archive(effect.tree)) {
                             duration = TurboEmission::effectSeconds(
                                 effect, &archive->trees.tree(*archive->trees.find(effect.tree)));
                         }
                     }
                     const auto emission = TurboEmission::prepare(
                         *stats, row, {character, player.actor.position(), player.actor.facing()},
                         damage, duration);
                     REQUIRE(emission);
                     plans.push_back(*emission);
                 }});
        // Preparing another copy neither pays the live meter nor starts sounds, artwork,
        // projectiles or damage carriers. Only the following native dispatcher does so.
        CHECK(fixture.effects.count() == effectsBefore);
        CHECK(fixture.attacks.strikes().count() == strikesBefore);
        CHECK(fixture.arsenal.missiles().count() == shotsBefore);
        CHECK(fixture.device.texturesCreated == texturesBefore);
        CHECK(player.actor.save().progress().inventory.powerups == inventoryBefore);
        fixture.attacks.updateTurbo(0, 2, 1.0f / 30, fixture.players, [](s32, usize) {});
        CHECK(player.move.checkpoint() == timeline.checkpoint());

        REQUIRE(fixture.arsenal.missiles().count() == shotsBefore + volleys.size());
        for (usize index = 0; index < volleys.size(); ++index) {
            const auto& actual = fixture.arsenal.missiles().missile(shotsBefore + index);
            vectorMatches(actual.position, volleys[index].position);
            const auto& launch = volleys[index];
            vectorMatches(actual.velocity,
                          launch.velocity.value_or(PlayerMissiles::launchVelocity(
                              launch.direction, launch.speed, launch.reach, launch.spec->weight)));
            CHECK(actual.damage == volleys[index].damage);
        }
        usize nextEffect = effectsBefore;
        usize nextStrike = strikesBefore;
        const Mat4 body = PlayerFigure::bodyPlacement(
            player.actor.transform(), player.actor.save(),
            PowerupEffects::of(player.actor.save().progress().inventory));
        for (const auto& plan : plans) {
            ++observed;
            if (plan.carrier) {
                REQUIRE(nextStrike < fixture.attacks.strikes().count());
                auto expected = *plan.carrier;
                // Native attached areas are placed after all their chained effects.
                for (const auto& effect : plan.effects) {
                    if (effect.attachment && effect.attachment->carrier) {
                        const auto& attachment = *effect.attachment;
                        const Mat4 parent =
                            attachment.bodyParent ? body : player.figure->rootAttachment(body);
                        const Mat4 placed = parent * attachment.local;
                        expected.position = Vec3{placed[3]};
                        const Vec3 forward{placed[2]};
                        const f32 length = std::hypot(forward.x, forward.z);
                        if (length > 0.0001f) {
                            expected.facing = Vec3{forward.x / length, 0, forward.z / length};
                        }
                    }
                }
                carrierMatches(fixture.attacks.strikes().strike(nextStrike++), expected);
            }
            for (const auto& emission : plan.effects) {
                const auto& authored = emission.authored;
                const bool available = emission.particle
                                           ? fixture.particleAvailable(authored.tree)
                                           : !authored.tree.empty() && authored.tree != "NULLFX" &&
                                                 fixture.archive(authored.tree) != nullptr;
                if (!available) {
                    continue;
                }
                CAPTURE(plan.row, emission.row, authored.tree);
                REQUIRE(nextEffect < fixture.effects.count());
                const auto& actual = fixture.effects.effect(nextEffect++);
                CHECK(actual.name == authored.tree);
                if (emission.particle) {
                    REQUIRE(actual.trails.size() == 1);
                    const auto& descriptor = actual.trails.emitter(0).descriptor();
                    const auto& expected = emission.particle->descriptor;
                    CHECK(descriptor.texture == expected.texture);
                    CHECK(descriptor.emitFrames == expected.emitFrames);
                    CHECK(descriptor.rate == expected.rate);
                    CHECK(descriptor.speed == expected.speed);
                    CHECK(descriptor.particleLife == expected.particleLife);
                    CHECK(descriptor.particleFade == expected.particleFade);
                    CHECK(descriptor.width.lifeStart == expected.width.lifeStart);
                    continue;
                }
                CHECK(actual.scale == emission.setting.scale);
                if (emission.setting.seconds > 0) {
                    REQUIRE(fixture.effects.remaining(actual.id));
                    CHECK(*fixture.effects.remaining(actual.id) ==
                          Approx(emission.setting.seconds));
                }
                vectorMatches(actual.velocity, emission.setting.velocity);
                CHECK(actual.then == emission.setting.then);
                CHECK(actual.morphIn == emission.setting.morphIn);
                CHECK(actual.holdForMorph == emission.setting.holdForMorph);
                CHECK(actual.shrinks == emission.setting.shrinks);
                CHECK(actual.tint.r == emission.setting.tint.r);
                CHECK(actual.tint.g == emission.setting.tint.g);
                CHECK(actual.tint.b == emission.setting.tint.b);
                CHECK(actual.light.has_value() == emission.setting.light.has_value());
                if (actual.light && emission.setting.light) {
                    vectorMatches(actual.light->color, emission.setting.light->color);
                    CHECK(actual.light->radius == emission.setting.light->radius);
                    CHECK(actual.light->swells == emission.setting.light->swells);
                }
                if (emission.attachment) {
                    REQUIRE(actual.attachment);
                    const auto& attachment = *emission.attachment;
                    const Mat4 parent =
                        attachment.bodyParent ? body : player.figure->rootAttachment(body);
                    matrixMatches(*actual.attachment, parent * attachment.local);
                } else {
                    CHECK_FALSE(actual.attachment);
                    vectorMatches(actual.position, emission.position);
                    CHECK(actual.yaw == emission.setting.yaw);
                }
            }
        }
        CHECK(nextEffect == fixture.effects.count());
        CHECK(nextStrike == fixture.attacks.strikes().count());
    }
    CHECK(observed > 0);
}
} // namespace
