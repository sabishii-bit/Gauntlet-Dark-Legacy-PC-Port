#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/online/NativeRollbackDriver.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Status = NativeRollbackDriver::Status;
using Admission = NativeRollbackDriver::Admission;

struct NativePeer {
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene scene;
    MatchContext context;
    u8 id;
    NativeRollbackDriver driver;
    NativeRollbackDriver::Result last;

    NativePeer(u8 peer, u8 delay, bool injured = false) : id(peer) {
        device.recordDraws = false;
        config.timing.tickRate = 60;
        const auto root = test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2")
                              .parent_path()
                              .parent_path()
                              .parent_path();
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName("G1");
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        GameContext game;
        game.config = &config;
        game.unpackedRoot = root;
        game.levels = &catalog;
        std::array<PartyMember, 2> members;
        members[0].player = 0;
        members[1].player = 2;
        for (auto& member : members) {
            member.save.name = "TEST";
            member.save.character = member.player;
            member.save.color = member.player;
            member.save.progress().health = 2000;
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, game, world, members, options));
        if (injured) {
            // Prepare the same real death state before recording/prediction.
            // Damage injected outside a recorded tick cannot author audio cues.
            scene.hurtPlayer(0, 100000.0f, HurtKind::Blow);
            REQUIRE(scene.participants()[0].life == PlayerLife::Dying);
        }
        context.epoch = 1;
        context.scene = 1;
        context.owners = {1, 0, 2, 0};
        context.grants = {1, 0, 2, 0};
        context.inputLead = delay;
        REQUIRE(scene.beginRecordedAudio(context.epoch));
        REQUIRE(driver.bind(scene, context, id, NativeRollbackDriver::AudioMode::Required));
    }
    NativePeer(const NativePeer&) = delete;
    NativePeer& operator=(const NativePeer&) = delete;
    ~NativePeer() {
        driver.clear();
        scene.endRecordedAudio();
    }
    u64 tick() const { return driver.nextTick() - 1; }
    Status advance() {
        last = driver.advance();
        REQUIRE(last.status != Status::Failed);
        return last.status;
    }
};

struct NativePair {
    std::unique_ptr<NativePeer> host, guest;
    explicit NativePair(u8 delay, bool injured = false)
        : host(std::make_unique<NativePeer>(1, delay, injured)),
          guest(std::make_unique<NativePeer>(2, delay, injured)) {}
    void pump() {
        for (const bool reverse : {false, true}) {
            auto& from = reverse ? *guest : *host;
            auto& to = reverse ? *host : *guest;
            if (!from.driver.redundantInputs().empty()) {
                const auto admitted = to.driver.submit(from.id, from.driver.redundantInputs());
                REQUIRE((admitted == Admission::Accepted || admitted == Admission::Duplicate ||
                         admitted == Admission::Stale));
            }
            const auto progress = to.driver.observePeerTick(from.id, 1, from.driver.nextTick());
            REQUIRE((progress == Admission::Accepted || progress == Admission::Duplicate ||
                     progress == Admission::Stale));
        }
    }
    std::array<Status, 2> advance(const SessionInputs::Frame& first = {},
                                  const SessionInputs::Frame& second = {}) {
        REQUIRE(host->driver.sample(first));
        REQUIRE(guest->driver.sample(second));
        pump();
        return {host->advance(), guest->advance()};
    }
    void step(const SessionInputs::Frame& first = {}, const SessionInputs::Frame& second = {}) {
        const auto result = advance(first, second);
        REQUIRE(result[0] == Status::Advanced);
        REQUIRE(result[1] == Status::Advanced);
    }
    void warm() {
        for (u32 tick = 0; tick < 420; ++tick) {
            step();
        }
    }
};
} // namespace

TEST_CASE("native rollback lifecycle runs sparse local seats through real scenes",
          "[native-rollback-lifecycle][assets]") {
    const auto delay = GENERATE(u8{0}, u8{1}, u8{2});
    NativePair pair(delay);
    u32 moved = 0;
    for (u32 tick = 1; tick <= 420; ++tick) {
        CAPTURE(tick, delay);
        SessionInputs::Frame first;
        SessionInputs::Frame second;
        if (tick > 360) {
            first[0].move = {{1, 0}, 0.5f};
            second[0].move = {{-1, 0}, 0.5f};
            first[0].attack = tick % 60 < 20;
            second[0].attack = tick % 60 > 40;
        }
        const auto before = pair.host->scene.actor(0)->position();
        pair.step(first, second);
        moved += static_cast<u32>(before != pair.host->scene.actor(0)->position());
        CHECK(pair.host->tick() == tick);
        CHECK(pair.guest->tick() == tick);
        CHECK(pair.host->driver.confirmedTick() == tick);
        CHECK(pair.guest->driver.confirmedTick() == tick);
        for (s32 const seat : {0, 2}) {
            CHECK(pair.host->scene.actor(seat)->position() ==
                  pair.guest->scene.actor(seat)->position());
            CHECK(pair.host->scene.actor(seat)->yaw() == pair.guest->scene.actor(seat)->yaw());
        }
        CHECK(pair.host->last.digests == pair.guest->last.digests);
    }
    REQUIRE(moved > 0);
}

TEST_CASE("native cleared rollback driver cannot advance a disconnected scene",
          "[native-rollback-lifecycle][assets]") {
    NativePair pair(0);
    pair.step();
    const auto hash = pair.host->scene.nativeAgreementDigest();
    REQUIRE(hash);
    pair.host->driver.clear(); // The session owner detaches and clears on disconnect.
    CHECK_FALSE(pair.host->driver.sample({}));
    CHECK(pair.host->driver.advance().status == Status::Failed);
    CHECK(pair.host->scene.nativeAgreementDigest() == hash);
}

TEST_CASE("an online match's fixed party offers a fallen player no Quit to answer",
          "[native-rollback-lifecycle][native-quit][assets]") {
    NativePair pair(u8{0}, true);
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        peer->scene.setFixedRoster(true);
    }
    pair.warm();
    for (u32 tick = 0; tick < 900 && !pair.host->scene.participants()[0].towerPrompt; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host->scene.participants()[0].towerPrompt);
    for (u32 tick = 0; tick < 3; ++tick) {
        SessionInputs::Frame input;
        input[0].menu.back = tick == 0;
        const auto result = pair.advance(input);
        REQUIRE(result[0] == result[1]);
        CHECK(result[0] == Status::Advanced);
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.participants()[0].towerPrompt); // still waiting to be answered
        CHECK_FALSE(peer->scene.participants()[0].departed);
    }
}

TEST_CASE("native predicted dead-player Quit is confirmed while Wait keeps the match running",
          "[native-rollback-lifecycle][native-quit][assets]") {
    const auto delay = GENERATE(u8{0}, u8{1}, u8{2});
    const bool quit = GENERATE(false, true);
    NativePair pair(delay, true);
    pair.warm();
    for (u32 tick = 0; tick < 900 && !pair.host->scene.participants()[0].towerPrompt; ++tick) {
        pair.step();
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        REQUIRE(peer->scene.participants()[0].towerPrompt);
        REQUIRE(peer->scene.participants()[1].life == PlayerLife::Standing);
    }
    bool answered = false;
    for (u32 tick = 0; tick <= static_cast<u32>(delay) + 1; ++tick) {
        SessionInputs::Frame input;
        input[0].menu.back = quit && tick == 0;
        input[0].menu.select = !quit && tick == 0;
        const auto result = pair.advance(input);
        REQUIRE(result[0] == result[1]);
        if (!pair.host->scene.participants()[0].towerPrompt) {
            CHECK(result[0] == (quit ? Status::Leave : Status::Advanced));
            answered = true;
            break;
        }
    }
    REQUIRE(answered);
    if (!quit) {
        for (u32 tick = 0; tick < 30; ++tick) {
            pair.step();
        }
        CHECK_FALSE(pair.host->scene.participants()[0].departed);
        return;
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.participants()[0].departed);
        CHECK_FALSE(peer->scene.participants()[1].departed);
        CHECK(peer->driver.confirmedTick() == peer->tick());
        const auto before = peer->tick();
        const auto livingTime = peer->scene.actor(2)->save().progress().lifetime.playSeconds;
        for (u32 poll = 0; poll < 30; ++poll) {
            REQUIRE(peer->driver.sample({}));
            CHECK(peer->advance() == Status::Held);
        }
        CHECK(peer->tick() == before);
        CHECK(peer->scene.actor(2)->save().progress().lifetime.playSeconds == livingTime);
    }
}

TEST_CASE("native simultaneous Quit confirms the fixed roster without malformed seats",
          "[native-rollback-lifecycle][native-quit][assets]") {
    NativePair pair(0);
    pair.warm();
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        for (const auto& participant : peer->scene.participants()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto& runtime = const_cast<PlayerRuntime&>(participant);
            runtime.life = PlayerLife::InTower;
            runtime.towerPrompt = true;
            runtime.actor.save().progress().health = 1;
        }
    }
    SessionInputs::Frame quit;
    quit[0].menu.back = true;
    const auto result = pair.advance(quit, quit);
    CHECK(result[0] == Status::Leave);
    CHECK(result[1] == Status::Leave);
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.gameOver().active());
        CHECK(peer->scene.party().empty());
        CHECK(peer->driver.confirmedTick() == peer->tick());
        CHECK(peer->advance() == Status::Held);
    }
}

TEST_CASE("native GameOver confirms its terminal frame once and then holds",
          "[native-rollback-lifecycle][native-quit][assets]") {
    NativePair pair(0);
    pair.host->scene.startGameOver();
    pair.guest->scene.startGameOver();
    for (u32 tick = 1; tick <= static_cast<u32>(GameOver::kDurationTicks); ++tick) {
        const auto result = pair.advance();
        const auto expected = tick == static_cast<u32>(GameOver::kDurationTicks) ? Status::GameOver
                                                                                 : Status::Advanced;
        CHECK(result[0] == expected);
        CHECK(result[1] == expected);
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.gameOver().finished());
        CHECK(peer->driver.confirmedTick() == peer->tick());
        CHECK(peer->advance() == Status::Held);
        CHECK(peer->tick() == GameOver::kDurationTicks);
    }
}
