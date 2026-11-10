#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"
#include "expansions/dreamcast/level/LegendsDreamcastContainers.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerHealth.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

ItemInfoRecord barrelDefinition() {
    ItemInfoRecord result;
    result.type = 2;
    result.subtype = 43;
    result.name = "BAROBJ";
    result.collisionType = 1;
    result.radius = 1;
    result.height = 2.5f;
    result.hitPoints = 5;
    result.armor = 1;
    result.activeType = 0x206;
    return result;
}

TEST_CASE("Dreamcast barrels use native bodies and stored health without changing N64 thresholds",
          "[legends][dreamcast-containers]") {
    ItemInstanceRecord instance;
    instance.position = {5, 2, 7};
    instance.rotation.y = .7f;
    const auto definition = legendsDreamcastContainer(barrelDefinition(), instance, 42);
    REQUIRE(definition);
    CHECK(definition->closedObstacle.centre == instance.position);
    CHECK(definition->closedObstacle.cylinderRadius == 1);
    CHECK(definition->closedObstacle.height == 2.5f);
    CHECK(definition->yaw == .7f);
    CHECK(definition->opening == "ACTIVE");
    CHECK(definition->broken == "DONE");
    instance.rotation.x = -8.88753e-8f;
    instance.rotation.z = -8.74107e-8f;
    REQUIRE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    instance.rotation.x = -.06654595f;
    instance.rotation.z = -.0761889f;
    const auto terrainAligned = legendsDreamcastContainer(barrelDefinition(), instance, 42);
    REQUIRE(terrainAligned);
    CHECK(terrainAligned->closedObstacle == definition->closedObstacle);
    instance.rotation.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    instance.rotation.x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    LegendsContainer barrel{*definition};
    for (s32 hit = 0; hit < 2; ++hit) {
        REQUIRE(barrel.strike(3));
        CHECK(barrel.targetable());
        CHECK(barrel.obstacle().solid);
        CHECK(barrel.state() == LegendsContainer::State::Closed);
    }
    REQUIRE(barrel.strike(3));
    CHECK(barrel.state() == LegendsContainer::State::Opening);
    CHECK_FALSE(barrel.obstacle().solid);
    CHECK_FALSE(barrel.strike(3));
    REQUIRE(barrel.finishOpening());

    auto nativeN64 = *definition;
    nativeN64.damageMode = LegendsContainerDefinition::DamageMode::Threshold;
    LegendsContainer oldMode{nativeN64};
    for (s32 hit = 0; hit < 10; ++hit) {
        CHECK_FALSE(oldMode.strike(4));
    }
    REQUIRE(oldMode.strike(5));
    LegendsContainer minimumDamage{*definition};
    for (s32 hit = 0; hit < 5; ++hit) {
        REQUIRE(minimumDamage.strike(.5f));
    }
    CHECK(minimumDamage.state() == LegendsContainer::State::Opening);
}

TEST_CASE("Dreamcast chest contact spends one key and holds collision until its animation finishes",
          "[legends][dreamcast-containers]") {
    auto info = barrelDefinition();
    info.subtype = 48;
    info.name = "CHESTS";
    info.activeType = 0x16;
    info.collisionType = 3;
    info.xSize = 1.4f;
    info.zSize = 1;
    const auto definition = legendsDreamcastContainer(info, {}, 7);
    REQUIRE(definition);
    CHECK(definition->closedObstacle.halfAcross == 1.4f);
    CHECK(definition->closedObstacle.halfAlong == 1);
    CHECK(definition->goldOpening == "CHESTSG");
    LegendsContainer chest{*definition};
    s32 keys = 2;
    REQUIRE(chest.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK_FALSE(chest.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK(chest.obstacle().solid);
    REQUIRE(chest.finishOpening());
    CHECK_FALSE(chest.obstacle().solid);
    auto deferred = *definition;
    deferred.interactive = false;
    LegendsContainer unavailable{deferred};
    CHECK_FALSE(unavailable.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK(unavailable.obstacle().solid);
    info.activeType = 0x42;
    std::string diagnostic;
    CHECK_FALSE(legendsDreamcastContainer(info, {}, 7, &diagnostic));
    CHECK_FALSE(diagnostic.empty());
}

TEST_CASE("Dreamcast contents resolve nested choices at opening using a shared native cursor",
          "[legends][dreamcast-containers]") {
    std::array<ItemInfoRecord, 5> rows;
    rows[0].type = rows[2].type = ItemInfoRecord::kChoiceList;
    rows[0].subtype = rows[2].subtype = 2;
    rows[0].choices = {1, 2};
    rows[2].choices = {3, 4};
    rows[1].type = rows[3].type = rows[4].type = 1;
    LegendsDreamcastLoot loot{rows, 255};
    CHECK(loot.outcomes(0) == std::vector<s16>{1, 3, 4});
    CHECK(loot.seed() == 255);
    CHECK(loot.select(0, 0) == 4); // 7%2 -> nested row; 21%2 -> final leaf.
    CHECK(loot.seed() == 255 + 439 * 2);
    const auto prior = loot.seed();
    CHECK(loot.select(1, 5) == 1);
    CHECK_FALSE(loot.select(-1, 5));
    CHECK(loot.seed() == prior);
    LegendsDreamcastLoot at30{rows, 255};
    LegendsDreamcastLoot at60{rows, 255};
    for (s32 tick = 0; tick < 90; ++tick) {
        at30.advance(1.0f / 30);
        at60.advance(1.0f / 60);
        at60.advance(1.0f / 60);
    }
    CHECK(at30.seed() == 345);
    CHECK(at60.seed() == at30.seed());
    CHECK(at30.select(0, 19) == at60.select(0, 19));
    rows[2].choices = {0, 4};
    LegendsDreamcastLoot cycle{rows, 255};
    CHECK_THROWS_AS(cycle.outcomes(0), FormatError);
    CHECK_THROWS_AS(cycle.select(0, 0), FormatError);
    CHECK(cycle.seed() == 255);
    rows[2].choices = {8, 4};
    CHECK_THROWS_AS(LegendsDreamcastLoot{rows}.outcomes(0), FormatError);
}

TEST_CASE("Every extracted Dreamcast ordinary container retains a native body and valid loot graph",
          "[legends][dreamcast-containers][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Dreamcast extraction is not installed");
    }
    usize barrels = 0;
    usize chests = 0;
    usize deathContents = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto path = entry.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto bytes = readFile(path);
        if (bytes.size() < 96) {
            continue;
        }
        const auto native = LegendsDreamcastScene::parse(bytes);
        const LegendsDreamcastLoot loot{native.itemInfos};
        for (usize index = 0; index < native.itemInstances.size(); ++index) {
            const auto& placement = native.itemInstances[index];
            const auto& info = native.itemInfos[static_cast<usize>(placement.info)];
            if (info.type != 2) {
                continue;
            }
            CAPTURE(path, index, info.subtype, info.name);
            std::string diagnostic;
            const auto definition =
                legendsDreamcastContainer(info, placement, static_cast<u32>(index), &diagnostic);
            CAPTURE(diagnostic);
            REQUIRE(definition);
            CHECK(definition->position == placement.position);
            if (definition->kind == LegendsContainerDefinition::Kind::Barrel) {
                ++barrels;
            } else {
                ++chests;
            }
            for (const auto outcome :
                 loot.outcomes(std::bit_cast<s16>(readU16LE(placement.params, 0)))) {
                deathContents += loot.definition(outcome).type == 4 ? 1 : 0;
            }
        }
    }
    CHECK(barrels == 610);
    CHECK(chests == 495);
    CHECK(deathContents >= 48); // Do not accidentally reinterpret Death as an empty pickup.
}

TEST_CASE("Dreamcast native barrel and gold chest sequences release one bound inventory offer",
          "[legends][dreamcast-containers][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto native = LegendsDreamcastScene::parse(bytes);
    const auto animations = AnimationSet::fromDecoded(parseDreamcastAnimation(
        readFile(directory.parent_path().parent_path() / "items/levelb/anim.rom")));
    const auto objects = readFile(directory / "objects.rom");
    auto map = LegendsMap::parseDreamcast(bytes, objects);
    map.textures = decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    const auto textures = device.texturesCreated;
    bool barrel = false;
    bool chest = false;
    for (const auto& target : scene->containerTargets()) {
        const auto& instance = native.itemInstances[target.nativeIndex];
        const auto& info = native.itemInfos[static_cast<usize>(instance.info)];
        if (info.type != 2) {
            continue;
        }
        const bool isBarrel = info.subtype == 43;
        if ((isBarrel && barrel) || (!isBarrel && chest)) {
            continue;
        }
        const s16 contents = std::bit_cast<s16>(readU16LE(instance.params, 0));
        if (contents < 0) {
            continue;
        }
        const auto& leaf = native.itemInfos[static_cast<usize>(contents)];
        if (leaf.type != 1 || leaf.subtype != 1) {
            continue;
        }
        const auto offered = std::ranges::find_if(scene->pickups(), [&](const auto& pickup) {
            return pickup.nativeIndex == target.nativeIndex && !pickup.active &&
                   pickup.offer.kind == static_cast<s32>(ItemKind::Gold);
        });
        REQUIRE(offered != scene->pickups().end());
        const usize pickupIndex = static_cast<usize>(offered - scene->pickups().begin());
        const auto treeIndex = animations.find(info.name);
        REQUIRE(treeIndex);
        const auto& tree = animations.tree(*treeIndex);
        const auto active = tree.findSequence("ACTIVE");
        REQUIRE(active);
        AnimationPlayer nativeClock;
        nativeClock.start(tree.sequences[*active], *active);
        CharacterSave save;
        save.progress().inventory.keys = 2;
        PlayerActor actor;
        actor.spawn(0, save, nullptr, target.base, 0);
        if (isBarrel) {
            for (s32 hit = 0; hit < 4; ++hit) {
                REQUIRE(scene->strikeContainer(target.id, 2, 0, 0));
                CHECK(std::ranges::any_of(scene->containerObstacles(), [&](const auto& body) {
                    return body.centre == target.base;
                }));
            }
            REQUIRE(scene->strikeContainer(target.id, 2, 0, 0));
            barrel = true;
        } else {
            (void)scene->collect(actor, 1, false);
            CHECK(actor.save().progress().inventory.keys == 1);
            chest = true;
        }
        CHECK_FALSE(scene->pickups()[pickupIndex].active);
        CHECK(actor.save().gold == 0);
        s32 ticks = 0;
        while (!scene->pickups()[pickupIndex].active && ticks < 180) {
            CHECK(std::ranges::any_of(scene->containerObstacles(), [&](const auto& body) {
                      return body.centre == target.base;
                  }) == !isBarrel);
            scene->capturePresentation();
            scene->update(1.0f / 60);
            nativeClock.advance(1.0f / 60, false);
            CHECK(scene->pickups()[pickupIndex].active == nativeClock.finished());
            ++ticks;
        }
        REQUIRE(ticks > 1);
        REQUIRE(scene->pickups()[pickupIndex].active);
        CHECK_FALSE(std::ranges::any_of(scene->containerObstacles(), [&](const auto& body) {
            return body.centre == target.base;
        }));
        (void)scene->collect(actor, 1, false);
        CHECK(actor.save().gold == leaf.value);
        (void)scene->collect(actor, 1, false);
        CHECK(actor.save().gold == leaf.value);
        CHECK(device.texturesCreated == textures);
    }
    CHECK(barrel);
    CHECK(chest);
}

TEST_CASE(
    "Dreamcast empty and explosive trap barrels keep collision and emit distinct native blasts",
    "[legends][dreamcast-containers][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto native = LegendsDreamcastScene::parse(bytes);
    const auto objects = readFile(directory / "objects.rom");
    auto map = LegendsMap::parseDreamcast(bytes, objects);
    map.textures = decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    const auto soloTargets = scene->containerTargets();
    std::array<usize, 3> soloCounts{};
    for (const auto& target : soloTargets) {
        const auto& item = native.itemInstances[target.nativeIndex];
        const auto& info = native.itemInfos[static_cast<usize>(item.info)];
        if (info.type == 10 && info.subtype >= 43 && info.subtype <= 45) {
            ++soloCounts[static_cast<usize>(info.subtype - 43)];
        }
    }
    CHECK(soloCounts == std::array<usize, 3>{3, 0, 1});
    scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    const auto initialTargets = scene->containerTargets();
    const auto initialPickups = scene->pickups().size();
    std::array<usize, 3> counts{};
    for (const auto& target : initialTargets) {
        const auto& instance = native.itemInstances[target.nativeIndex];
        const auto& info = native.itemInfos[static_cast<usize>(instance.info)];
        if (info.type != 10 || info.subtype < 43 || info.subtype > 45) {
            continue;
        }
        CAPTURE(target.nativeIndex, info.name);
        REQUIRE(target.breakable);
        REQUIRE(std::ranges::any_of(scene->containerObstacles(),
                                    [&](const auto& body) { return body.centre == target.base; }));
        ++counts[static_cast<usize>(info.subtype - 43)];
        REQUIRE(scene->strikeContainer(target.id, 100, 0, 0));
        CHECK_FALSE(scene->strikeContainer(target.id, 100, 0, 0));
        CHECK_FALSE(std::ranges::any_of(scene->containerObstacles(), [&](const auto& body) {
            return body.centre == target.base;
        }));
        const auto blasts = scene->takeFixtureEvents();
        const auto sounds = scene->takeSounds();
        REQUIRE(sounds.size() == 1);
        CHECK(sounds.front().token == (info.subtype == 43   ? 0x210024U
                                       : info.subtype == 44 ? 0x210026U
                                                            : 0x210025U));
        if (info.subtype == 43) {
            CHECK(blasts.empty());
        } else {
            REQUIRE(blasts.size() == 1);
            const bool poison = info.subtype == 45;
            CHECK(blasts.front().kind == (poison ? LegendsFixtureEvent::Kind::Poison
                                                 : LegendsFixtureEvent::Kind::Explosion));
            CHECK(blasts.front().radius == (poison ? 8 : 6));
            CHECK(blasts.front().damage == 30);
            CHECK(blasts.front().position == target.base + Vec3{0, poison ? -1.0f : 2.0f, 0});
            CHECK(blasts.front().seconds == (poison ? 4.0f : 1.0f));
        }
    }
    CHECK(counts == std::array<usize, 3>{5, 2, 1});
    CHECK(scene->pickups().size() == initialPickups);
    for (s32 tick = 0; tick < 300; ++tick) {
        scene->update(1.0f / 30);
    }
    CHECK(scene->takeFixtureEvents().empty());
}

TEST_CASE("Dreamcast poison barrels damage grounded and levitating players through normal health",
          "[legends][dreamcast-containers][dreamcast-poison][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto world = readFile(directory / "worlds.rom");
    const auto native = LegendsDreamcastScene::parse(world);
    const auto objects = readFile(directory / "objects.rom");
    auto map = LegendsMap::parseDreamcast(world, objects);
    map.textures = decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    LegendsLevel level{device, map, std::move(adapter), directory, test::dataDirectory()};
    const auto targets = level.adapter().containerTargets();
    const auto barrel = std::ranges::find_if(targets, [&](const auto& target) {
        const auto& instance = native.itemInstances[target.nativeIndex];
        const auto& info = native.itemInfos[static_cast<usize>(instance.info)];
        return info.type == 10 && info.subtype == 45;
    });
    REQUIRE(barrel != targets.end());
    CAPTURE(barrel->nativeIndex, barrel->base.x, barrel->base.y, barrel->base.z);
    PlayerRuntime player;
    player.actor.spawn(2, {}, nullptr, barrel->base, 0);
    player.actor.save().progress().health = 1000;
    auto& inventory = player.actor.save().progress().inventory;
    s32 expectedHealth = 970;
    bool protectedByArmor = false;
    SECTION("ordinary grounded player") {}
    SECTION("Levitation does not make poison gas harmless") {
        inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, -1);
        // The effect must overlap the lifted body, not merely the stored floor.
        player.actor.place(barrel->base + Vec3{0, 2, 0});
    }
    SECTION("gas immunity still protects the player") {
        inventory.addPowerup(powerup::kArmor, armor::kImmuneGas, 0, -1);
        expectedHealth = 1000;
        protectedByArmor = true;
    }
    SECTION("silver invulnerability still protects the player") {
        inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
        expectedHealth = 1000;
        protectedByArmor = true;
    }
    SECTION("gold invulnerability retains the GDL healing mechanic") {
        inventory.addPowerup(powerup::kArmor, armor::kGoldInvulnerable, 0, -1);
        expectedHealth = 1003;
    }
    std::array<PlayerActor*, 1> party{&player.actor};
    level.updateParty(party, 0, false);
    REQUIRE(level.strike(barrel->id, 100, 0, 2));
    PlayerHealth health;
    const PlayerHealth::Events feedback{.block = [](f32, f32) {},
                                        .sound = [](std::string_view) {},
                                        .cry = [](std::string_view) {},
                                        .named = [](std::string_view, f32) {},
                                        .learnBlock = {},
                                        .vibrate = {}};
    usize contacts = 0;
    const auto contact = [&] {
        level.affectArea({});
        for (const auto& hit : level.takeHazardHits()) {
            if (!hit.poison) {
                continue;
            }
            REQUIRE(hit.player == 2);
            CHECK(hit.damage == 30);
            CHECK(hit.impact.flags == damage::kGas);
            health.hurt(player, hit.damage, HurtKind::Gas, hurtSourceOf(hit.impact.flags), {},
                        feedback, hit.impact);
            ++contacts;
        }
    };
    contact();
    CHECK(contacts == 0); // The native blast starts before its visible radius grows.
    for (s32 tick = 0; tick < 60; ++tick) {
        level.update(1.0f / 60, false);
        level.updateParty(party, 1.0f / 60, false);
        contact();
    }
    REQUIRE(contacts == 1);
    CHECK(player.actor.save().health() == expectedHealth);
    if (!protectedByArmor && expectedHealth < 1000) {
        CHECK(player.gagSeconds > 0);
    }
    // DC ProcessEffects 8c0d4400/49aa gates further hits until the
    // remaining effect duration plus 0.066667s. It does NOT have GDL's
    // recurring 0.5s gas branch. Removing immunity cannot manufacture a retry.
    for (auto& item : inventory.powerups) {
        item.on = false;
    }
    for (s32 tick = 0; tick < 240; ++tick) {
        level.update(1.0f / 60, false);
        level.updateParty(party, 1.0f / 60, false);
        contact();
    }
    CHECK(contacts == 1);
    CHECK(player.actor.save().health() == expectedHealth);
    PlayerActor lateArrival;
    lateArrival.spawn(1, {}, nullptr, barrel->base, 0);
    std::array<PlayerActor*, 1> lateParty{&lateArrival};
    level.updateParty(lateParty, 0, false);
    level.affectArea({});
    const auto expiredHits = level.takeHazardHits();
    CHECK(std::ranges::none_of(expiredHits, [](const auto& hit) { return hit.poison; }));
}

TEST_CASE("Dreamcast barrel blasts stay where they went off while the barrel rides on",
          "[legends][dreamcast-containers][dreamcast-motion][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/leveld3";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    struct Barrel {
        u32 nativeIndex;
        u32 pad; ///< the native switch whose target carries the barrel's support
        std::string_view blast;
        Vec3 offset;
    };
    // D3's explosive #394 and poison #390 stand on supports that pads 196 and 242 set moving.
    const std::array barrels{Barrel{394, 196, "EXPLOSION", {0, 2, 0}},
                             Barrel{390, 242, "POISONEXP", {0, -1, 0}}};
    const auto bytes = readFile(directory / "worlds.rom");
    const auto objects = readFile(directory / "objects.rom");
    const auto native = LegendsDreamcastScene::parse(bytes);
    for (const auto& barrel : barrels) {
        CAPTURE(barrel.nativeIndex);
        auto map = LegendsMap::parseDreamcast(bytes, objects);
        test::FakeRenderDevice device;
        auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
        const auto* motion = scene->dreamcastWorldMotion();
        REQUIRE(motion);
        const auto targets = scene->containerTargets();
        const auto target =
            std::ranges::find(targets, barrel.nativeIndex, &LegendsContainerTarget::nativeIndex);
        REQUIRE(target != targets.end());
        REQUIRE(target->nativeTrap);
        const auto support = motion->supportAt(target->base);
        REQUIRE(support);
        REQUIRE(motion->carriesItems(support->object));
        const auto switches = motion->switches();
        const auto pad = std::ranges::find(switches, barrel.pad,
                                           &LegendsDreamcastWorldMotion::Switch::nativeIndex);
        REQUIRE(pad != switches.end());
        const auto padIndex = static_cast<usize>(pad - switches.begin());
        const auto& name =
            native.itemInfos[static_cast<usize>(native.itemInstances[barrel.nativeIndex].info)]
                .name;
        // The barrel's own figure, which is its debris too, and its hidden blast.
        std::optional<s32> body;
        std::optional<s32> blast;
        for (usize index = 0; index < scene->fixtureCount(); ++index) {
            const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
            const auto figure = scene->fixtureObject(id);
            if (figure && figure->name == barrel.blast &&
                glm::distance(figure->position, target->base + barrel.offset) < .001f) {
                blast = id;
            }
            if (figure && figure->name == name &&
                glm::distance(figure->position, target->base) < .001f) {
                body = id;
            }
        }
        REQUIRE(body);
        REQUIRE(blast);
        CHECK_FALSE(scene->fixtureObject(*blast)->visible);
        const auto press = [&] {
            const LegendsPortalVisitor visitor{.position = motion->switchPosition(padIndex),
                                               .floorObject = pad->floor};
            scene->touchSwitches(std::array{visitor});
            scene->capturePresentation();
            scene->update(1.0f / 30);
        };
        Mat4 carried = motion->riderTransform(support->object);
        for (s32 tick = 0; tick < 300 && motion->riderTransform(support->object) == carried;
             ++tick) {
            press();
        }
        carried = motion->riderTransform(support->object);
        REQUIRE(carried != Mat4{1});
        // Set off on the moving support, the blast is born where the barrel stands.
        REQUIRE(scene->strikeContainer(target->id, 100, 0, 0));
        const auto born = scene->fixtureObject(*blast);
        REQUIRE(born);
        CHECK(born->visible);
        CHECK(glm::distance(born->position, Vec3{carried * Vec4{target->base, 1}} + barrel.offset) <
              .001f);
        for (s32 tick = 0; tick < 30; ++tick) {
            press();
            CAPTURE(tick);
            CHECK(glm::distance(scene->fixtureObject(*blast)->position, born->position) < .0001f);
        }
        // The support went on moving through the blast and carried the barrel's debris with it.
        const Mat4 now = motion->riderTransform(support->object);
        REQUIRE(glm::distance(Vec3{now * Vec4{target->base, 1}},
                              Vec3{carried * Vec4{target->base, 1}}) > .01f);
        CHECK(glm::distance(scene->fixtureObject(*body)->position,
                            Vec3{now * Vec4{target->base, 1}}) < .001f);
    }
}
} // namespace
