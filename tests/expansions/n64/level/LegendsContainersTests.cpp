#include <algorithm>
#include <bit>
#include <filesystem>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/level/LegendsContainers.h"
#include "expansions/shared/level/LegendsItems.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value >> 8U);
    bytes[at + 1] = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}
struct NativeTables {
    LegendsN64Fixture fixture;
    std::vector<u8> containers = std::vector<u8>(48);
    std::vector<u8> powerups = std::vector<u8>(32);
    NativeTables() {
        fixture.kind = LegendsN64FixtureKind::Container;
        fixture.templateKey = 0x1901;
        fixture.nativeIndex = 7;
        fixture.position = {5, 2, 9};
        fixture.facingRadians = .7f;
        fixture.containedPowerup = 0x0301;
        put32(containers, 0, 1);
        put16(containers, 4, fixture.templateKey);
        put16(containers, 6, 6);
        put32(containers, 8, 0x24000000);
        put32(containers, 12, 0x43425243); // CBRC
        put32(containers, 16, 0x43425241); // CBRA
        put32(containers, 20, 0x43425244); // CBRD
        put32(containers, 28, 1);
        putFloat(containers, 32, 1);
        putFloat(containers, 36, 3);
        put32(containers, 44, 22);
        put32(powerups, 0, 1);
        put16(powerups, 4, 0x0301);
        put16(powerups, 6, 150);
        put32(powerups, 8, 6);
        put32(powerups, 12, 0x54525331); // TRS1
    }
    void chest() {
        fixture.templateKey = 0x1702;
        put16(containers, 4, fixture.templateKey);
        put16(containers, 6, 0);
        put32(containers, 8, 0x02000000);
        put32(containers, 12, 0x43434743); // CCGC
        put32(containers, 16, 0x43434741); // CCGA
        put32(containers, 20, 0);
        put32(containers, 28, 3);
        putFloat(containers, 32, 1.2f);
        putFloat(containers, 36, 1);
        putFloat(containers, 40, 1.2f);
        put32(containers, 44, 29);
    }
    LegendsContainerDefinition decode() const {
        const auto result = decodeLegendsN64Container(fixture, containers, powerups);
        REQUIRE(result);
        return *result;
    }
};
} // namespace

TEST_CASE("Native Legends containers decode their own collision and contained grant",
          "[legends][containers]") {
    NativeTables tables;
    const auto native = tables.decode();
    CHECK(native.nativeIndex == 7);
    CHECK(native.kind == LegendsContainerDefinition::Kind::Barrel);
    CHECK(native.closed == "CBRC");
    CHECK(native.opening == "CBRA");
    CHECK(native.broken == "CBRD");
    CHECK(native.damageThreshold == 6);
    CHECK(native.openingSound == 22);
    CHECK(native.closedObstacle.cylinderRadius == 1);
    CHECK(native.closedObstacle.height == 3);
    CHECK(native.yaw == -.7f);
    REQUIRE(native.contents);
    REQUIRE(legendsN64Pickup(*native.contents));
    CHECK(legendsN64Pickup(*native.contents)->offer.amount == 150);
    tables.chest();
    const auto chest = tables.decode();
    CHECK(chest.kind == LegendsContainerDefinition::Kind::Chest);
    CHECK(chest.openingSound == 29);
    CHECK(chest.closedObstacle.cylinderRadius == 0);
    CHECK(chest.closedObstacle.halfAcross == 19.0f / 32);
    CHECK(chest.closedObstacle.halfAlong == 19.0f / 32);
    CHECK_FALSE(chest.closedObstacle.touchedBy({7, 2, 9}, .75f, 0));
    CHECK(chest.closedObstacle.height == 1);
    CHECK(chest.goldOpening == "CCGG");
}

TEST_CASE("Native barrels require one sufficient hit and release once at animation completion",
          "[legends][containers]") {
    LegendsContainer barrel{NativeTables{}.decode()};
    REQUIRE(barrel.targetable());
    REQUIRE(barrel.obstacle().solid);
    for (usize i = 0; i < 20; ++i) {
        CHECK_FALSE(barrel.strike(5.99f));
    }
    CHECK_FALSE(barrel.strike(std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(barrel.strike(std::numeric_limits<f32>::quiet_NaN()));
    REQUIRE(barrel.strike(6));
    CHECK_FALSE(barrel.targetable());
    CHECK_FALSE(barrel.obstacle().solid);
    CHECK_FALSE(barrel.strike(9999));
    CHECK(barrel.state() == LegendsContainer::State::Opening);
    REQUIRE(barrel.finishOpening());
    CHECK_FALSE(barrel.finishOpening());
    CHECK(barrel.state() == LegendsContainer::State::Open);
    barrel.collected();
    CHECK(barrel.state() == LegendsContainer::State::Empty);
    CHECK_FALSE(barrel.obstacle().solid);
}

TEST_CASE("Native chests spend exactly one key on contact and ignore attacks",
          "[legends][containers]") {
    NativeTables tables;
    tables.chest();
    LegendsContainer chest{tables.decode()};
    s32 keys = 2;
    CHECK_FALSE(chest.strike(9999));
    CHECK_FALSE(chest.touch({50, 2, 9}, .75f, 4, keys));
    CHECK_FALSE(chest.touch({5, 8, 9}, .75f, 4, keys));
    CHECK(keys == 2);
    SECTION("a taller player can reach a chest above their feet") {
        REQUIRE(chest.touch({5, -1, 9}, .75f, 4, keys));
        CHECK(keys == 1);
        return;
    }
    keys = 0;
    CHECK_FALSE(chest.touch({5, 2, 9}, .75f, 4, keys));
    keys = 2;
    REQUIRE(chest.touch({5, 2, 9}, .75f, 4, keys));
    CHECK(keys == 1);
    CHECK(chest.obstacle().solid);
    CHECK(chest.obstacle().blocksSegment({5, 3, 5}, {5, 3, 13}, .5f));
    CHECK_FALSE(chest.touch({5, 2, 9}, .75f, 4, keys));
    CHECK(keys == 1);
    REQUIRE(chest.finishOpening());
    CHECK_FALSE(chest.obstacle().solid);
}

TEST_CASE("Native container decoder refuses malformed metadata and identifies absent loot",
          "[legends][containers]") {
    NativeTables tables;
    SECTION("truncation") {
        tables.containers.resize(47);
        CHECK_THROWS_AS(tables.decode(), FormatError);
    }
    SECTION("bounds") {
        putFloat(tables.containers, 32, std::numeric_limits<f32>::quiet_NaN());
        CHECK_THROWS_AS(tables.decode(), FormatError);
    }
    SECTION("hidden placement") {
        tables.fixture.placementFlags = 8;
        CHECK_FALSE(decodeLegendsN64Container(tables.fixture, tables.containers, tables.powerups));
    }
    SECTION("empty") {
        tables.fixture.containedPowerup = 0x2400;
        put16(tables.powerups, 4, 0x2400);
        const auto empty = tables.decode();
        CHECK_FALSE(empty.contents);
        CHECK(empty.diagnostic.empty());
    }
    SECTION("timed selector") {
        tables.fixture.containedPowerup = 0x2201;
        put16(tables.powerups, 4, 0x2201);
        const auto selector = tables.decode();
        CHECK_FALSE(selector.contents);
        CHECK(selector.selector == 0x2201);
        CHECK(selector.diagnostic.empty());
    }
}

TEST_CASE("All native N64 container placements decode without replacing authored rewards",
          "[legends][containers][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/pcon.rom")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto powerups = readFile(root / "items/generic/ppow.rom");
    const auto decorations = readFile(root / "items/generic/pdec.rom");
    const auto generators = readFile(root / "items/generic/pgen.rom");
    const auto traps = readFile(root / "items/generic/ptrap.rom");
    const auto teleports = readFile(root / "items/generic/ptel.rom");
    const auto containers = readFile(root / "items/generic/pcon.rom");
    const auto randoms = readFile(root / "items/generic/prnd.rom");
    const LegendsN64Templates templates{powerups,  decorations, generators, traps,
                                        teleports, containers,  randoms};
    usize count = 0;
    usize knownContents = 0;
    std::set<u16> types;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        for (const auto& fixture :
             decodeLegendsN64Fixtures(readFile(entry.path() / "ipr.rom"), templates)) {
            if (fixture.kind != LegendsN64FixtureKind::Container) {
                continue;
            }
            const auto definition = decodeLegendsN64Container(fixture, containers, powerups);
            REQUIRE(definition);
            CHECK(definition->nativeIndex == fixture.nativeIndex);
            CHECK(definition->position == fixture.position);
            CHECK(definition->closedObstacle.solid);
            types.insert(fixture.templateKey);
            if (definition->contents && legendsN64Pickup(*definition->contents)) {
                ++knownContents;
            }
            ++count;
        }
    }
    CHECK(count == 1065);
    CHECK(types == std::set<u16>{0x1701, 0x1702, 0x1901, 0x2e01});
    CHECK(knownContents > 400);
}

TEST_CASE("Native barrel and chest art release pickups through the ordinary actor inventory",
          "[legends][containers][scene-adapter][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto objects = readFile(directory / "objects.wad");
    const auto placements = readFile(directory / "ipr.rom");
    const usize containerRecords = readU32BE(placements, 24);
    auto map = LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                             ? readFile(directory / "texdyn.rom")
                             : std::vector<u8>{};
    for (auto& texture : decodeLegendsN64Textures(objects, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto targets = scene->containerTargets();
    REQUIRE_FALSE(targets.empty());
    static_cast<void>(scene->takeSounds());
    bool testedBarrel = false;
    bool testedChest = false;
    for (const auto& target : targets) {
        if (target.nativeTrap) {
            continue;
        }
        // This test exercises fixed authored gold; selector choices have their
        // own shared-clock coverage and need not select gold when opened.
        if ((readU16BE(placements,
                       containerRecords + static_cast<usize>(target.nativeIndex) * 16 + 12) >>
             8U) == 0x22) {
            continue;
        }
        if ((target.breakable && testedBarrel) || (!target.breakable && testedChest)) {
            continue;
        }
        const auto pickups = scene->pickups();
        const auto pickup = std::ranges::find_if(pickups, [&](const auto& value) {
            return !value.active && value.position == target.base &&
                   value.offer.kind == static_cast<s32>(ItemKind::Gold);
        });
        if (pickup == pickups.end()) {
            continue;
        }
        const usize pickupIndex = static_cast<usize>(pickup - pickups.begin());
        const s32 gold = pickup->offer.amount;
        CAPTURE(target.id, target.breakable, gold);
        CharacterSave save;
        save.progress().inventory.keys = 2;
        save.gold = 0;
        PlayerActor actor;
        actor.spawn(0, save, nullptr, target.base, 0);
        if (target.breakable) {
            REQUIRE_FALSE(scene->strikeContainer(target.id, 5, 0, 0));
            REQUIRE(scene->strikeContainer(target.id, 6, 0, 0));
            CHECK_FALSE(scene->strikeContainer(target.id, 6, 0, 0));
            testedBarrel = true;
        } else {
            CHECK_FALSE(scene->strikeContainer(target.id, 9999, 0, 0));
            static_cast<void>(scene->collect(actor, 1, false));
            CHECK(actor.save().progress().inventory.keys == 1);
            testedChest = true;
        }
        static_cast<void>(scene->collect(actor, 1, false));
        const auto sounds = scene->takeSounds();
        CHECK(std::ranges::count(sounds, target.breakable ? 22U : 29U, &LegendsSoundEvent::token) ==
              1);
        CHECK(scene->takeSounds().empty());
        CHECK_FALSE(scene->pickups()[pickupIndex].active);
        CHECK_FALSE(scene->pickups()[pickupIndex].collected);
        CHECK(actor.save().gold == 0);
        s32 openingTicks = 0;
        while (!scene->pickups()[pickupIndex].active && openingTicks < 120) {
            const auto bodies = scene->containerObstacles();
            CHECK(std::ranges::any_of(bodies, [&](const auto& body) {
                      return body.centre == target.base && body.solid;
                  }) == !target.breakable);
            static_cast<void>(scene->collect(actor, 1, false));
            CHECK(actor.save().gold == 0);
            CHECK_FALSE(scene->pickups()[pickupIndex].collected);
            scene->capturePresentation();
            scene->update(1.0f / 30);
            ++openingTicks;
        }
        REQUIRE(openingTicks > 1);
        REQUIRE(scene->pickups()[pickupIndex].active);
        CHECK_FALSE(std::ranges::any_of(scene->containerObstacles(), [&](const auto& body) {
            return body.centre == target.base && body.solid;
        }));
        static_cast<void>(scene->collect(actor, 1, false));
        CHECK(scene->pickups()[pickupIndex].collected);
        CHECK(actor.save().gold == gold);
        static_cast<void>(scene->collect(actor, 1, false));
        CHECK(actor.save().gold == gold);
        CHECK(actor.save().progress().inventory.keys == (target.breakable ? 2 : 1));
    }
    CHECK(testedBarrel);
    CHECK(testedChest);
}

TEST_CASE("Native random containers freeze the prebound outcome chosen by the shared timer",
          "[legends][containers][loot-selectors][scene-adapter][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    for (auto& texture : decodeLegendsN64Textures(objects)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto targets = scene->containerTargets();
    const auto barrel = std::ranges::find_if(
        targets, [](const auto& target) { return !target.nativeTrap && target.nativeIndex == 20; });
    REQUIRE(barrel != targets.end());
    REQUIRE(barrel->breakable);
    // The two visible and two gated POTR consumers preceding this barrel leave
    // its first selected native outcome PBRD, not a random roll when struck.
    for (s32 tick = 0; tick < 21; ++tick) {
        scene->update(1.0f / 30);
    }
    REQUIRE(scene->strikeContainer(barrel->id, 6, 0, 0));
    for (s32 tick = 0; tick < 120; ++tick) {
        scene->update(1.0f / 30);
    }
    const auto pickups = scene->pickups();
    const auto active = std::ranges::find_if(pickups, [&](const auto& pickup) {
        return pickup.active && pickup.position == barrel->base && pickup.nativeIndex == 20;
    });
    REQUIRE(active != pickups.end());
    CHECK(active->name == "PBRD");
    REQUIRE(active->offer.kind == static_cast<s32>(ItemKind::Potion));
    const usize index = static_cast<usize>(active - pickups.begin());
    for (s32 tick = 0; tick < 200; ++tick) {
        scene->update(1.0f / 30);
    }
    CHECK(scene->pickups()[index].name == "PBRD");
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, barrel->base, 0);
    static_cast<void>(scene->collect(actor, 1, false));
    CHECK(scene->pickups()[index].collected);
    CHECK(actor.save().progress().inventory.potions.size() == 1);
}

TEST_CASE("Native floor selectors change visible pickup and stop after collection",
          "[legends][containers][loot-selectors][scene-adapter][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/hell1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                             ? readFile(directory / "texdyn.rom")
                             : std::vector<u8>{};
    for (auto& texture : decodeLegendsN64Textures(objects, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto pickups = scene->pickups();
    const auto initial = std::ranges::find_if(pickups, [](const auto& pickup) {
        return pickup.active && pickup.nativeIndex == 0 && pickup.name == "PYRD";
    });
    REQUIRE(initial != pickups.end());
    const usize index = static_cast<usize>(initial - pickups.begin());
    scene->update(1.0f / 30);
    REQUIRE(scene->pickups()[index].name == "PGRD");
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, scene->pickups()[index].position, 0);
    static_cast<void>(scene->collect(actor, 1, false));
    REQUIRE(scene->pickups()[index].collected);
    const auto potions = actor.save().progress().inventory.potions;
    for (s32 tick = 0; tick < 120; ++tick) {
        scene->update(1.0f / 30);
    }
    CHECK(scene->pickups()[index].collected);
    static_cast<void>(scene->collect(actor, 1, false));
    CHECK(actor.save().progress().inventory.potions == potions);
}

TEST_CASE("Native explosive barrels decode from PTrap rather than ordinary containers",
          "[legends][containers][blasts]") {
    LegendsN64Fixture fixture;
    fixture.kind = LegendsN64FixtureKind::Trap;
    fixture.templateKey = 0x1301;
    fixture.scale = 1;
    fixture.position = {5, 7, 9};
    std::vector<u8> templates(100);
    put32(templates, 0, 1);
    put16(templates, 4, 0x1301);
    put32(templates, 8, 0x04000000);
    put16(templates, 12, 1);
    templates[14] = 40;
    put32(templates, 16, 0x54425252); // TBRR
    put32(templates, 20, 0x54425241); // TBRA
    put32(templates, 24, 0x54425244); // TBRD
    put32(templates, 32, 147);
    put32(templates, 36, 1);
    putFloat(templates, 40, 1);
    putFloat(templates, 44, 3);
    SECTION("red barrel") {
        const auto decoded = decodeLegendsN64Explosive(fixture, templates);
        REQUIRE(decoded);
        CHECK(decoded->hazard == LegendsContainerDefinition::Hazard::Fire);
        CHECK(decoded->group == "TRAP");
        CHECK(decoded->openingSound == 147);
        CHECK(decoded->blastRadius == 5);
        CHECK(decoded->blastDamage == 40);
        CHECK(decoded->damageThreshold == 1);
        LegendsContainer state{*decoded};
        CHECK_FALSE(state.strike(.99f));
        CHECK(state.strike(1));
        CHECK_FALSE(state.obstacle().solid);
        CHECK_FALSE(state.strike(999));
    }
    SECTION("scaled placement changes body not the authored blast radius") {
        fixture.scale = 2;
        const auto decoded = decodeLegendsN64Explosive(fixture, templates);
        REQUIRE(decoded);
        CHECK(decoded->closedObstacle.cylinderRadius == 2);
        CHECK(decoded->closedObstacle.height == 6);
        CHECK(decoded->blastRadius == 5);
    }
    SECTION("unsupported placeholder is not fabricated") {
        fixture.templateKey = 0x1302;
        CHECK_FALSE(decodeLegendsN64Explosive(fixture, templates));
    }
    SECTION("truncated native table fails") {
        templates.resize(99);
        CHECK_THROWS_AS(decodeLegendsN64Explosive(fixture, templates), FormatError);
    }
    SECTION("invalid dimensions fail") {
        putFloat(templates, 44, std::numeric_limits<f32>::quiet_NaN());
        CHECK_THROWS_AS(decodeLegendsN64Explosive(fixture, templates), FormatError);
    }
}

TEST_CASE("All native explosive barrel placements retain their trap identity and attack metadata",
          "[legends][containers][blasts][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ptrap.rom")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto powerups = readFile(root / "items/generic/ppow.rom");
    const auto decorations = readFile(root / "items/generic/pdec.rom");
    const auto generators = readFile(root / "items/generic/pgen.rom");
    const auto traps = readFile(root / "items/generic/ptrap.rom");
    const auto teleports = readFile(root / "items/generic/ptel.rom");
    const auto containers = readFile(root / "items/generic/pcon.rom");
    const auto randoms = readFile(root / "items/generic/prnd.rom");
    const LegendsN64Templates templates{powerups,  decorations, generators, traps,
                                        teleports, containers,  randoms};
    usize fire = 0;
    usize poison = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        for (const auto& fixture :
             decodeLegendsN64Fixtures(readFile(entry.path() / "ipr.rom"), templates)) {
            const auto definition = decodeLegendsN64Explosive(fixture, traps);
            if (!definition) {
                continue;
            }
            CAPTURE(entry.path(), fixture.nativeIndex, fixture.templateKey);
            CHECK(definition->nativeIndex == fixture.nativeIndex);
            CHECK(definition->position == fixture.position);
            CHECK_FALSE(definition->contents);
            CHECK_FALSE(definition->selector);
            CHECK(definition->damageThreshold == 1);
            if (fixture.templateKey == 0x1301) {
                ++fire;
                CHECK(definition->blastDamage == 40);
                CHECK(definition->openingSound == 147);
            } else {
                ++poison;
                CHECK(definition->blastDamage == 30);
                CHECK(definition->openingSound == 22);
            }
        }
    }
    CHECK(fire == 134);
    CHECK(poison == 128);
}

TEST_CASE("Native red and green barrels launch their own art and exactly one combat event",
          "[legends][containers][blasts][scene-adapter][assets]") {
    const auto directory = test::dataDirectory().parent_path() / "assets/n64/extracted/levels/ice1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    for (auto& texture : decodeLegendsN64Textures(objects)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const auto targets = scene->containerTargets();
    for (const auto& warning : scene->warnings()) {
        INFO(warning);
        CHECK_FALSE(warning.starts_with("Native explosive-barrel"));
    }
    // Both are ungated placements in Ice1; Mount1's only green barrel requires
    // an additional player and therefore is intentionally absent solo.
    for (const u32 index : {21U, 31U}) {
        const bool poison = index == 31;
        const auto target = std::ranges::find_if(targets, [&](const auto& candidate) {
            return candidate.nativeTrap && candidate.nativeIndex == index;
        });
        REQUIRE(target != targets.end());
        REQUIRE(target->breakable);
        static_cast<void>(scene->takeSounds());
        CHECK_FALSE(scene->strikeContainer(target->id, .99f, 0, 0));
        CHECK(scene->takeFixtureEvents().empty());
        const auto frame = CameraFrame::of(WorldCamera{});
        device.draws.clear();
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
        const auto before = device.draws;
        REQUIRE(scene->strikeContainer(target->id, 1, 0, 2));
        CHECK_FALSE(scene->strikeContainer(target->id, 1, 0, 2));
        const auto events = scene->takeFixtureEvents();
        REQUIRE(events.size() == 1);
        const auto& event = events.front();
        CHECK(event.kind ==
              (poison ? LegendsFixtureEvent::Kind::Poison : LegendsFixtureEvent::Kind::Explosion));
        CHECK(event.position == target->base + Vec3{0, 2, 0});
        CHECK(event.player == 2);
        CHECK(event.damage == (poison ? 30 : 40));
        CHECK(event.radius == (poison ? 4.5f : 5));
        CHECK(event.seconds > 0);
        CHECK(event.noFalloff == poison);
        // Without advancing the rest of the level, the active/debris/attack
        // trees must replace the ready tree through the real render bindings.
        device.draws.clear();
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
        scene->draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
        bool changed = before.size() != device.draws.size();
        for (usize i = 0; i < std::min(before.size(), device.draws.size()); ++i) {
            changed |= before[i].texture != device.draws[i].texture ||
                       before[i].vertices != device.draws[i].vertices;
        }
        CHECK(changed);
        const auto sounds = scene->takeSounds();
        REQUIRE(sounds.size() == (poison ? 2 : 1));
        CHECK(sounds.front().token == (poison ? 22 : 147));
        if (poison) {
            CHECK(sounds.back().token == 60);
        }
        CHECK(scene->takeSounds().empty());
        CHECK(scene->takeFixtureEvents().empty());
        const auto remaining = scene->containerTargets();
        CHECK(std::ranges::none_of(remaining,
                                   [&](const auto& current) { return current.id == target->id; }));
        for (usize tick = 0; tick < 300; ++tick) {
            scene->capturePresentation();
            scene->update(1.0f / 30);
        }
        CHECK(scene->takeFixtureEvents().empty());
        CHECK_FALSE(scene->strikeContainer(target->id, 999, 0, 2));
    }
}
