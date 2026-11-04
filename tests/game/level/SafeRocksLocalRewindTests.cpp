#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TextureAnimator.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/combat/CombatArenaTarget.h"
#include "game/level/NativeStateResources.h"
#include "game/level/SafeRocks.h"
#include "game/level/StageRegistry.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `rocks` now holds exactly the values `saved` holds. */
bool holds(const SafeRocks& rocks, const SafeRocks::LocalSnapshot& saved) {
    SafeRocks::LocalSnapshot now;
    rocks.saveLocal(now);
    return rocks.equalLocal(now, saved);
}

/** The cover's native digest over the stage it was bound to, and any archive lending it
 * animated textures, so two loads can be compared. */
NativeStateDigest::Digest digestOf(const SafeRocks& rocks, const ItemArchive& items,
                                   const WorldLayout& layout, const RenderDevice& device,
                                   const ItemArchive* lender = nullptr) {
    NativeStateResources resources;
    test::registerItemStage(resources, items, layout, nullptr, device);
    if (lender != nullptr) {
        REQUIRE(resources.registerArchive(*lender, "lender"));
    }
    return test::liveDigest(rocks, resources);
}

void checkTargets(std::span<const CombatArenaTarget> actual,
                  std::span<const CombatArenaTarget> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK((std::tie(actual[index].index, actual[index].placement, actual[index].active) ==
               std::tie(expected[index].index, expected[index].placement, expected[index].active)));
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    SafeRocks rocks;

    explicit Fixture(std::string_view name) {
        const auto directory = test::scratchDirectory(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":10,"subtype":41,"name":"ROCK","radius":2.3,"height":5,
             "collisionType":1,"hitPoints":40,"armor":10},
            {"type":10,"subtype":43,"name":"OVERRIDE","radius":1,"height":2,
             "hitPoints":20,"armor":-1},
            {"type":10,"subtype":41,"name":"ZERO","radius":1,"height":1,
             "hitPoints":0}],
          "itemInstances":[
            {"info":0,"minPlayers":1,"position":[7,30,-1],"rotation":[0,0.4,0],
             "params":[41,0,3,0,0,0,0,0,0,0,0,0]},
            {"info":0,"minPlayers":2,"position":[20,30,-1],
             "params":[0,0,2,0,0,0,0,0,0,0,0,0]},
            {"info":1,"position":[40,30,-1],
             "params":[41,0,3,0,0,0,0,0,0,0,0,0]},
            {"info":2,"position":[60,30,-1],
             "params":[0,0,3,0,0,0,0,0,0,0,0,0]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(rocks.bind(device, layout, items));
        rocks.setPlayerCount(1);
        REQUIRE(rocks.size() == 4);
        REQUIRE(rocks.bodyResources().empty()); // Missing art does not remove gameplay cover.
    }
    NativeStateDigest::Digest digest() const { return digestOf(rocks, items, layout, device); }
};

void mutate(SafeRocks& rocks, s32 tick, f32 seconds) {
    switch (tick) {
    case 0: REQUIRE_FALSE(rocks.strike(0, 50)); break;
    case 1: REQUIRE_FALSE(rocks.strike(0, 5)); break;
    case 2: REQUIRE_FALSE(rocks.strike(0, 49)); break;
    case 3: REQUIRE(rocks.strike(0, 1000)); break;
    case 4: rocks.hideForEruptions(); break;
    case 5: rocks.scheduleActivation(0, 0.5f); break;
    case 10: rocks.scheduleActivation(0, 0.2f); break;
    case 30: rocks.setPlayerCount(2); break;
    case 31: rocks.scheduleActivation(1, 0.1f); break;
    case 45: rocks.activate(2); break;
    case 46: REQUIRE_FALSE(rocks.strike(2, 1000)); break;
    case 50: rocks.activate(3); break; // Native zero-health tier-three residue is valid.
    case 60: REQUIRE(rocks.strike(0, 1000)); break;
    default: break;
    }
    rocks.update(seconds);
    if (tick % 7 == 0) {
        rocks.capturePresentation();
    }
}

TEST_CASE("safe rock checkpoints resume all native damage and eruption states",
          "[safe-rocks-local-rewind]") {
    Fixture host("safe-rock-checkpoint-host");
    Fixture guest("safe-rock-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    std::array<bool, 4> tiers{};
    bool rescheduled = false;
    bool reactivated = false;
    SafeRocks::LocalSnapshot before;
    SafeRocks::LocalSnapshot expected;
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick, seconds);
        host.rocks.saveLocal(before);
        mutate(host.rocks, tick, seconds);
        host.rocks.saveLocal(expected);
        const auto& rock = host.rocks.rock(0);
        tiers[static_cast<usize>(rock.tier)] = true;
        rescheduled |= tick == 10 && rock.activationDelay > 0;
        reactivated |= tick > 10 && tick < 60 && !rock.dormant && rock.health == 120;
        mutate(guest.rocks, tick, seconds);
        CHECK(guest.digest() == host.digest());
        checkTargets(guest.rocks.arenaTargets(), host.rocks.arenaTargets());
        checkTargets(guest.rocks.eruptionTargets(), host.rocks.eruptionTargets());
        CHECK(guest.rocks.attackAnchors() == host.rocks.attackAnchors());
        CHECK(guest.rocks.obstacles().size() == host.rocks.obstacles().size());
        CHECK(guest.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}) ==
              host.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}));
        REQUIRE(host.rocks.restoreLocal(before));
        CHECK(holds(host.rocks, before));
        mutate(host.rocks, tick, seconds);
        CHECK(holds(host.rocks, expected));
    }
    CHECK(std::ranges::all_of(tiers, [](bool present) { return present; }));
    CHECK(rescheduled);
    CHECK(reactivated);
    CHECK(host.rocks.rock(0).health == 0);  // An expired activation is not fired twice.
    CHECK(host.rocks.rock(2).health == 60); // Negative armour remains indestructible.
    CHECK(host.rocks.rock(3).health == 0);
    CHECK(host.rocks.rock(3).tier == 3);
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("empty safe rock rosters retain their native animation binding",
          "[safe-rocks-local-rewind][assets]") {
    const auto directory = test::scratchDirectory("safe-rock-empty-animation-binding");
    writeTextFile(
        directory / "world.json",
        R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],"itemInfos":[],"itemInstances":[]})");
    WorldLayout hostLayout;
    WorldLayout guestLayout;
    REQUIRE(hostLayout.load(directory));
    REQUIRE(guestLayout.load(directory));
    const auto archive = test::assetOrSkip("ITEMS/LEVELK5/objects.ngc").parent_path();
    ItemArchive hostItems;
    ItemArchive guestItems;
    REQUIRE(hostItems.load(archive));
    REQUIRE(guestItems.load(archive));
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    SafeRocks host;
    SafeRocks guest;
    CHECK_FALSE(host.bind(hostDevice, hostLayout, hostItems));
    CHECK_FALSE(guest.bind(guestDevice, guestLayout, guestItems));
    REQUIRE(host.size() == 0);
    REQUIRE(guest.size() == 0);
    const auto unbound = digestOf(guest, guestItems, guestLayout, guestDevice);

    // PlayScene performs this bind unconditionally after opening opponents,
    // even for ordinary levels without arena cover or resolved texture clocks.
    host.bindAnimations(hostDevice, hostItems);
    host.update(0.075f);
    const auto authority = digestOf(host, hostItems, hostLayout, hostDevice);
    CHECK(unbound != authority);
    CHECK(digestOf(guest, guestItems, guestLayout, guestDevice) == unbound);

    guest.bindAnimations(guestDevice, guestItems);
    const auto uploads = guestDevice.texturesCreated;
    const auto updates = guestDevice.textureUpdates;
    guest.update(0.075f);
    CHECK(digestOf(guest, guestItems, guestLayout, guestDevice) == authority);
    CHECK(guestDevice.texturesCreated == uploads);
    CHECK(guestDevice.textureUpdates == updates);
}

TEST_CASE("native arena cover checkpoints retain all authored tiers and stable resource addresses",
          "[safe-rocks-local-rewind][assets]") {
    const auto* const name = GENERATE("B6", "A5", "I5");
    CAPTURE(name);
    const std::string relative = std::string{"LEVELS/LEVEL"} + name + "/WORLDS.PS2";
    const std::string itemRelative = std::string{"ITEMS/LEVEL"} + name + "/objects.ngc";
    WorldLayout layout;
    ItemArchive items;
    test::FakeRenderDevice device;
    const auto levelPath = test::assetOrSkip(relative).parent_path();
    const auto itemPath = test::assetOrSkip(itemRelative).parent_path();
    REQUIRE(layout.load(levelPath));
    REQUIRE(items.load(itemPath));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, items));
    rocks.bindAnimations(device, items);
    rocks.setPlayerCount(4);
    SafeRocks::LocalSnapshot before;
    rocks.saveLocal(before);
    const auto resources = rocks.bodyResources();
    REQUIRE_FALSE(resources.empty());
    const auto uploads = device.texturesCreated;
    rocks.draw(device, Mat4{1}, {});
    const auto draws = device.draws;
    REQUIRE_FALSE(draws.empty());
    for (usize index = 0; index < rocks.size(); ++index) {
        REQUIRE(rocks.strike(index, 10000));
    }
    rocks.hideForEruptions();
    rocks.scheduleActivation(0, 0.5f);
    rocks.update(0.13f);
    REQUIRE_FALSE(holds(rocks, before));
    REQUIRE(rocks.restoreLocal(before));
    CHECK(holds(rocks, before));
    const auto restoredResources = rocks.bodyResources();
    REQUIRE(restoredResources.size() == resources.size());
    for (usize index = 0; index < resources.size(); ++index) {
        CHECK(restoredResources[index].index == resources[index].index);
        CHECK(restoredResources[index].tier == resources[index].tier);
        CHECK(restoredResources[index].model == resources[index].model);
        CHECK(restoredResources[index].textures == resources[index].textures);
    }
    device.draws.clear();
    rocks.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == draws.size());
    for (usize index = 0; index < draws.size(); ++index) {
        CHECK(device.draws[index].texture == draws[index].texture);
        CHECK(device.draws[index].transform == draws[index].transform);
        CHECK(device.draws[index].vertices == draws[index].vertices);
        CHECK(device.draws[index].state == draws[index].state);
    }
    CHECK(device.texturesCreated == uploads);
}

struct PlagueFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    ItemArchive boss;
    SafeRocks rocks;
    u32 first = 0;
    usize drawCount = 0;
    usize animatedDrawCount = 0;

    PlagueFixture() {
        const auto levelPath = test::assetOrSkip("LEVELS/LEVELK5/WORLDS.PS2").parent_path();
        const auto itemPath = test::assetOrSkip("ITEMS/LEVELK5/objects.ngc").parent_path();
        const auto bossPath = test::assetOrSkip("MONSTERS/PBOSS/objects.ngc").parent_path();
        REQUIRE(layout.load(levelPath));
        REQUIRE(items.load(itemPath));
        REQUIRE(boss.load(bossPath));
        REQUIRE(rocks.bind(device, layout, items));
        rocks.setPlayerCount(1);
        const std::array lenders{&boss.textures};
        rocks.bindAnimations(device, items, lenders);
        const auto texture = boss.textures.find("BLOB_BOSS00");
        REQUIRE(texture);
        first = *texture;
        REQUIRE(rocks.size() == 3);
        const auto bodies = rocks.bodyResources();
        REQUIRE_FALSE(bodies.empty());
        REQUIRE(bodies.front().textures->size() == 1);
        // The one resolved clock is the boss's blob, whose frames start at BLOB_BOSS00.
        const auto& animations = items.trees.textureAnimations();
        const auto animation =
            std::ranges::find(animations, "BLOB_BOSS", &TextureAnimationInfo::name);
        REQUIRE(animation != animations.end());
        REQUIRE(animation->frames == 15);
        REQUIRE(animation->rate == 3);
        REQUIRE(animation->texture >= 0);
        const auto animatedSlot = static_cast<u32>(animation->texture);
        // A native rock can contain several material parts. Count the authored
        // parts and the animated slot rather than assuming one draw per rock.
        for (usize index = 0; index < rocks.size(); ++index) {
            const auto& rock = rocks.rock(index);
            const auto& placement = layout.itemInstances()[static_cast<usize>(rock.instance)];
            const auto& definition = layout.itemInfos()[static_cast<usize>(placement.info)];
            std::optional<u32> model;
            const auto base = definition.name + std::to_string(rock.tier);
            for (const auto* suffix : {"", "L1", "L1ROOT"}) {
                model = items.models.find(base + suffix);
                if (model) {
                    break;
                }
            }
            REQUIRE(model);
            const auto& parts = items.models.mesh(*model).parts;
            REQUIRE_FALSE(parts.empty());
            drawCount += parts.size();
            animatedDrawCount +=
                static_cast<usize>(std::ranges::count(parts, animatedSlot, &MeshPart::texture));
        }
        REQUIRE(animatedDrawCount > 0);
    }
    /** The texture clock every loaded tier of the cover shares. */
    TextureAnimator::Clock clock() const {
        const auto bodies = rocks.bodyResources();
        REQUIRE_FALSE(bodies.empty());
        return bodies.front().textures->clock();
    }
    NativeStateDigest::Digest digest() const {
        return digestOf(rocks, items, layout, device, &boss);
    }
    void drawFrame(u32 frame, f32 alpha) {
        device.draws.clear();
        rocks.draw(device, Mat4{1}, {}, alpha);
        const Texture* expected = &boss.textures.texture(device, first + frame);
        REQUIRE(device.draws.size() == drawCount);
        CHECK(static_cast<usize>(std::ranges::count(
                  device.draws, expected, &test::RecordedDraw::texture)) == animatedDrawCount);
    }
};

TEST_CASE(
    "safe rock checkpoints preserve borrowed boss texture clocks across corrections and holds",
    "[safe-rocks-local-rewind][assets]") {
    PlagueFixture host;
    PlagueFixture guest;
    const auto uploads = guest.device.texturesCreated;
    const auto updates = guest.device.textureUpdates;
    std::array<bool, 15> seen{};
    SafeRocks::LocalSnapshot state;
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(tick);
        host.rocks.update(1.0f / 60);
        guest.rocks.update(1.0f / 60);
        if (tick % 11 == 0) {
            host.rocks.capturePresentation();
            guest.rocks.capturePresentation();
        }
        guest.rocks.saveLocal(state);
        CHECK(guest.digest() == host.digest());
        const auto clock = host.clock();
        const u32 frame = (clock.frame / 3) % 15;
        seen[frame] = true;
        host.drawFrame(frame, -1);
        guest.drawFrame(frame, -1);
        // Independent archives have distinct texture pointers, but resolve the
        // same exact borrowed native image. Comparing pixels verifies the lender.
        for (usize index = 0; index < host.device.draws.size(); ++index) {
            const auto& a =
                dynamic_cast<const test::FakeTexture&>(*host.device.draws[index].texture);
            const auto& b =
                dynamic_cast<const test::FakeTexture&>(*guest.device.draws[index].texture);
            CHECK((a.pixels == b.pixels));
            CHECK(host.device.draws[index].vertices == guest.device.draws[index].vertices);
        }
        guest.rocks.update(0.071f);
        guest.rocks.hideForEruptions();
        REQUIRE(guest.rocks.restoreLocal(state));
        CHECK(holds(guest.rocks, state));
        guest.drawFrame(frame, -1);
        if (clock.advance == 0) {
            for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
                guest.drawFrame(frame, alpha);
            }
        }
    }
    CHECK(std::ranges::all_of(seen, [](bool present) { return present; }));
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.device.textureUpdates == updates);
}

TEST_CASE("native cover history rewinds damage party gates and pending eruptions by value",
          "[local-rewind][safe-rocks-local-rewind]") {
    Fixture reference("safe-rock-native-reference");
    Fixture corrected("safe-rock-native-corrected");
    LocalStateHistory<SafeRocks::LocalSnapshot> history;
    constexpr f32 kSeconds = 1.0f / 60;
    std::array<SafeRocks::LocalSnapshot, 8> expectedLocal;
    SafeRocks::LocalSnapshot observed;
    s32 tick = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        const auto start = static_cast<u64>(tick);
        REQUIRE(history.capture(start, [&](auto& saved) {
            corrected.rocks.saveLocal(saved);
            return true;
        }));
        const s32 distance = 1 + window % 8;
        const auto* saved = history.find(start);
        REQUIRE(saved);
        for (s32 offset = 0; offset < distance; ++offset) {
            mutate(corrected.rocks, tick + offset, kSeconds);
            corrected.rocks.saveLocal(expectedLocal[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.rocks.restoreLocal(*saved));
        for (s32 offset = 0; offset < distance; ++offset) {
            mutate(reference.rocks, tick + offset, kSeconds);
            corrected.rocks.activate(static_cast<usize>(offset % 4));
            corrected.rocks.setPlayerCount(4);
            corrected.rocks.hideForEruptions();
            corrected.rocks.scheduleActivation(1, 0.01f);
            corrected.rocks.update(kSeconds);
        }
        REQUIRE(corrected.rocks.restoreLocal(*saved));
        CHECK(holds(corrected.rocks, *saved));
        for (s32 offset = 0; offset < distance; ++offset) {
            mutate(corrected.rocks, tick + offset, kSeconds);
            corrected.rocks.saveLocal(observed);
            CHECK(corrected.rocks.equalLocal(observed, expectedLocal[static_cast<usize>(offset)]));
        }
        CHECK(corrected.digest() == reference.digest());
        checkTargets(corrected.rocks.arenaTargets(), reference.rocks.arenaTargets());
        checkTargets(corrected.rocks.eruptionTargets(), reference.rocks.eruptionTargets());
        CHECK(corrected.rocks.attackAnchors() == reference.rocks.attackAnchors());
        CHECK(corrected.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}) ==
              reference.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}));
        tick += distance;
    }
}

TEST_CASE("native cover snapshots reject foreign owners and replaced arena bindings",
          "[local-rewind][safe-rocks-local-rewind]") {
    Fixture first("safe-rock-native-fences-first");
    Fixture second("safe-rock-native-fences-second");
    SafeRocks::LocalSnapshot saved;
    CHECK_FALSE(first.rocks.restoreLocal(saved));
    CHECK_FALSE(first.rocks.equalLocal(saved, saved));
    first.rocks.saveLocal(saved);
    SafeRocks::LocalSnapshot before;
    second.rocks.saveLocal(before);
    CHECK_FALSE(second.rocks.restoreLocal(saved));
    CHECK_FALSE(second.rocks.equalLocal(saved, saved));
    CHECK(holds(second.rocks, before));
    first.rocks.bindAnimations(first.device, first.items);
    CHECK_FALSE(first.rocks.restoreLocal(saved));
    CHECK_FALSE(first.rocks.equalLocal(saved, saved));
    first.rocks.saveLocal(saved);
    first.rocks.clear();
    CHECK_FALSE(first.rocks.restoreLocal(saved));
    REQUIRE(first.rocks.bind(first.device, first.layout, first.items));
    CHECK_FALSE(first.rocks.restoreLocal(saved));
}

TEST_CASE("native cover equality distinguishes dormant activation and subframe texture clocks",
          "[local-rewind][safe-rocks-local-rewind][native-equality]") {
    Fixture fixture("safe-rock-local-equality");
    fixture.rocks.hideForEruptions();
    SafeRocks::LocalSnapshot initial;
    SafeRocks::LocalSnapshot scheduled;
    SafeRocks::LocalSnapshot advanced;
    SafeRocks::LocalSnapshot restored;
    fixture.rocks.saveLocal(initial);
    const auto obstacle = fixture.rocks.rock(0).obstacle;
    const auto placement = fixture.rocks.rock(0).placement;
    fixture.rocks.scheduleActivation(0, 0.5f);
    fixture.rocks.saveLocal(scheduled);
    CHECK(fixture.rocks.rock(0).obstacle == obstacle);
    CHECK(fixture.rocks.rock(0).placement == placement);
    CHECK_FALSE(fixture.rocks.equalLocal(initial, scheduled));
    REQUIRE(fixture.rocks.restoreLocal(initial));
    fixture.rocks.update(1.0f / 240);
    fixture.rocks.saveLocal(advanced);
    CHECK(fixture.rocks.textureClock() == 0); // same displayed frame, different fractional clock
    CHECK_FALSE(fixture.rocks.equalLocal(initial, advanced));
    REQUIRE(fixture.rocks.restoreLocal(initial));
    fixture.rocks.saveLocal(restored);
    CHECK(fixture.rocks.equalLocal(initial, restored));
}

TEST_CASE("native cover rewind retains shared boss texture animation and model bindings",
          "[local-rewind][safe-rocks-local-rewind][assets]") {
    PlagueFixture reference;
    PlagueFixture corrected;
    LocalStateHistory<SafeRocks::LocalSnapshot> history;
    const auto resources = corrected.rocks.bodyResources();
    const auto uploads = corrected.device.texturesCreated;
    const auto updates = corrected.device.textureUpdates;
    u64 tick = 0;
    auto advance = [](SafeRocks& rocks, u64 step) {
        rocks.update(1.0f / 60);
        if (step % 11 == 0) {
            rocks.capturePresentation();
        }
    };
    for (u64 window = 0; window < 72; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.rocks.saveLocal(saved);
            return true;
        }));
        const u64 distance = 1 + window % 8;
        for (u64 offset = 0; offset < distance; ++offset) {
            advance(reference.rocks, tick + offset);
            corrected.rocks.hideForEruptions();
            corrected.rocks.update(0.13f);
        }
        const auto* saved = history.find(tick);
        REQUIRE(saved);
        REQUIRE(corrected.rocks.restoreLocal(*saved));
        for (u64 offset = 0; offset < distance; ++offset) {
            advance(corrected.rocks, tick + offset);
        }
        CHECK(corrected.digest() == reference.digest());
        const u32 frame = (reference.clock().frame / 3) % 15;
        reference.drawFrame(frame, -1);
        corrected.drawFrame(frame, -1);
        REQUIRE(reference.device.draws.size() == corrected.device.draws.size());
        for (usize index = 0; index < reference.device.draws.size(); ++index) {
            CHECK(reference.device.draws[index].vertices == corrected.device.draws[index].vertices);
        }
        const auto retained = corrected.rocks.bodyResources();
        REQUIRE(retained.size() == resources.size());
        for (usize index = 0; index < resources.size(); ++index) {
            CHECK(retained[index].model == resources[index].model);
            CHECK(retained[index].textures == resources[index].textures);
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == uploads);
    CHECK(corrected.device.textureUpdates == updates);
}
} // namespace
