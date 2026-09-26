#include <array>
#include <cmath>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/ItemPickup.h"
#include "game/world/PlacedItems.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("food poisoning preserves missing artwork and uses record kind rather than value",
          "[game][world][poison-food]") {
    const bool missingFigure = GENERATE(false, true);
    const auto dir = test::scratchDirectory("placed-items-poison");
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/FOOD.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(
        dir / "objects.json",
        R"({"objects":[{"index":0,"name":"FOOD","file":"models/FOOD.obj","meshTriangles":1}]})");
    writeFile(dir / "textures/FOOD.png", test::kTinyPng);
    writeTextFile(
        dir / "textures.json",
        R"({"defs":[],"bitmaps":[{"index":0,"name":"FOOD","file":"textures/FOOD.png","width":2,"height":2,"flags":0}]})");
    const std::string trees =
        missingFigure
            ? R"({"trees":[{"name":"FOOD","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]}]})"
            : R"({"trees":[{"name":"FOOD","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]},{"name":"BADMEAT","nodes":[{"name":"FOOD","object":"FOOD","parent":-1,"position":[0,0,0]}]}]})";
    writeTextFile(dir / "animations.json", trees);
    // A meat-kind record with a small healing value: classification is not value >= 100.
    writeTextFile(dir / "world.json",
                  R"({"objects":[{"name":"GROUND","position":[0,0,0]}],"itemInfos":[
        {"type":1,"subtype":3,"name":"FOOD","collisionType":1,"radius":0.5,"height":2,
         "armor":-2,"hitPoints":2,"value":10,"collisionOffset":[2,0,0]}],
        "itemInstances":[{"info":0,"position":[0,0,0],"minPlayers":1}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&archive};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    REQUIRE(items.size() == 1);
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == 0); // hidden party instance
    items.setPlayerCount(1);
    CHECK(items.poisonFood(device, Vec3{0}, 1, 10) == 0); // collision centre is offset
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 2) == 0);
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == (missingFigure ? 0 : 1));
    CHECK(items.item(0).name == (missingFigure ? "FOOD" : "BADMEAT"));
    CHECK(items.item(0).value == (missingFigure ? 10 : -100));
    CHECK(items.item(0).takeable());
    items.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    CHECK(items.poisonFood(device, Vec3{2, 0, 0}, 1, 10) == 0);
}

TEST_CASE("gas poisons food models and pickup values without moving or consuming them",
          "[game][world][poison-food][unpacked]") {
    const auto root = test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELG1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(0); // isolate newly placed food from authored level pickups
    const Vec3 origin{10000, 0, 10000};
    const usize apple = items.size();
    REQUIRE(items.place(device, "APPLE", origin, nullptr));
    const usize meat = items.size();
    REQUIRE(items.place(device, "CHICKEN", origin + Vec3{0, 0, 2}, nullptr));
    const usize protectedFood = items.size();
    REQUIRE(items.place(device, "HAM", origin, nullptr));
    items.attach(protectedFood, items.item(protectedFood).transform, true);
    const usize distant = items.size();
    REQUIRE(items.place(device, "APPLE", origin + Vec3{20, 0, 0}, nullptr));
    const usize above = items.size();
    REQUIRE(items.place(device, "APPLE", origin + Vec3{0, 20, 0}, nullptr));
    const usize potion = items.size();
    REQUIRE(items.place(device, "POT_BLU", origin, nullptr));
    const Mat4 transform = items.item(meat).transform;
    const s32 record = items.item(meat).info;
    REQUIRE(items.poisonFood(device, origin, 6.5f, 2) == 0);
    REQUIRE(items.poisonFood(device, origin, 0, 10) == 0);
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 2);
    CHECK(items.item(apple).name == "GAPPLE");
    CHECK(items.item(apple).value == -50);
    CHECK(items.item(meat).name == "BADMEAT");
    CHECK(items.item(meat).value == -100);
    CHECK(items.item(meat).info == record);
    CHECK(items.item(meat).transform == transform);
    CHECK(items.item(protectedFood).name == "HAM");
    CHECK(items.item(distant).name == "APPLE");
    CHECK(items.item(above).name == "APPLE");
    CHECK(items.item(potion).name == "POT_BLU");
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 0);
    for (const usize index : {apple, meat}) {
        const auto& item = items.item(index);
        REQUIRE(item.figure == &powerups.trees.tree(*powerups.trees.find(item.name)));
        REQUIRE(item.model.bound());
        REQUIRE(item.takeable());
    }
    items.update(0.1f);
    items.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    CharacterSave save;
    save.progress().health = 500;
    const std::array collectors{Collector{origin, 4, 1}};
    const auto collected =
        items.collect(device, collectors, [&](const Pickup& pickup) -> std::optional<s32> {
            if (pickup.subtype != static_cast<s32>(ItemKind::Food)) {
                return std::nullopt;
            }
            const auto result =
                takeItem(save, {pickup.subtype, pickup.amount, pickup.flags, pickup.strength});
            CHECK(result.hurt);
            CHECK_FALSE(result.ate);
            CHECK(result.message == 28);
            return result.left;
        });
    REQUIRE(collected.size() == 2);
    CHECK(save.health() == 350);
    CHECK(items.item(apple).taken);
    CHECK(items.item(meat).taken);
    // Chest contents become vulnerable only after release; taken food never returns.
    items.attach(protectedFood, items.item(protectedFood).transform, false);
    REQUIRE(items.poisonFood(device, origin, 6.5f, 10) == 1);
    CHECK(items.item(protectedFood).name == "BADMEAT");
    CHECK(items.item(apple).taken);
}

TEST_CASE("the tower's crystals stand on the floor for a party large enough",
          "[game][world][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELL1/collision.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    WorldCollision collision;
    REQUIRE(collision.load(root / "LEVELS/LEVELL1", layout));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, &collision, archives));
    REQUIRE(items.size() >= 15);
    REQUIRE(items.visibleCount() == 0); // nobody in the party yet

    usize gems = 0;
    for (usize i = 0; i < items.size(); ++i) {
        const PlacedItems::Item& item = items.item(i);
        if (item.name != "GEMORANGE") {
            continue;
        }
        ++gems;
        REQUIRE(item.subtype == ItemInfo::kCrystal);
        REQUIRE(item.minPlayers == 1);
        REQUIRE(item.model.bound());
        // Lifted onto the floor near where the level put it.
        const ItemInstance& source = layout.itemInstances()[static_cast<usize>(item.instance)];
        REQUIRE(item.position.x == source.position.x);
        REQUIRE(std::abs(item.position.y - source.position.y) <=
                PlacedItems::kFloorReachAbove + PlacedItems::kFloorLift);
        REQUIRE(Vec3{item.transform[3]} == item.position);
    }
    REQUIRE(gems == 15);

    items.setPlayerCount(1);
    REQUIRE(items.visibleCount() >= 15);
    // The crystals turn: their tree's sequence loops, moving the gem's node.
    usize firstGem = 0;
    while (items.item(firstGem).name != "GEMORANGE") {
        ++firstGem;
    }
    REQUIRE(items.item(firstGem).player.playing());
    const Mat4 gemAtRest = items.item(firstGem).pose.matrices()[2];
    items.update(0.5f);
    const Mat4& turned = items.item(firstGem).pose.matrices()[2];
    REQUIRE(turned != gemAtRest);
    // About the vertical axis alone: up stays up, and the gem's foot stays put.
    REQUIRE(Vec3{turned[1]}.y == Approx(1.0f).margin(1e-4f));
    REQUIRE(std::abs(Vec3{turned[1]}.x) < 1e-4f);
    REQUIRE(std::abs(Vec3{turned[1]}.z) < 1e-4f);
    REQUIRE(Vec3{turned[3]} == Vec3{0.0f, 0.0f, 0.0f});
    // The sheen slides over the crystal: the archive's scroll on its texture has moved.
    constexpr u32 kSheenTexture = 181;
    REQUIRE(items.item(firstGem).model.textureOffset(kSheenTexture) != Vec2{0.0f, 0.0f});
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.size() >= usize{60}); // a shadow, the crystal, its shine and glow
    bool glowing = false;
    for (const auto& draw : device.draws) {
        glowing = glowing || draw.state.blend == BlendMode::Additive;
    }
    REQUIRE(glowing);

    items.setPlayerCount(0);
    device.draws.clear();
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty());
    items.clear();
    REQUIRE(items.size() == 0);
}

TEST_CASE("a collector on a crystal takes it and its burst plays", "[game][world][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELL1/collision.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    const usize shown = items.visibleCount();
    usize gem = 0;
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "GEMORANGE") {
            gem = i;
            break;
        }
    }
    const PlacedItems::Item& crystal = items.item(gem);
    REQUIRE(crystal.realm() == 1); // the orange gems count towards the first realm
    REQUIRE(crystal.radius == 0.1f);
    REQUIRE(crystal.height == 2.0f);
    // Off to the side nothing is taken; a character's whole width away, the crystal goes to
    // one whose reach is that width, not to one reaching half of it.
    Collector far;
    far.position = crystal.position + Vec3{3.0f, 0.0f, 0.0f};
    REQUIRE(items.collect(device, std::array{far}).empty());
    Collector on;
    on.position = crystal.position + Vec3{1.4f, 1.0f, 0.0f};
    on.radius = 0.75f;
    REQUIRE(items.collect(device, std::array{on}).empty());
    on.radius = 1.5f;
    const std::vector<Pickup> pickups = items.collect(device, std::array{on});
    REQUIRE(pickups.size() == 1);
    REQUIRE(pickups[0].item == gem);
    REQUIRE(pickups[0].collector == 0);
    REQUIRE(pickups[0].subtype == ItemInfo::kCrystal);
    REQUIRE(pickups[0].realm == 1);
    REQUIRE(pickups[0].position == crystal.position);
    REQUIRE(crystal.taken);
    REQUIRE_FALSE(crystal.visible);
    REQUIRE(items.visibleCount() == shown - 1);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.collect(device, std::array{on}).empty()); // only once
    items.setPlayerCount(2);
    REQUIRE_FALSE(crystal.visible); // taken stays taken
    // The burst's sparks pour out for the tree's second and a half, then die away.
    REQUIRE(items.burstParticleCount() == 0);
    items.update(1.0f / 30.0f);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.burstParticleCount() > 0);
    // Its emitters ride the tree's sequence: two of them, moving on as it plays.
    const PlacedItems::Effect& burst = items.effect(0);
    REQUIRE(burst.player.playing());
    REQUIRE(burst.emitters.size() == 2);
    const Mat4 emitterStart = items.bursts().emitter(burst.emitters[0].second).node();
    device.draws.clear();
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    bool sparks = false;
    for (const auto& draw : device.draws) {
        sparks = sparks || (draw.texture ==
                            &powerups.textures.texture(device, *powerups.textures.find("ORAN03")));
    }
    REQUIRE(sparks);
    items.update(1.0f);
    REQUIRE(items.effectCount() == 1);
    REQUIRE(items.bursts().emitter(burst.emitters[0].second).node() != emitterStart);
    // Frame by frame (a long jump counts as one frame): the emitters stop at a second and a
    // half and the last sparks die within the next.
    for (s32 frame = 0; frame < 90; ++frame) {
        items.update(1.0f / 30.0f);
    }
    REQUIRE(items.effectCount() == 0);
    REQUIRE(items.burstParticleCount() == 0);
}

TEST_CASE("items can be dropped by their record's name and left lying or part taken",
          "[game][world][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELL1/world.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    const usize before = items.size();
    REQUIRE_FALSE(items.place(device, "NO_SUCH_THING", Vec3{0.0f}, nullptr));
    REQUIRE(items.place(device, "KEYRING", Vec3{500.0f, 0.0f, 500.0f}, nullptr));
    REQUIRE(items.place(device, "POT_GRE", Vec3{520.0f, 0.0f, 500.0f}, nullptr));
    REQUIRE(items.size() == before + 2);
    const PlacedItems::Item ring = items.item(before);
    REQUIRE(ring.visible);
    REQUIRE(ring.subtype == 2);
    REQUIRE(ring.value == 3);
    REQUIRE(items.item(before + 1).flags == 4); // the green potion's kind
    // By its record's number too, as a chest drops what it held, holding as many as said.
    const s32 keyRecord = items.item(before).info;
    REQUIRE_FALSE(items.placeRecord(device, -1, Vec3{0.0f}, nullptr));
    REQUIRE_FALSE(items.placeRecord(device, 9999, Vec3{0.0f}, nullptr));
    REQUIRE(items.placeRecord(device, keyRecord, Vec3{540.0f, 0.0f, 500.0f}, nullptr, 5));
    REQUIRE(items.item(before + 2).value == 5);
    REQUIRE(items.item(before + 2).name == "KEYRING");

    Collector on;
    on.position = Vec3{500.0f, 0.0f, 500.0f};
    const std::array<Collector, 1> party{on};
    // Refused, it stays; part taken, it stays with what is left; taken, it goes.
    s32 asked = 0;
    REQUIRE(items
                .collect(device, party,
                         [&](const Pickup& pickup) -> std::optional<s32> {
                             ++asked;
                             REQUIRE(pickup.amount == 3);
                             REQUIRE(pickup.subtype == 2);
                             return std::nullopt;
                         })
                .empty());
    REQUIRE(asked == 1);
    REQUIRE(items.item(before).visible);
    std::vector<Pickup> got =
        items.collect(device, party, [](const Pickup&) -> std::optional<s32> { return 1; });
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].amount == 3);
    REQUIRE(items.item(before).visible);
    REQUIRE(items.item(before).value == 1);
    got = items.collect(device, party, [](const Pickup& pickup) -> std::optional<s32> {
        REQUIRE(pickup.amount == 1);
        return 0;
    });
    REQUIRE(got.size() == 1);
    REQUIRE(items.item(before).taken);
    REQUIRE(items.collect(device, party).empty());
}

TEST_CASE("the crystals can start unseen and be revealed from the origin outward",
          "[game][world][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&powerups};
    REQUIRE(items.bind(device, layout, nullptr, archives));
    items.setPlayerCount(1);
    REQUIRE_FALSE(items.revealing());
    items.hideCrystals();
    REQUIRE(items.revealing());
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty()); // unseen
    const auto fading = [&]() {
        usize count = 0;
        for (usize i = 0; i < items.size(); ++i) {
            const PlacedItems::Item& item = items.item(i);
            count += item.subtype == ItemInfo::kCrystal && item.alpha > 0.0f ? 1 : 0;
        }
        return count;
    };
    // The reveal starts 26 units out; the nearest gem stands 38 out, so nothing shows yet.
    items.reveal(0.0f);
    REQUIRE(fading() == 0);
    // A second on it reaches 41: that gem alone fades, nearly whole within the second.
    items.reveal(1.0f);
    REQUIRE(fading() == 1);
    for (usize i = 0; i < items.size(); ++i) {
        const PlacedItems::Item& item = items.item(i);
        if (item.alpha > 0.0f) {
            REQUIRE(item.alpha > 0.9f);
            REQUIRE(item.alpha < 1.0f);
        }
    }
    items.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(device.draws[0].state.depthWrite); // blended while it fades
    // Four seconds more reach past the farthest, and every gem is whole.
    items.reveal(4.0f);
    REQUIRE_FALSE(items.revealing());
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).subtype == ItemInfo::kCrystal) {
            REQUIRE(items.item(i).alpha == 1.0f);
        }
    }
    items.reveal(1.0f); // nothing left to reveal
    REQUIRE_FALSE(items.revealing());
}

TEST_CASE("a thrown item sails out, bounces to rest on the floor and can be taken only after "
          "a while",
          "[game][world][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("ITEMS/LEVELG5/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::unpackedOrSkip("LEVELS/LEVELG5/collision.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELG5"));
    WorldCollision collision;
    REQUIRE(collision.load(root / "LEVELS/LEVELG5", layout));
    ItemArchive crypt;
    REQUIRE(crypt.load(root / "ITEMS/LEVELG5"));
    test::FakeRenderDevice device;
    PlacedItems items;
    const std::array<ItemArchive*, 1> archives{&crypt};
    items.bind(device, layout, &collision, archives);
    items.setPlayerCount(1);
    REQUIRE_FALSE(items.goldLeft());
    const usize index = items.size();
    // From three up over the boss's mark, thrown up and toward where the party comes in.
    REQUIRE(items.throwItem(device, "COIN_GOLD", Vec3{0.0f, 3.0f, 0.0f}, Vec3{0.0f, 20.0f, 10.0f},
                            &collision, 2.0f));
    REQUIRE(items.goldLeft());
    const PlacedItems::Item& coin = items.item(index);
    REQUIRE(coin.thrown);
    REQUIRE(coin.value == 5000);
    REQUIRE(coin.position == Vec3{0.0f, 3.0f, 0.0f}); // not on the floor yet
    REQUIRE_FALSE(coin.takeable());
    // These sprites spin through sequence-keyed texture frames, not geometry.
    coin.model.draw(device, Mat4{1}, coin.transform);
    REQUIRE_FALSE(device.draws.empty());
    const Texture* firstTexture = device.draws.back().texture;
    device.draws.clear();
    items.update(5.0f / 30.0f);
    coin.model.draw(device, Mat4{1}, coin.transform);
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(device.draws.back().texture != firstTexture);
    // It rises first, then falls and bounces.
    f32 highest = 0.0f;
    s32 bounces = 0;
    f32 lastVy = coin.velocity.y;
    for (s32 i = 0; i < 300; ++i) {
        items.update(1.0f / 60.0f);
        highest = std::max(highest, coin.position.y);
        if (lastVy < 0.0f && coin.velocity.y > 0.0f) {
            ++bounces;
        }
        lastVy = coin.velocity.y;
    }
    REQUIRE(highest > 5.0f);
    REQUIRE(bounces >= 1);
    CAPTURE(coin.position.x, coin.position.y, coin.position.z, coin.velocity.y, coin.velocity.z);
    REQUIRE_FALSE(coin.thrown);
    REQUIRE(coin.velocity == Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(coin.position.z > 2.0f);
    const auto floor = collision.floorAt(coin.position, PlacedItems::kFloorReachAbove,
                                         PlacedItems::kFloorReachBelow);
    REQUIRE(floor.has_value());
    REQUIRE(coin.position.y == Approx(floor->y + PlacedItems::kThrownFloorLift));
    REQUIRE(Vec3{coin.transform[3]} == coin.position);
    // Five seconds on it can be taken, and once it is no gold is left.
    REQUIRE(coin.takeable());
    Collector on;
    on.position = coin.position;
    const std::array<Collector, 1> party{on};
    const std::vector<Pickup> got = items.collect(device, party);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].amount == 5000);
    REQUIRE_FALSE(items.goldLeft());
    // With no floor to land on it stays where it is thrown.
    REQUIRE(items.throwItem(device, "COIN_BRONZE", Vec3{5.0f, 3.0f, 5.0f}, Vec3{10.0f, 20.0f, 0.0f},
                            nullptr, 0.0f));
    REQUIRE_FALSE(items.item(index + 1).thrown);
    REQUIRE(items.item(index + 1).takeable());
}

TEST_CASE("an instance's minimum can mean exactly that many players", "[game][world]") {
    PlacedItems::Item item;
    item.minPlayers = 2;
    REQUIRE_FALSE(item.shownTo(1));
    REQUIRE(item.shownTo(2));
    REQUIRE(item.shownTo(4));
    item.minPlayers = PlacedItems::kExactPlayersMark + 2;
    REQUIRE_FALSE(item.shownTo(1));
    REQUIRE(item.shownTo(2));
    REQUIRE_FALSE(item.shownTo(3));
}

} // namespace
