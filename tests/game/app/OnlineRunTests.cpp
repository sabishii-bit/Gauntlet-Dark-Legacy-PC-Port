#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/SoundPlayer.h"
#include "engine/platform/Paths.h"
#include "engine/platform/Window.h"
#include "engine/render/ThreadedRenderDevice.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/NativeOnlineIdentity.h"
#include "game/app/OnlineRun.h"
#include "game/characters/Party.h"
#include "game/context/GameServices.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void exerciseLobby(RenderDevice& hostDevice, RenderDevice& guestDevice,
                   const std::function<void()>& frameFinished) {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const GameConfig config;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    GameServices context;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    context.strings = &strings;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    context.sounds = &sounds;
    context.movieMixer = &mixer;
    OnlineRun host;
    OnlineRun guest;
    REQUIRE(host.openLobby(hostDevice, context, OnlineRun::Links{}, {}, "test",
                           std::string(64, 'a'), 1, true));
    const auto pump = [&](const auto& finished) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished() && std::chrono::steady_clock::now() < deadline) {
            host.update({}, {});
            guest.update({}, {});
            REQUIRE(hostDevice.beginFrame());
            REQUIRE(guestDevice.beginFrame());
            host.render(hostDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            guest.render(guestDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
            hostDevice.endFrame();
            guestDevice.endFrame();
            hostDevice.waitIdle();
            guestDevice.waitIdle();
            frameFinished();
            std::array<f32, 1600> audio{};
            sounds.update();
            mixer.mix(audio);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(finished());
    };
    pump([&] { return !host.lobby().invitation.empty(); });
    CHECK_FALSE(host.lobby().selected);
    CHECK_FALSE(host.lobby().canStart);
    REQUIRE_FALSE(host.start());
    REQUIRE(guest.openLobby(guestDevice, context, OnlineRun::Links{}, host.lobby().invitation,
                            "test", std::string(64, 'a'), 1, true));
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
    CHECK_FALSE(host.settings({2, 2, 1, 1, 1})); // a mechanic this build does not offer
    REQUIRE(host.settings({2, 2, 1, 1, 0}));
    pump([&] { return guest.lobby().settings == RoomSettings{2, 2, 1, 1, 0}; });
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
    REQUIRE(guest.pause(2));
    guest.update({}, {});
    CHECK(guest.status() == "Local menu open - the game continues for everyone.");
    guest.resume();
    guest.update({}, {});
    CHECK(guest.status().empty());
    hostDevice.waitIdle();
    guestDevice.waitIdle();
}

TEST_CASE("the network line reads the ping alone", "[netplay][online-run]") {
    CHECK(OnlineRun::networkSummary(12) == "12 ms");
    CHECK(OnlineRun::networkSummary(140) == "140 ms");
    CHECK(OnlineRun::networkSummary(-1) == "ping ?");
}

TEST_CASE("a stall is announced after a second with the seconds left to its deadline",
          "[netplay][online-run]") {
    CHECK_FALSE(OnlineRun::stallNotice(0.0));
    CHECK_FALSE(OnlineRun::stallNotice(0.9));
    CHECK(OnlineRun::stallNotice(1.0) == "Waiting for another player... 14s");
    CHECK(OnlineRun::stallNotice(7.5) == "Waiting for another player... 8s");
    CHECK(OnlineRun::stallNotice(14.99) == "Waiting for another player... 1s");
    CHECK(OnlineRun::stallNotice(20.0) == "Waiting for another player... 0s");
}

TEST_CASE("menu-created iroh rooms select characters ready and enter the shared tower",
          "[netplay][online-lobby][assets][serial]") {
    REQUIRE(NativeOnlineIdentity::prepareProcessNumeric());
    const bool threaded = GENERATE(false, true);
    auto hostBackend = std::make_unique<test::FakeRenderDevice>();
    auto guestBackend = std::make_unique<test::FakeRenderDevice>();
    auto& host = *hostBackend;
    auto& guest = *guestBackend;
    std::unique_ptr<RenderDevice> hostRenderer = std::move(hostBackend);
    std::unique_ptr<RenderDevice> guestRenderer = std::move(guestBackend);
    if (threaded) {
        hostRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(hostRenderer), Extent2D{640, 448});
        guestRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(guestRenderer), Extent2D{640, 448});
    }
    exerciseLobby(*hostRenderer, *guestRenderer, [&] {
        host.draws.clear();
        guest.draws.clear();
    });
    // Clearing the loading or pause notice must remove its texture, not upload
    // the zero-sized image returned by the font renderer for empty text.
    CHECK(host.emptyTexturesCreated == 0);
    CHECK(guest.emptyTexturesCreated == 0);
}

TEST_CASE("online lobby starts and resumes on host and guest Vulkan renderers",
          "[netplay][online-lobby-gpu][gpu][assets]") {
    REQUIRE(NativeOnlineIdentity::prepareProcessNumeric());
    (void)test::assetOrSkip("WEAPONS/ANIM.PS2");
    const bool threaded = GENERATE(false, true);
    REQUIRE(glfwInit() == GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    auto hostWindow = createGlfwWindow({.title = "Online host test", .width = 640, .height = 448});
    auto guestWindow =
        createGlfwWindow({.title = "Online guest test", .width = 640, .height = 448});
    glfwDefaultWindowHints();
    RenderDeviceDesc desc;
    desc.enableValidation = true;
    desc.shaderDirectory = paths::executableDirectory() / "shaders";
    auto hostRenderer = createVulkanRenderDevice(*hostWindow, desc);
    auto guestRenderer = createVulkanRenderDevice(*guestWindow, desc);
    if (threaded) {
        hostRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(hostRenderer), Extent2D{640, 448});
        guestRenderer =
            std::make_unique<ThreadedRenderDevice>(std::move(guestRenderer), Extent2D{640, 448});
    }
    exerciseLobby(*hostRenderer, *guestRenderer, [&] { hostWindow->pollEvents(); });
}

TEST_CASE("three real iroh menu peers run four split-seat players and play on when one departs",
          "[netplay][online-lobby][online-lobby-multipeer][assets][serial]") {
    REQUIRE(NativeOnlineIdentity::prepareProcessNumeric());
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog levels;
    StringTable strings;
    REQUIRE(levels.load(root));
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    std::array<GameConfig, 3> configs;
    std::array<GameServices, 3> contexts;
    std::array<test::RecordingPartyStore, 3> stores;
    std::array<test::RecordingUnlocks, 3> unlocks;
    std::array<std::unique_ptr<test::FakeRenderDevice>, 3> devices;
    std::array<std::unique_ptr<OnlineRun>, 3> runs;
    for (usize peer = 0; peer < runs.size(); ++peer) {
        configs[peer].game.autoSave = true;
        contexts[peer].config = &configs[peer];
        contexts[peer].strings = &strings;
        contexts[peer].levels = &levels;
        contexts[peer].unpackedRoot = root;
        devices[peer] = std::make_unique<test::FakeRenderDevice>();
        devices[peer]->recordDraws = false;
        runs[peer] = std::make_unique<OnlineRun>();
    }
    std::array<SessionInputs::Frame, 3> input{};
    // These are physical local devices, not the canonical seats assigned by
    // HostedRoom. Exercise the real bootstrap's two-seat host plus two guests.
    input[0][0].movieSkipPressed = input[0][3].movieSkipPressed = true;
    input[1][2].movieSkipPressed = input[2][1].movieSkipPressed = true;
    std::array<bool, 3> active{true, true, true};
    auto due = std::chrono::steady_clock::now();
    const auto step = [&] {
        for (usize peer = 0; peer < runs.size(); ++peer) {
            if (!active[peer]) {
                continue;
            }
            runs[peer]->update(input[peer], {});
            REQUIRE(devices[peer]->beginFrame());
            runs[peer]->render(*devices[peer], Mat4{1}, 640, 448, 1.0 / 60, 1);
            devices[peer]->endFrame();
        }
        const auto now = std::chrono::steady_clock::now();
        due = std::max(due + std::chrono::nanoseconds(16'666'667), now);
        std::this_thread::sleep_until(due);
    };
    const auto pump = [&](const auto& finished, bool expectHealthy = true) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (!finished() && std::chrono::steady_clock::now() < deadline) {
            step();
            if (expectHealthy) {
                for (usize peer = 0; peer < runs.size(); ++peer) {
                    if (!active[peer]) {
                        continue;
                    }
                    INFO("peer=" << peer + 1 << " status=" << runs[peer]->status());
                    REQUIRE(runs[peer]->lobby().page != NetplayMenu::Page::Finished);
                }
            }
        }
        REQUIRE(finished());
    };
    const auto links = [&](usize peer) {
        return OnlineRun::Links{.unlocks = &unlocks[peer], .store = &stores[peer]};
    };
    REQUIRE(runs[0]->openLobby(*devices[0], contexts[0], links(0), {}, "test", std::string(64, 'a'),
                               2, true));
    pump([&] { return !runs[0]->lobby().invitation.empty(); });
    // The actual provider invitation stays private; never include it in INFO/CAPTURE.
    const auto invitation = runs[0]->lobby().invitation;
    for (usize peer = 1; peer < runs.size(); ++peer) {
        REQUIRE(runs[peer]->openLobby(*devices[peer], contexts[peer], links(peer), invitation,
                                      "test", std::string(64, 'a'), 1, true));
    }
    pump([&] {
        return std::ranges::all_of(runs, [](const auto& run) {
            return run->lobby().connected && run->lobby().players == 4;
        });
    });
    // The two guests link to each other directly, so their inputs need not cross the host.
    pump([&] { return runs[1]->directLinks() == 1 && runs[2]->directLinks() == 1; });
    CHECK(runs[0]->directLinks() == 0);
    const auto member = [](s32 device, s32 character, const char* name) {
        PartyMember result;
        result.player = device;
        result.slot = static_cast<usize>(10 + device);
        result.save.name = name;
        result.save.character = character;
        result.save.color = character;
        result.save.progress().health = 1000;
        return result;
    };
    REQUIRE(runs[0]->select(std::array{member(0, 0, "HOST"), member(3, 1, "LOCAL")}));
    REQUIRE(runs[1]->select(std::array{member(2, 2, "GUEST1")}));
    REQUIRE(runs[2]->select(std::array{member(1, 3, "GUEST2")}));
    const RoomSettings rules{4, 2, 1};
    REQUIRE(runs[0]->settings(rules));
    pump([&] {
        return std::ranges::all_of(runs,
                                   [&](const auto& run) { return run->lobby().settings == rules; });
    });
    for (const auto& run : runs) {
        REQUIRE(run->ready(true));
    }
    pump([&] { return runs[0]->lobby().canStart; });
    REQUIRE(runs[0]->start());
    pump([&] {
        return std::ranges::all_of(runs, [](const auto& run) {
            return run->playing() && run->status().empty() && run->confirmedTick() >= 90;
        });
    });
    for (const auto& run : runs) {
        CHECK(run->confirmedTick() <= run->simulatedTick());
        CHECK(run->simulatedTick() - run->confirmedTick() <= 8);
        CHECK_FALSE(run->hasCompletedProgress());
    }
    // The host's canonical second seat is not its physical device 1.
    CHECK_FALSE(runs[0]->pause(1));
    CHECK_FALSE(runs[1]->pause(0));
    REQUIRE(runs[0]->pause(0));
    std::array<u64, 3> confirmed{};
    for (usize peer = 0; peer < runs.size(); ++peer) {
        confirmed[peer] = runs[peer]->confirmedTick();
    }
    pump([&] {
        for (usize peer = 0; peer < runs.size(); ++peer) {
            if (runs[peer]->confirmedTick() < confirmed[peer] + 90) {
                return false;
            }
        }
        return true;
    });
    CHECK(runs[0]->status() == "Local menu open - the game continues for everyone.");
    CHECK(runs[1]->status().empty());
    CHECK(runs[2]->status().empty());
    runs[0]->resume();
    step();
    CHECK(runs[0]->status().empty());
    // A real provider departure after start drops that guest: both surviving
    // machines say so, keep simulating and never finish or save on its account.
    runs[1]->close();
    active[1] = false;
    const std::string notice = "A player dropped out of the online game. The game goes on.";
    pump([&] { return runs[0]->status() == notice && runs[2]->status() == notice; });
    for (const usize peer : {usize{0}, usize{2}}) {
        confirmed[peer] = runs[peer]->confirmedTick();
    }
    pump([&] {
        return runs[0]->confirmedTick() >= confirmed[0] + 90 &&
               runs[2]->confirmedTick() >= confirmed[2] + 90 && runs[0]->status().empty() &&
               runs[2]->status().empty();
    });
    CHECK_FALSE(runs[1]->playing());
    for (const usize peer : {usize{0}, usize{2}}) {
        CAPTURE(peer);
        CHECK(runs[peer]->playing());
        CHECK(runs[peer]->lobby().page != NetplayMenu::Page::Finished);
        CHECK_FALSE(runs[peer]->hasCompletedProgress());
        CHECK_FALSE(runs[peer]->saveCompletedProgress());
    }
    for (usize peer = 0; peer < runs.size(); ++peer) {
        CAPTURE(peer);
        CHECK(stores[peer].autoSaves == 0);
        CHECK(stores[peer].saves == 0);
        CHECK(unlocks[peer].masks.empty());
    }
}

TEST_CASE("development iroh launcher retains explicit native setup and automatic start",
          "[netplay][online-native-launch][assets][serial]") {
    REQUIRE(NativeOnlineIdentity::prepareProcessNumeric());
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto directory = test::scratchDirectory("online-native-launch");
    const auto invitation = directory / "invitation.txt";
    const GameConfig config;
    StringTable strings;
    LevelCatalog levels;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    REQUIRE(levels.load(root));
    const auto initial = levels.byName("G1");
    REQUIRE(initial);
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &levels;
    context.unpackedRoot = root;
    PartyMember first;
    first.player = 0;
    first.save.name = "HOST";
    first.save.progress().health = 1000;
    auto second = first;
    second.player = 2;
    second.save.name = "GUEST";
    second.save.color = 1;
    PlayOptions options;
    options.welcome = false;
    options.yaw = 0.25f;
    auto hostDevice = std::make_unique<test::FakeRenderDevice>();
    auto guestDevice = std::make_unique<test::FakeRenderDevice>();
    hostDevice->recordDraws = guestDevice->recordDraws = false;
    OnlineRun host;
    OnlineRun guest;
    REQUIRE(host.open(*hostDevice, context, OnlineRun::Links{}, "local", {}, "test",
                      std::string(64, 'a'), std::array{first}, *initial, options, true,
                      invitation));
    bool joined = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (std::chrono::steady_clock::now() < deadline) {
        host.update({}, {});
        if (!joined && std::filesystem::exists(invitation)) {
            REQUIRE(std::filesystem::file_size(invitation) > 0);
            REQUIRE(guest.open(*guestDevice, context, OnlineRun::Links{}, "local",
                               invitation.string(), "test", std::string(64, 'a'),
                               std::array{second}, *initial, options, true));
            joined = true;
        }
        guest.update({}, {});
        REQUIRE(host.lobby().page != NetplayMenu::Page::Finished);
        REQUIRE(guest.lobby().page != NetplayMenu::Page::Finished);
        host.render(*hostDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
        guest.render(*guestDevice, Mat4{1}, 640, 448, 1.0 / 60, 1);
        if (joined && host.playing() && guest.playing() && host.status().empty() &&
            guest.status().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(joined);
    CHECK(host.playing());
    CHECK(guest.playing());
    CHECK(host.status().empty());
    CHECK(guest.status().empty());
    host.close();
    guest.close();
}
} // namespace
