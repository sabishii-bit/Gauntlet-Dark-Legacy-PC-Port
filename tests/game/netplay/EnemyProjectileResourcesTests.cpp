#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/ReplicaProjectiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("swarm projectile resources are independent of load order and preserve native shots",
          "[netplay][enemy-projectile-resources][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/objects.ngc").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies hostEnemies;
    Enemies clientEnemies;
    hostEnemies.open(device, root, nullptr, 64, {}, 1);
    clientEnemies.open(device, root, nullptr, 64, {}, 1);
    // EnemyKinds is the fixed native roster; special great creatures use their
    // CombatantAssets registry, not the swarm loader's tiered monster archives.
    for (s32 kind = 0; kind < kSwarmKindCount; ++kind) {
        CAPTURE(kind);
        REQUIRE(hostEnemies.loadKind(kind));
    }
    for (s32 kind = kSwarmKindCount - 1; kind >= 0; --kind) {
        CAPTURE(kind);
        REQUIRE(clientEnemies.loadKind(kind));
    }
    ProjectileResources host;
    ProjectileResources client;
    REQUIRE(host.addEnemies(device, hostEnemies));
    REQUIRE(client.addEnemies(device, clientEnemies));
    CHECK_FALSE(host.addEnemies(device, hostEnemies));
    CHECK(hostEnemies.projectileModel(kGruntKind, -1) == nullptr);
    CHECK(hostEnemies.projectileModel(kGruntKind, 3) == nullptr);
    CHECK(hostEnemies.projectileModel(-1, 0) == nullptr);
    usize tested = 0;
    const PlayerMissiles players;
    for (s32 kind = 0; kind < kSwarmKindCount; ++kind) {
        for (s32 slot = 0; slot < 3; ++slot) {
            CAPTURE(kind, slot);
            const auto* model = hostEnemies.projectileModel(kind, slot);
            if (model == nullptr) {
                CHECK(clientEnemies.projectileModel(kind, slot) == nullptr);
                continue;
            }
            const auto* peerModel = clientEnemies.projectileModel(kind, slot);
            REQUIRE(peerModel != nullptr);
            CHECK(host.modelId(model) == client.modelId(peerModel));
            EnemyMissiles missiles;
            missiles.launch(EnemyMissileKind::bolt(20, 25, 0.5f), {0, 5, 0}, {0, 5, 20}, 1, model,
                            0);
            missiles.update(0.1f, nullptr, {});
            CombatSnapshot state;
            state.motion.epoch = 1;
            state.motion.cameraContinuity = 1;
            REQUIRE(ProjectileCapture::append(state, host, players, missiles));
            REQUIRE(state.projectiles.size() == 1);
            ReplicaProjectiles replica;
            REQUIRE(replica.begin(1));
            REQUIRE(replica.show(state, client));
            const auto textures = device.texturesCreated;
            device.draws.clear();
            replica.draw(device, client, Mat4{1}, {}, CameraFrame::at({10, 20, 30}));
            CHECK_FALSE(device.draws.empty());
            CHECK(device.texturesCreated == textures);
            for (const auto& draw : device.draws) {
                CHECK(draw.state.depthTest);
            }
            ++tested;
        }
    }
    CHECK(tested >= 20);
    CHECK(hostEnemies.count() == 0);
    CHECK(clientEnemies.count() == 0);
    CHECK(clientEnemies.takeFeedback().empty());
}
} // namespace
