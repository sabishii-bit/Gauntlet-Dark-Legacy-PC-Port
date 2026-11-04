#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/NativeDigestProfileSupport.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct CameraFixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;

    explicit CameraFixture(bool wide) {
        device.recordDraws = false;
        if (wide) {
            config.camera.horizontalFovDegrees = 85;
            config.display.frameWidth = 960;
            config.display.frameHeight = 540;
        }
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName("G1");
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        std::array<PartyMember, 2> party;
        for (usize seat = 0; seat < party.size(); ++seat) {
            CharacterSave save;
            save.character = static_cast<s32>(seat);
            save.color = static_cast<s32>(seat);
            save.progress().health = 2000;
            party[seat] = {static_cast<s32>(seat), save};
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }

    Mat4 render(f32 blend = 1) {
        const auto width = static_cast<f32>(config.display.frameWidth);
        const auto height = static_cast<f32>(config.display.frameHeight);
        // NOLINTNEXTLINE(readability-suspicious-call-argument): the screen's y axis points down
        const auto projection = glm::ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
        REQUIRE(device.beginFrame());
        scene.render(device, projection, width, height, false, blend);
        device.endFrame();
        return scene.viewCamera().clipTransform(config.horizontalFovRadians(), width, height,
                                                projection);
    }
};

void sameCamera(const WorldCamera& left, const WorldCamera& right) {
    CHECK(left.position == right.position);
    CHECK(left.pitch == right.pitch);
    CHECK(left.yaw == right.yaw);
    CHECK(left.roll == right.roll);
}

void sameNative(CameraFixture& left, CameraFixture& right) {
    PlayScene::NativeDigestProfile first;
    PlayScene::NativeDigestProfile second;
    REQUIRE(left.scene.nativeAgreementDigest(&first));
    REQUIRE(right.scene.nativeAgreementDigest(&second));
    CHECK(test::firstDivergence(first, second).empty());
}

Vec2 cursorFor(const Mat4& clip, const Vec3& target) {
    const Vec4 projected = clip * Vec4{target, 1};
    REQUIRE(projected.w > 0);
    const Vec2 cursor = (Vec2{projected} / projected.w + Vec2{1}) * 0.5f;
    REQUIRE(cursor.x >= 0);
    REQUIRE(cursor.x <= 1);
    REQUIRE(cursor.y >= 0);
    REQUIRE(cursor.y <= 1);
    return cursor;
}
} // namespace

TEST_CASE("native gameplay camera is independent of local FOV aspect and rendering cadence",
          "[simulation-camera][assets]") {
    const u32 renderEvery = GENERATE(0U, 2U);
    CAPTURE(renderEvery);
    auto first = std::make_unique<CameraFixture>(false);
    auto second = std::make_unique<CameraFixture>(true);
    const CameraView canonical;
    CHECK(first->scene.cameraView().horizontalFov == canonical.horizontalFov);
    CHECK(first->scene.cameraView().aspect == canonical.aspect);
    CHECK(second->scene.cameraView().horizontalFov == canonical.horizontalFov);
    CHECK(second->scene.cameraView().aspect == canonical.aspect);
    sameNative(*first, *second);

    u32 movingTicks = 0;
    usize mostEnemies = 0;
    for (u32 tick = 1; tick <= 420; ++tick) {
        CAPTURE(tick);
        if (tick == 180) {
            // A local settings change during arrival cannot refit the shared
            // camera or change visibility/placement activation on the next tick.
            second->config.camera.horizontalFovDegrees = 100;
            second->config.display.frameWidth = 1260;
            second->config.display.frameHeight = 540;
        }
        PlayScene::Inputs inputs;
        if (tick > 360) {
            inputs[0].move = {{1, 0}, 0.5f};
            inputs[1].move = {{-1, 0}, 0.5f};
        }
        const auto before = first->scene.actor(0)->position();
        REQUIRE(first->scene.update(1.0 / 60, inputs) == PlayOutcome::Running);
        REQUIRE(second->scene.update(1.0 / 60, inputs) == PlayOutcome::Running);
        if (renderEvery != 0 && tick % renderEvery == 0) {
            second->render(0.25f);
        }
        movingTicks += before != first->scene.actor(0)->position() ? 1U : 0U;
        mostEnemies = std::max(mostEnemies, first->scene.enemies().count());
        sameCamera(first->scene.camera().camera(), second->scene.camera().camera());
        sameCamera(first->scene.viewCamera(), second->scene.viewCamera());
        CHECK(first->scene.camera().attention() == second->scene.camera().attention());
        for (s32 seat = 0; seat < 2; ++seat) {
            CHECK(first->scene.actor(seat)->position() == second->scene.actor(seat)->position());
            CHECK(first->scene.actor(seat)->yaw() == second->scene.actor(seat)->yaw());
        }
        if (tick % 30 == 0) {
            sameNative(*first, *second);
        }
    }
    REQUIRE_FALSE(first->scene.spawning());
    REQUIRE(movingTicks > 0);
    REQUIRE(mostEnemies > 0);
    CHECK(second->device.frames == (renderEvery != 0 ? 420U / renderEvery : 0U));
    CHECK(first->device.frames == 0);
}

TEST_CASE("local cursor projection resolves once into a render-independent world command",
          "[simulation-camera][cursor][assets]") {
    auto first = std::make_unique<CameraFixture>(false);
    auto second = std::make_unique<CameraFixture>(true);
    for (u32 tick = 0; tick < 360; ++tick) {
        REQUIRE(first->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        REQUIRE(second->scene.update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(first->scene.spawning());
    REQUIRE_FALSE(second->scene.spawning());
    const auto origin = first->scene.actor(0)->position();
    REQUIRE(origin == second->scene.actor(0)->position());
    const auto target = origin + Vec3{1, 0, 1};
    const auto firstCursor = cursorFor(first->render(), target);
    const auto secondCursor = cursorFor(second->render(), target);
    CHECK(glm::distance(firstCursor, secondCursor) > 0.001f);
    const auto resolved = first->scene.cursorAim(firstCursor, origin);
    const auto otherViewAim = second->scene.cursorAim(secondCursor, origin);
    REQUIRE(resolved);
    REQUIRE(otherViewAim);
    CHECK(glm::distance(*resolved, glm::normalize(target - origin)) == Approx(0).margin(0.001));
    CHECK(glm::distance(*otherViewAim, glm::normalize(target - origin)) == Approx(0).margin(0.001));

    PlayInput sampled;
    sampled.aimDirection = resolved;
    const std::array commands{SessionInputs::command(sampled, 1, 361, 1, 0)};
    const auto encoded = InputPacket::encode(commands);
    REQUIRE(encoded);
    const auto decoded = InputPacket::decode(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    REQUIRE(decoded->front().aimDirection == resolved);
    PlayScene::Inputs received;
    received[0] = SessionInputs::playInput(decoded->front());

    // Replace only one peer's presented projection after sampling. Replaying the
    // recorded command must not consult either peer's new/stale cursor history.
    second->config.camera.horizontalFovDegrees = 110;
    second->render(0);
    for (u32 tick = 0; tick < 12; ++tick) {
        REQUIRE(first->scene.update(1.0 / 60, received) == PlayOutcome::Running);
        REQUIRE(second->scene.update(1.0 / 60, received) == PlayOutcome::Running);
        if (tick % 2 == 0) {
            second->render(0.5f);
        }
        CHECK(first->scene.actor(0)->yaw() == second->scene.actor(0)->yaw());
        CHECK(first->scene.actor(0)->yaw() ==
              Approx(std::atan2(resolved->x, resolved->z)).margin(0.001));
        sameCamera(first->scene.camera().camera(), second->scene.camera().camera());
    }
    sameNative(*first, *second);
}
