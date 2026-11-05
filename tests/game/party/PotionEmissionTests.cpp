#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PotionEmission.h"
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
        actor.spawn(2, {}, nullptr, {10, 2, 20}, 0);
        actor.faceToward(actor.position() + Vec3{0.6f, 0, 0.8f});
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
    }
};

constexpr std::array<std::string_view, 5> kBottles{"POT_RED_TW", "POT_RED_TW", "POT_BLU_TW",
                                                   "POT_YEL_TW", "POT_GRE_TW"};
constexpr std::array<std::string_view, 5> kBursts{"MP_FIRE", "MP_FIRE", "MP_ELEC", "MP_LIGHT",
                                                  "MP_ACID"};
constexpr std::array<std::string_view, 5> kSounds{"S_POTION2", "S_POTION2", "S_POTION1",
                                                  "S_POTION3", "S_POTION4"};
constexpr std::array<Vec3, 5> kLights{Vec3{2, 0, 0}, Vec3{2, 0, 0}, Vec3{1}, Vec3{2, 2, 0},
                                      Vec3{0, 2, 0}};

void checkImpact(const MissileImpact& actual, const MissileImpact& expected) {
    CHECK(actual.position == expected.position);
    CHECK(actual.owner == expected.owner);
    CHECK(actual.potion == expected.potion);
    CHECK(actual.potency == expected.potency);
    CHECK(actual.damage == expected.damage);
    CHECK(actual.flags == expected.flags);
    CHECK(actual.target == expected.target);
    CHECK(actual.worldObject == expected.worldObject);
    CHECK(actual.liquid == expected.liquid);
}

TEST_CASE("potion shields keep their authored dome height at every magic power",
          "[game][world][potion-emission]") {
    const f32 radius = GENERATE(0.0f, 2.0f, 4.0f, 8.0f, 12.0f, 20.0f);
    const auto scale = PotionEmission::shieldScale(radius);
    CHECK(scale.x == Approx(std::clamp(radius / 12.0f, 0.33f, 1.0f)));
    CHECK(scale.y == 1.0f);
    CHECK(scale.z == scale.x);
}

TEST_CASE("pure potion descriptors preserve native colour artwork and independent harm power",
          "[game][world][potion-emission]") {
    const s32 kind = GENERATE(0, 1, 2, 3, 4);
    const f32 power = GENERATE(0.0f, 16.0f, 32.0f, 64.0f);
    const f32 bonus = GENERATE(1.0f, 1.1f);
    const PotionEmission::Context context{3,     {10, 2, 20}, {0.6f, 0, 0.8f}, kind,
                                          power, bonus,       damage::kHeal};
    const auto burst = PotionEmission::burst(context);
    const auto& look = PotionEmission::look(kind);
    CHECK(look.bottle == kBottles[static_cast<usize>(kind)]);
    CHECK(look.burst == kBursts[static_cast<usize>(kind)]);
    CHECK(look.sound == kSounds[static_cast<usize>(kind)]);
    CHECK(burst.tree == look.burst);
    CHECK(burst.sound == look.sound);
    CHECK(burst.impact.position == context.position);
    CHECK(burst.impact.owner == 3);
    CHECK(burst.impact.potion == kind); // even zero is not silently resolved by preparation
    CHECK(burst.impact.potency == power);
    CHECK(burst.impact.damage == 40.0f * bonus);
    CHECK(burst.impact.flags == damage::kHeal);
    CHECK(burst.impact.target == -1);
    CHECK(burst.impact.worldObject == -1);
    CHECK_FALSE(burst.impact.liquid);
    EffectTrees::Setting expected;
    expected.scale = std::min(power / 32, 1.0f);
    expected.light = EffectTrees::Light{kLights[static_cast<usize>(kind)], 1.5f * power};
    CHECK(burst.setting == expected);
    for (const s32 fallback : {-1, 5, 99}) {
        CHECK(PotionEmission::look(fallback).bottle == kBottles[0]);
        CHECK(PotionEmission::look(fallback).burst == kBursts[0]);
        CHECK(PotionEmission::look(fallback).sound == kSounds[0]);
    }
}

TEST_CASE("prepared potion bottles share exact native hand ballistic and damage parameters",
          "[game][world][potion-emission]") {
    const s32 held = GENERATE(-3, 0, 1, 8, 30);
    const s32 kind = GENERATE(0, 1, 2, 3, 4);
    const PotionEmission::Context context{2,  {10, 2, 20}, {0.6f, 0, 0.8f}, kind,
                                          22, 1.1f,        damage::kHeal};
    const auto launch = PotionEmission::throwPotion(context, held);
    CHECK(launch.owner == 2);
    CHECK(launch.position.x == Approx(11.2f));
    CHECK(launch.position.y == 6);
    CHECK(launch.position.z == Approx(21.6f));
    CHECK(launch.direction == context.facing);
    const f32 speed = 5 + 1.5f * static_cast<f32>(std::max(held, 0));
    REQUIRE(launch.velocity);
    CHECK(*launch.velocity == Vec3{0.6f * 0.707f, 0.707f, 0.8f * 0.707f} * speed);
    CHECK(launch.potion == kind);
    CHECK(launch.potency == 16.5f);
    CHECK(launch.damage == 40.0f * 1.1f);
    CHECK(launch.flags == damage::kHeal);
    CHECK(launch.spec == &MissileSpec::potion());
    CHECK(launch.spec->weight == 100);
    CHECK(launch.spec->radius == 0.5f);
    CHECK(launch.tree == kBottles[static_cast<usize>(kind)]);
    CHECK(launch.model == nullptr);
    CHECK(launch.archive == nullptr);
    CHECK(launch.textureLender == nullptr);
    CHECK(launch.riderArchive == nullptr);
    CHECK(launch.riderTree.empty());
    CHECK(launch.streak.texture == nullptr);
    CHECK_FALSE(launch.release);
}

TEST_CASE("potion preparation neither spends inventory nor advances the shared colour cycle",
          "[game][world][potion-emission]") {
    Fixture fixture;
    auto& inventory = fixture.actor.save().progress().inventory;
    inventory.potions = {2, 0};
    inventory.addPowerup(powerup::kMagic, 0, 7, 30);
    fixture.actor.save().progress().health = 123;
    const auto before = inventory;
    const auto textures = fixture.device.texturesCreated;
    const PotionEmission::Context context{
        fixture.actor.player(), fixture.actor.position(), fixture.actor.facing(), 4, 25, 1, 0};
    for (s32 tick = 0; tick < 120; ++tick) {
        const auto burst = PotionEmission::burst(context);
        const auto launch = PotionEmission::throwPotion(context, tick);
        CHECK(burst.impact.potion == 4);
        CHECK(launch.potion == 4);
    }
    CHECK(inventory == before);
    CHECK(fixture.actor.save().health() == 123);
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.arsenal.missiles().count() == 0);
    CHECK(fixture.effects.count() == 0);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 1);
    fixture.arsenal.clear();
    CHECK_FALSE(fixture.arsenal.usePotion(fixture.actor));
    fixture.arsenal.throwPotion(fixture.actor, 10);
    CHECK(inventory == before); // live dispatch still requires its bound services
    CHECK(fixture.arsenal.missiles().count() == 0);
}

TEST_CASE(
    "native potion release uses prepared descriptors after actual class colour and level power",
    "[game][world][potion-emission][assets]") {
    Fixture fixture;
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    REQUIRE(fixture.classes.load(root / "pdata"));
    constexpr std::array<s32, 4> kElements{3, 2, 1, 4};
    s32 cycle = 1;
    const auto resolve = [&](s32 stored) {
        if (stored != 0) {
            return stored;
        }
        const s32 result = cycle;
        cycle = cycle % 4 + 1;
        return result;
    };
    for (const s32 character : {0, 2, 6}) {
        fixture.actor.save().character = character;
        const auto* stats = fixture.classes.stats(character);
        REQUIRE(stats != nullptr);
        auto& progress = fixture.actor.save().progress();
        progress.magicAdd = 17;
        progress.health = 321;
        progress.inventory.addPowerup(powerup::kMagic, 0, 3.0f, 30);
        for (const s32 level : {1, 24, 25, 75, 99}) {
            progress.experience = levelExperience(level);
            const s32 stat = displayStats(*stats, level, progress).magic();
            const f32 magic = PowerupEffects::of(progress.inventory).magicPower(stat);
            CHECK(fixture.arsenal.magicPowerOf(fixture.actor) == magic);
            for (s32 colour = 0; colour < 4; ++colour) {
                fixture.actor.save().color = colour;
                for (s32 stored = 0; stored <= 4; ++stored) {
                    CAPTURE(character, level, colour, stored);
                    const f32 bonus = stored == kElements[static_cast<usize>(colour)] ? 1.1f : 1;
                    const f32 power = magic * bonus;
                    const u32 flags = level >= 25 ? damage::kHeal : 0;
                    progress.inventory.potions = {2, stored};
                    const PotionEmission::Context context{fixture.actor.player(),
                                                          fixture.actor.position(),
                                                          fixture.actor.facing(),
                                                          resolve(stored),
                                                          power,
                                                          bonus,
                                                          flags};
                    const auto expected = PotionEmission::burst(context);
                    const auto actual = fixture.arsenal.usePotion(fixture.actor);
                    REQUIRE(actual);
                    checkImpact(*actual, expected.impact);
                    CHECK(progress.inventory.potions == std::vector<s32>{2});
                    CHECK(fixture.actor.save().health() == 321);
                    progress.inventory.potions = {2, stored};
                    auto throwContext = context;
                    throwContext.resolvedKind = resolve(stored);
                    const auto prepared = PotionEmission::throwPotion(throwContext, 8);
                    fixture.arsenal.throwPotion(fixture.actor, 8);
                    REQUIRE(fixture.arsenal.missiles().count() == 1);
                    const auto& shot = fixture.arsenal.missiles().missile(0);
                    CHECK(shot.owner == prepared.owner);
                    CHECK(shot.position == prepared.position);
                    CHECK(shot.velocity == *prepared.velocity);
                    CHECK(shot.potion == prepared.potion);
                    CHECK(shot.potency == prepared.potency);
                    CHECK(shot.damage == prepared.damage);
                    CHECK(shot.flags == prepared.flags);
                    CHECK(shot.spec == prepared.spec);
                    CHECK(shot.model != nullptr);
                    CHECK(progress.inventory.potions == std::vector<s32>{2});
                    CHECK(fixture.actor.save().health() == 321);
                    fixture.arsenal.missiles().clear();
                }
            }
        }
    }
    CHECK(fixture.arsenal.resolvePotionKind(0) == cycle);
}

TreeInfo potionTree() {
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo root;
    root.name = "L1ROOT";
    root.type = 1;
    tree.nodes.push_back(root);
    for (const auto& [name, frames] :
         {std::pair{"READY", 60}, std::pair{"THROWPOTIONS", 11}, std::pair{"THROWPOTIONR", 9},
          std::pair{"MAGICS", 11}, std::pair{"MAGICR", 15}, std::pair{"DEATH", 20}}) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = frames;
        sequence.frameRate = 30;
        tree.sequences.push_back(sequence);
    }
    return tree;
}

TEST_CASE("native potion cancellation spends nothing until the release event and cannot refund it",
          "[game][world][potion-emission]") {
    const bool thrown = GENERATE(false, true);
    const bool cancelBefore = GENERATE(false, true);
    Fixture fixture;
    fixture.actor.save().progress().inventory.potions = {0};
    const auto tree = potionTree();
    PlayerAnimator animator;
    REQUIRE(animator.bind(tree, Entrance::Skip));
    u32 emissions = 0;
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick, thrown, cancelBefore);
        const bool cancel = cancelBefore ? tick >= 3 : emissions != 0;
        const auto requested = thrown ? PlayerDeed::ThrowPotion : PlayerDeed::UsePotion;
        animator.update(PlayerMotion::Stand, 1, 1.0f / 60, cancel ? PlayerDeed::Die : requested);
        // The normal native action owner, not descriptor preparation, spends the potion.
        if (animator.potionThrown()) {
            fixture.arsenal.throwPotion(fixture.actor, animator.potionThrowTicks());
            ++emissions;
        } else if (animator.potionUsed()) {
            REQUIRE(fixture.arsenal.usePotion(fixture.actor));
            ++emissions;
        }
    }
    CHECK(emissions == (cancelBefore ? 0 : 1));
    CHECK(fixture.actor.save().progress().inventory.potions.size() == (cancelBefore ? 1 : 0));
    CHECK(fixture.arsenal.missiles().count() == (!cancelBefore && thrown ? 1 : 0));
    CHECK(fixture.arsenal.resolvePotionKind(0) == (cancelBefore ? 1 : 2));
}

TEST_CASE("native potion bursts preserve authored effects and exactly one impact-time sound",
          "[game][world][potion-emission][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture fixture;
    REQUIRE(fixture.weapons.load(root / "WEAPONS"));
    fixture.audio.open(root, &sounds, nullptr);
    fixture.arsenal.bind({fixture.device,
                          fixture.classes,
                          fixture.weapons,
                          fixture.collision,
                          fixture.effects,
                          fixture.audio,
                          &sounds,
                          {}});
    SoundSet common;
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    std::vector<SoundPlayer::Event> played;
    const SoundPlayer::Capture capture(&sounds, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            played.push_back(event);
        }
    });
    for (s32 kind = 1; kind <= 4; ++kind) {
        CAPTURE(kind);
        played.clear();
        fixture.effects.clear();
        const PotionEmission::Context context{2, {10, 2, 20}, {0, 0, 1}, kind, 16, 1, 0};
        const auto expected = PotionEmission::burst(context);
        fixture.arsenal.burstPotion(kind, context.position, context.power);
        REQUIRE(fixture.effects.count() == 1);
        const auto& effect = fixture.effects.effect(0);
        CHECK(effect.name == expected.tree);
        CHECK(effect.position == expected.impact.position);
        CHECK(effect.scale == expected.setting.scale);
        CHECK(effect.light == expected.setting.light);
        const auto call = common.find(expected.sound);
        REQUIRE(call);
        REQUIRE(played.size() == 1);
        CHECK(played.front().bank == common.sequence(*call).bank);
        CHECK(played.front().call == *call);
        played.clear();
        fixture.actor.save().progress().inventory.potions = {kind};
        fixture.arsenal.throwPotion(fixture.actor, 8);
        CHECK(played.empty()); // unlike an immediate cast, the bottle is silent at release
        fixture.arsenal.missiles().clear();
        auto impact = expected.impact;
        fixture.arsenal.presentImpact(impact);
        REQUIRE(played.size() == 1);
        CHECK(played.front().call == *call);
        played.clear();
        impact.liquid = true;
        fixture.arsenal.presentImpact(impact);
        const auto splash = common.find("S_SPLASH");
        REQUIRE(splash);
        REQUIRE(played.size() == 1);
        CHECK(played.front().bank == common.sequence(*splash).bank);
        CHECK(played.front().call == *splash);
    }
    fixture.arsenal.clear();
    fixture.effects.clear();
    fixture.audio.close();
    sounds.stopAll();
}
} // namespace
