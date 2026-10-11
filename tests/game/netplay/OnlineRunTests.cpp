#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/OnlineRun.h"
#include "game/screens/GameContext.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("menu-created iroh rooms select characters ready and enter the shared tower",
          "[netplay][online-lobby][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const GameConfig config;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    OnlineRun host;
    OnlineRun guest;
    REQUIRE(host.openLobby(hostDevice, context, {}, "test", std::string(64, 'a'), 1, true));
    const auto pump = [&](const auto& finished) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished() && std::chrono::steady_clock::now() < deadline) {
            host.update({}, {});
            guest.update({}, {});
            hostDevice.draws.clear();
            guestDevice.draws.clear();
            host.render(hostDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            guest.render(guestDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(finished());
    };
    pump([&] { return !host.lobby().invitation.empty(); });
    CHECK_FALSE(host.lobby().selected);
    CHECK_FALSE(host.lobby().canStart);
    REQUIRE_FALSE(host.start());
    REQUIRE(guest.openLobby(guestDevice, context, host.lobby().invitation, "test",
                            std::string(64, 'a'), 1, true));
    pump([&] { return guest.lobby().connected && host.lobby().players == 2; });
    CHECK_FALSE(host.ready(true));
    PartyMember first;
    first.player = 0;
    first.save.name = "HOST";
    first.save.progress().health = 1000;
    auto second = first;
    second.player = 2;
    second.save.name = "GUEST";
    second.save.color = 1;
    REQUIRE(host.select(std::array{first}));
    REQUIRE(guest.select(std::array{second}));
    REQUIRE(host.settings({2, 2, 1}));
    pump([&] { return guest.lobby().settings == RoomSettings{2, 2, 1}; });
    REQUIRE(host.ready(true));
    REQUIRE(guest.ready(true));
    pump([&] { return host.lobby().canStart; });
    REQUIRE(host.start());
    pump([&] {
        return host.playing() && guest.playing() && host.status().empty() && guest.status().empty();
    });
    REQUIRE(host.pause(0));
    for (s32 tick = 0; tick < 120; ++tick) {
        host.update({}, {});
        guest.update({}, {});
    }
    CHECK(host.status() == "Local menu open - the game continues for everyone.");
    CHECK(guest.status().empty());
    host.resume();
    host.update({}, {});
    CHECK(host.status().empty());
}
