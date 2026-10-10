#include <algorithm>
#include <random>
#include <ranges>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/CombatReplica.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
constexpr f32 kStep = 1.0f / 30;

CombatSnapshot capture(const WorldScene& scene, u64 tick) {
    CombatSnapshot snapshot;
    snapshot.motion.epoch = 1;
    snapshot.motion.tick = tick;
    snapshot.motion.cameraContinuity = 1;
    snapshot.geometry = scene.geometry();
    REQUIRE(snapshot.valid());
    return snapshot;
}

void transfer(CombatReplica& receiver, WorldScene& display, const CombatSnapshot& state) {
    const auto packets = CombatReplica::packets(state);
    REQUIRE(packets);
    // Network order does not determine which nodes are applied first.
    for (usize i = packets->size(); i > 0; --i) {
        REQUIRE(receiver.receive(1, (*packets)[i - 1]) ==
                (i == 1 ? CombatReplica::Admission::Committed : CombatReplica::Admission::Pending));
    }
    REQUIRE(receiver.latest());
    REQUIRE(receiver.latest()->geometry);
    REQUIRE(display.applyGeometry(*receiver.latest()->geometry));
    CHECK(CombatPacket::encode(capture(display, state.motion.tick)) == CombatPacket::encode(state));
}

TEST_CASE("Maze rising platforms replicate atomically and recover without client triggers",
          "[netplay][geometry-replica][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("J4");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld host;
    LevelWorld client;
    REQUIRE(host.load(device, root, *level));
    REQUIRE(client.load(device, root, *level));
    host.startTriggers({});
    auto display = client.scene();
    CombatReplica receiver;
    REQUIRE(receiver.begin(1, 1));
    transfer(receiver, display, capture(host.scene(), 1));
    const Mat4 clientFloor = client.scene().worldTransform(7);
    const f32 initialY = display.worldTransform(7)[3].y;
    const LevelTrigger* fountain = nullptr;
    for (usize i = 0; i < host.triggers().size(); ++i) {
        if (host.triggers().trigger(i).instance == 417) {
            fountain = &host.triggers().trigger(i);
        }
    }
    REQUIRE(fountain != nullptr);
    const std::array visitors{TriggerVisitor{.position = fountain->spot}};
    host.updateTriggers(kStep, visitors);
    REQUIRE(host.triggers().opened(7));
    for (s32 frame = 0; frame < 60; ++frame) {
        host.update(kStep);
        host.updateTriggers(kStep, {});
    }
    const auto partial = CombatReplica::packets(capture(host.scene(), 2));
    REQUIRE(partial);
    REQUIRE(partial->size() > 1);
    for (usize i = 1; i < partial->size(); ++i) {
        REQUIRE(receiver.receive(1, (*partial)[i]) == CombatReplica::Admission::Pending);
    }
    CHECK(receiver.latest()->motion.tick == 1);
    CHECK(display.worldTransform(7)[3].y == initialY);
    for (s32 frame = 0; frame < 120; ++frame) {
        host.update(kStep);
        host.updateTriggers(kStep, {});
    }
    const auto textures = device.texturesCreated;
    transfer(receiver, display, capture(host.scene(), 3));
    CHECK(display.worldTransform(7)[3].y == Approx(initialY + 43).margin(0.02f));
    CHECK(receiver.receive(1, partial->front()) == CombatReplica::Admission::Stale);
    CHECK(client.scene().worldTransform(7) == clientFloor);
    CHECK_FALSE(client.triggers().opened(7));
    CHECK(device.texturesCreated == textures);
}

TEST_CASE("Temple unkeyed height triggers replicate their actual platform transforms",
          "[netplay][geometry-replica][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.startTriggers({});
    auto display = world.scene();
    CombatReplica receiver;
    REQUIRE(receiver.begin(1, 1));
    transfer(receiver, display, capture(world.scene(), 1));
    usize tested = 0;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        const auto& instance = world.layout().itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = world.layout().itemInfos()[static_cast<usize>(instance.info)];
        if (info.subtype != 26) {
            continue;
        }
        REQUIRE(trigger.target >= 0);
        const auto target = static_cast<usize>(trigger.target);
        const f32 initial = world.scene().worldTransform(target)[3].y;
        Vec3 spot = trigger.spot;
        for (usize parent = 0; parent < world.triggers().size(); ++parent) {
            if (world.triggers().trigger(parent).next == static_cast<s32>(i)) {
                spot = world.triggers().trigger(parent).spot;
            }
        }
        const std::array visitors{TriggerVisitor{.position = spot}};
        world.updateTriggers(kStep, visitors);
        world.updateTriggers(2, {});
        REQUIRE(trigger.fired);
        transfer(receiver, display, capture(world.scene(), 2 + tested));
        CHECK(display.worldTransform(target)[3].y == Approx(initial + 5));
        ++tested;
    }
    CHECK(tested > 0);
}

TEST_CASE("every native level fits the bounded geometry checkpoint and restores its moving nodes",
          "[netplay][geometry-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::set<std::string> names{"L1"};
    for (const auto& realm : catalog.realms()) {
        names.insert(realm.levels.begin(), realm.levels.end());
    }
    REQUIRE(names.size() > 1);
    usize maximum = 0;
    usize rawBytes = 0;
    usize wireBytes = 0;
    usize repairedBytes = 0;
    usize maxWireBytes = 0;
    std::string largest;
    for (const auto& name : names) {
        CAPTURE(name);
        const auto level = name == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(name);
        REQUIRE(level);
        test::assetOrSkip(level->directory + "/WORLDS.PS2");
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root, *level));
        auto display = world.scene();
        CombatReplica receiver;
        REQUIRE(receiver.begin(1, 1));
        world.startTriggers({});
        for (u64 tick = 1; tick <= 3; ++tick) {
            if (tick < 3) {
                world.update(kStep);
                world.updateTriggers(kStep, {});
            }
            auto source = world.scene();
            if (tick == 3) {
                // Capacity must also cover every mutable still node, not only
                // the moving nodes visible at entry. Batched geometry ignores these.
                for (usize i = 0; i < world.layout().objects().size(); ++i) {
                    source.setObjectAlpha(i, 0.5f);
                    source.setObjectVisible(i, false);
                }
            }
            const auto state = capture(source, tick);
            const auto raw = CombatPacket::encode(state);
            const auto wire = CombatReplica::packets(state);
            const auto repaired = CombatReplica::packets(
                state, SnapshotBlock::Compression::Automatic, CombatReplica::Recovery::SingleLoss);
            REQUIRE(raw);
            REQUIRE(wire);
            REQUIRE(repaired);
            usize size = 0;
            for (const auto& packet : *wire) {
                size += packet.size();
            }
            rawBytes += raw->size();
            wireBytes += size;
            for (const auto& packet : *repaired) {
                repairedBytes += packet.size();
            }
            maxWireBytes = std::max(maxWireBytes, size);
            transfer(receiver, display, state);
            for (usize i = 0; i < world.layout().objects().size(); ++i) {
                if (world.scene().moving(i)) {
                    CHECK(display.worldTransform(i) == world.scene().worldTransform(i));
                }
            }
            if (state.geometry->objects.size() > maximum) {
                maximum = state.geometry->objects.size();
                largest = name;
            }
        }
    }
    WARN("Geometry census: " << names.size() << " levels; largest checkpoint " << largest
                             << " with " << maximum << " records; raw " << rawBytes
                             << " bytes, compressed wire " << wireBytes
                             << " bytes, including repair " << repairedBytes
                             << " bytes, largest wire checkpoint " << maxWireBytes << " bytes");
    CHECK(maximum > 0);
    CHECK(wireBytes < rawBytes / 2);
    CHECK(repairedBytes < rawBytes / 2);
}

TEST_CASE("dense native geometry remains atomic through packet loss and reordering",
          "[netplay][geometry-replica][assets]") {
    auto recovery = CombatReplica::Recovery::None;
    SECTION("independent compressed checkpoints") {}
    SECTION("compressed checkpoints with single loss repair") {
        recovery = CombatReplica::Recovery::SingleLoss;
    }
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.startTriggers({});
    auto display = world.scene();
    CombatReplica receiver;
    REQUIRE(receiver.begin(1, 1));
    struct Delivery {
        u64 due;
        std::vector<u8> bytes;
    };
    std::vector<Delivery> queue;
    std::map<u64, std::vector<u8>> expected;
    std::mt19937 random(91);
    usize sent = 0;
    usize dropped = 0;
    usize committed = 0;
    usize largest = 0;
    u64 latest = 0;
    u64 longestGap = 0;
    // Eight seconds at 60 Hz, publishing at 20 Hz, with 33-67 ms delay,
    // independently scheduled 10% packet loss, reordering and duplicates.
    for (u64 tick = 1; tick <= 484; ++tick) {
        if (tick <= 480) {
            world.update(1.0f / 60);
            world.updateTriggers(1.0f / 60, {});
            if (tick % 3 == 0) {
                auto source = world.scene();
                for (usize i = 0; i < world.layout().objects().size(); ++i) {
                    // Include all mutable still nodes as well as actual native
                    // moving transforms; this is the census's largest roster.
                    source.setObjectAlpha(i, 0.5f);
                    source.setObjectVisible(i, false);
                }
                const auto state = capture(source, tick);
                largest = std::max(largest, state.geometry->objects.size());
                const auto raw = CombatPacket::encode(state);
                const auto packets =
                    CombatReplica::packets(state, SnapshotBlock::Compression::Automatic, recovery);
                REQUIRE(raw);
                REQUIRE(packets);
                expected.emplace(tick, *raw);
                for (const auto& packet : *packets) {
                    ++sent;
                    if (random() % 10 == 0) {
                        ++dropped;
                        continue;
                    }
                    queue.push_back({tick + 2 + random() % 3, packet});
                    if (sent % 17 == 0) {
                        queue.push_back({tick + 3, packet});
                    }
                }
            }
        }
        // Reverse arrival within a due tick as well as varying each delay.
        for (const auto& packet : std::views::reverse(queue)) {
            if (packet.due > tick) {
                continue;
            }
            const auto admission = receiver.receive(1, packet.bytes);
            REQUIRE(admission != CombatReplica::Admission::Invalid);
            if (admission == CombatReplica::Admission::Committed) {
                REQUIRE(receiver.latest());
                const auto& state = *receiver.latest();
                CHECK(CombatPacket::encode(state) == expected.at(state.motion.tick));
                REQUIRE(display.applyGeometry(*state.geometry));
                CHECK(CombatPacket::encode(capture(display, state.motion.tick)) ==
                      expected.at(state.motion.tick));
                longestGap = std::max(longestGap, tick - latest);
                latest = tick;
                ++committed;
            }
            REQUIRE(receiver.pending() <= CombatReplica::kPendingSnapshots);
        }
        std::erase_if(queue, [tick](const Delivery& packet) { return packet.due <= tick; });
    }
    WARN("Dense native loss test: "
         << (recovery == CombatReplica::Recovery::SingleLoss ? "repair; " : "no repair; ")
         << largest << " records; " << dropped << '/' << sent << " packets dropped; " << committed
         << "/160 checkpoints committed; longest update gap " << longestGap << " ticks at 60 Hz");
    CHECK(largest >= 800);
    CHECK(dropped > 0);
    CHECK(committed > 0);
    CHECK(committed < 160);
    if (recovery == CombatReplica::Recovery::SingleLoss) {
        CHECK(committed >= 80);
        CHECK(longestGap <= 30);
    }
    CHECK(queue.empty());
    // Recovery never depends on a lost baseline or a reliable retransmission.
    transfer(receiver, display, capture(world.scene(), 485));
}
} // namespace
