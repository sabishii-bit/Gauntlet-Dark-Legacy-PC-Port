#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/ClientClock.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaView.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Result = PlayReplication::Result;

struct Wire final : PacketTransport {
    std::vector<std::vector<u8>> packets;
    SendResult send(Connection /*connection*/, std::span<const u8> bytes,
                    Delivery /*delivery*/) override {
        packets.emplace_back(bytes.begin(), bytes.end());
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return std::nullopt;
    }
    void close(Connection /*connection*/) override {}
};
struct Pair {
    MatchSession host;
    MatchSession guest;
    Wire wire;
    Pair() {
        constexpr MatchOwners kOwners{1, 2, 0, 0};
        const std::array clients{MatchLink{2, 1}};
        const std::array server{MatchLink{1, 99}};
        REQUIRE(host.open(1, kOwners, clients));
        REQUIRE(guest.open(2, kOwners, server));
    }
    void send(MatchSession& from, MatchSession& to, PacketTransport::Connection sender,
              bool duplicate = false) {
        from.flush(wire);
        for (const auto& bytes : wire.packets) {
            const auto result = to.receive(sender, bytes);
            REQUIRE((result == MatchSession::Admission::Accepted ||
                     result == MatchSession::Admission::Stale));
            if (duplicate) {
                const auto again = to.receive(sender, bytes);
                REQUIRE((again == MatchSession::Admission::Accepted ||
                         again == MatchSession::Admission::Stale));
            }
        }
        wire.packets.clear();
    }
    void pump() {
        send(host, guest, 99);
        send(guest, host, 1);
        send(host, guest, 99);
    }
    void prepare(MatchTransition transition = MatchTransition::Start) {
        REQUIRE(host.prepare(1, transition));
        pump();
    }
    void ready() {
        REQUIRE(host.loaded());
        REQUIRE(guest.loaded());
        pump();
        REQUIRE(host.phase() == MatchSession::Phase::Running);
        REQUIRE(guest.phase() == MatchSession::Phase::Running);
    }
};
struct Scene {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    PickupResources pickups;
    FixtureResources fixtures;
    explicit Scene(std::string_view levelName = {}, bool companions = false, bool timers = false,
                   bool welcome = false, std::span<const PartyMember> selected = {}) {
        if (!levelName.empty()) {
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName(levelName);
            REQUIRE(level);
            REQUIRE(world.load(device, root, *level));
        }
        GameContext context;
        context.config = &config;
        context.levels = levelName.empty() ? nullptr : &catalog;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = welcome;
        options.position = Vec3{3, 2, -20};
        if (timers) {
            REQUIRE(world.startPoint(0));
            options.position = world.startPoint(0)->position;
        }
        CharacterSave save;
        save.progress().health = 1000;
        if (timers) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
            save.progress().relics.addShard(9);
        }
        if (companions) {
            save.progress().experience = levelExperience(80);
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        }
        const std::array party{PartyMember{0, save}, PartyMember{1, save}};
        REQUIRE(play.open(device, context, world, selected.empty() ? std::span(party) : selected,
                          options));
        REQUIRE(pickups.bind(device, world.placedItems().archives()));
        auto source = world.placedItems().archives();
        std::vector<ItemArchive*> archives(source.begin(), source.end());
        if (world.realmItems().loaded()) {
            archives.push_back(&world.realmItems());
        }
        REQUIRE(
            fixtures.bind(device, archives, play.generators(), play.safeRocks(), &play.statues()));
    }
    void settle() {
        for (s32 i = 0; i < 400; ++i) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(play.spawning());
        REQUIRE_FALSE(play.animator(0)->entering());
        REQUIRE_FALSE(play.animator(1)->entering());
    }
};

TEST_CASE("remote scroll dismissal advances the host once and respects the native page hold",
          "[netplay][play-replication][assets]") {
    Scene scene({}, false, false, true);
    for (s32 i = 0; i < 600 && !scene.play.scrollLook(); ++i) {
        REQUIRE(scene.play.update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE(scene.play.scrollLook());
    CHECK(scene.play.scrollLook()->page == 0);
    const Vec3 start = scene.play.actor(1)->position();
    Pair pair;
    PlayReplication driver;
    const ProjectileResources resources;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    const auto step = [&](const PlayInput& input) {
        SessionInputs::Frame inputs;
        inputs[0] = input;
        REQUIRE(MatchInputs::sample(pair.guest, inputs));
        pair.send(pair.guest, pair.host, 1, true);
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        REQUIRE(driver.latest()->hud);
        const auto local = scene.play.scrollLook();
        const auto& captured = driver.latest()->hud->scroll;
        REQUIRE(captured.has_value() == local.has_value());
        if (local) {
            CHECK(captured->message == local->message);
            CHECK(captured->page == local->page);
            CHECK(captured->burnFrame == local->burnFrame);
            CHECK(captured->promptAlpha == local->promptAlpha);
            CHECK(scene.play.actor(1)->position() == start);
        }
        pair.pump();
    };
    PlayInput early;
    early.menu.buttonPressed = true;
    step(early);
    PlayInput held;
    held.attack = true;
    held.move = {{1, 0}, 1};
    for (s32 tick = 0; tick < 40; ++tick) {
        step(held);
    }
    REQUIRE(scene.play.scrollLook());
    CHECK(scene.play.scrollLook()->page == 0); // The early edge is not saved for later.
    const auto pages = scene.play.scroll().pageCount();
    for (usize page = 1; page < pages; ++page) {
        PlayInput press;
        press.menu.pointerPressed = true;
        step(press);
        for (s32 tick = 0; tick < 40; ++tick) {
            step(held);
        }
        REQUIRE(scene.play.scrollLook());
        CHECK(scene.play.scrollLook()->page == page);
        CHECK(scene.play.scrollLook()->burnFrame == -1);
        REQUIRE(pair.guest.playback().latest());
        CHECK(pair.guest.playback().latest()->hud->scroll->page == page);
    }
    PlayInput dismiss;
    dismiss.menu.buttonPressed = true;
    step(dismiss);
    bool sawBurn = false;
    for (s32 tick = 0; tick < 120 && scene.play.scrollLook(); ++tick) {
        step({});
        sawBurn = sawBurn || (scene.play.scrollLook() && scene.play.scrollLook()->burnFrame >= 0);
    }
    CHECK(sawBurn);
    CHECK_FALSE(scene.play.scrollLook());
    for (s32 tick = 0; tick < 4; ++tick) {
        step({});
    }
    REQUIRE(pair.guest.playback().latest());
    CHECK_FALSE(pair.guest.playback().latest()->hud->scroll);
}

TEST_CASE("real scene ticks only on the host after the load barrier and holds across pause",
          "[netplay][play-replication][assets]") {
    std::array<CharacterProfile, 2> profiles;
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        CharacterSave save;
        save.name = seat == 0 ? "HOST" : "GUEST";
        save.progress().health = 1000;
        save.progress().experience = levelExperience(80);
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        const auto profile = CharacterProfile::capture(save);
        REQUIRE(profile);
        profiles[seat] = *profile;
    }
    test::OnlinePair pair(profiles);
    REQUIRE(pair.hostSession.party());
    REQUIRE(pair.guestSession.party());
    std::vector<PartyMember> members;
    for (usize seat = 0; seat < pair.hostSession.party()->size(); ++seat) {
        if (const auto& profile = (*pair.hostSession.party())[seat]) {
            members.push_back({static_cast<s32>(seat), profile->gameplayCopy()});
        }
    }
    Scene scene({}, false, false, false, members);
    scene.settle();
    PlayReplication driver;
    ProjectileResources resources;
    REQUIRE(scene.play.bindPlayerProjectiles(resources));
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    CHECK_FALSE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    const Vec3 start = scene.play.actor(1)->position();
    const auto initialFrame = scene.play.animator(1)->player().frame();
    REQUIRE(pair.host.loaded());
    pair.pump();
    for (s32 i = 0; i < 60; ++i) {
        CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
    }
    CHECK(scene.play.actor(1)->position() == start);
    CHECK(scene.play.animator(1)->player().frame() == initialFrame);
    CHECK(pair.host.tick() == 0);
    REQUIRE(pair.guest.loaded());
    pair.pump();

    // Independent client resources: no PlayScene exists on this side.
    LevelWorld clientWorld;
    REQUIRE(clientWorld.load(scene.device, scene.root));
    Enemies clientEnemies;
    clientEnemies.open(scene.device, scene.root, nullptr, 4, {}, 1);
    ProjectileResources clientProjectiles;
    PickupResources clientPickups;
    REQUIRE(clientPickups.bind(scene.device, clientWorld.placedItems().archives()));
    FixtureResources clientFixtures;
    const Generators clientGenerators;
    const SafeRocks clientRocks;
    const auto sources = clientWorld.placedItems().archives();
    std::vector<ItemArchive*> fixtureArchives(sources.begin(), sources.end());
    if (clientWorld.realmItems().loaded()) {
        fixtureArchives.push_back(&clientWorld.realmItems());
    }
    REQUIRE(clientFixtures.bind(scene.device, fixtureArchives, clientGenerators, clientRocks));
    ReplicaView client;
    REQUIRE(client.begin(pair.guest.context(), clientWorld, clientEnemies, clientProjectiles,
                         clientPickups, clientFixtures));
    std::array<PlayerFigure*, InputCommand::kSeats> clientFigures{};
    for (u8 seat = 0; seat < 2; ++seat) {
        const auto save = (*pair.guestSession.party())[seat]->gameplayCopy();
        auto figure = PlayerFigure::load(scene.device, scene.root, save, false);
        clientFigures[seat] = figure.get();
        REQUIRE(client.setPlayer(seat, std::move(figure), PlayerFigure::bodyScale(save, {})));
    }
    ItemArchive companionWeapons;
    REQUIRE(companionWeapons.load(scene.root / "WEAPONS"));
    TextureSet clientStatic;
    REQUIRE(clientStatic.load(scene.root / "STATIC"));
    const std::array<TextureSet*, 5> clientLenders{
        &companionWeapons.textures, &clientWorld.items().textures,
        &clientWorld.realmItems().textures, &clientWorld.powerups().textures, &clientStatic};
    REQUIRE(
        clientProjectiles.addPlayers(scene.device, companionWeapons, clientFigures, clientLenders));
    REQUIRE(client.bindCompanions(scene.device, clientWorld.powerups(), &companionWeapons));
    HudResources hudResources;
    hudResources.powerups = &clientWorld.powerups();
    REQUIRE(client.loadHud(scene.device, scene.root, nullptr, hudResources));
    REQUIRE(scene.world.placeItem(scene.device, "TREAS_GOLD", scene.play.actor(0)->position()));
    const auto goldBefore = scene.play.actor(0)->save().gold;
    SessionInputs::Frame guestInput;
    guestInput[0].move = {{0, 1}, 1}; // guest device zero owns global seat one
    for (s32 i = 0; i < 40; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        CHECK(driver.latest()->motion.tick == pair.host.tick() - 1);
        CHECK(driver.latest()->motion.players[1]->position == scene.play.actor(1)->position());
        CHECK(driver.latest()->motion.camera.position == scene.play.viewCamera().position);
        for (usize seat = 0; seat < 2; ++seat) {
            REQUIRE(driver.latest()->players[seat]->companions[0]);
            REQUIRE(driver.latest()->players[seat]->companions[1]);
            REQUIRE(driver.latest()->hud);
            REQUIRE(driver.latest()->hud->players[seat]);
            CHECK(driver.latest()->hud->players[seat]->health ==
                  scene.play.actor(static_cast<s32>(seat))->save().health());
            CHECK(driver.latest()->hud->visible == scene.play.hudVisible());
        }
        pair.pump();
    }
    CHECK(glm::distance(start, scene.play.actor(1)->position()) > 1);
    CHECK(scene.play.actor(0)->save().gold > goldBefore);
    REQUIRE(driver.latest()->hud);
    REQUIRE_FALSE(driver.latest()->hud->cards.empty());
    CHECK(driver.latest()->hud->cards.back().kind == HudCardKind::Gold);
    REQUIRE(pair.guest.playback().latest());
    CHECK(pair.guest.playback().latest()->motion.tick == 39);
    ClientClock clock;
    REQUIRE(clock.begin(pair.guest));
    const auto firstShown = clock.sample(pair.guest, 0);
    REQUIRE(firstShown);
    REQUIRE(client.show(*firstShown));
    CHECK(client.actors().visiblePlayers() == 2);
    REQUIRE_FALSE(driver.latest()->pickups.empty());
    CHECK(client.pickupCount() == driver.latest()->pickups.size());
    REQUIRE_FALSE(driver.latest()->fixtures.empty());
    CHECK(client.fixtureCount() == driver.latest()->fixtures.size());
    const auto version = client.actors().playerFigure(1)->animationRevision();
    const auto captured = CombatPacket::encode(*driver.latest());
    const Mat4 projection = glm::orthoLH_ZO(0.0f, 1920.0f, 1080.0f, 0.0f, 0.0f, 1.0f);
    for (s32 draw = 0; draw < 3; ++draw) {
        scene.device.draws.clear();
        client.draw(scene.device, projection, 1920, 1080, 0);
        CHECK_FALSE(scene.device.draws.empty());
    }
    CHECK(client.actors().playerFigure(1)->animationRevision() == version);
    CHECK(clientEnemies.count() == 0);
    CHECK(clientEnemies.takeFeedback().empty());
    CHECK(CombatPacket::encode(*driver.latest()) == captured);
    guestInput = {};
    guestInput[0].attack = true;
    usize receivedShots = 0;
    for (s32 i = 0; i < 180; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        REQUIRE(pair.guest.playback().latest());
        for (s32 render = 0; render < 2; ++render) {
            const auto shown = clock.sample(pair.guest, 1.0 / 120);
            REQUIRE(shown);
            CAPTURE(i, render, clock.tick(), clock.fraction(), shown->motion.tick);
            REQUIRE(shown->valid());
            REQUIRE(shown->geometry);
            REQUIRE(client.geometry().acceptsGeometry(*shown->geometry));
            REQUIRE(client.actors().companionsReady(*shown));
            for (const auto& shot : shown->projectiles) {
                CAPTURE(shot.resource, shot.animation.frame, shot.animation.transition);
                REQUIRE(clientProjectiles.accepts(shot));
            }
            for (const auto& item : shown->pickups) {
                REQUIRE(clientPickups.accepts(item));
            }
            for (const auto& fixture : shown->fixtures) {
                REQUIRE(clientFixtures.accepts(fixture));
            }
            REQUIRE(client.show(*shown));
            receivedShots += shown->projectiles.size();
        }
    }
    CHECK(receivedShots > 0);
    const auto beforePause = CombatPacket::encode(*driver.latest());
    REQUIRE(pair.guest.requestPause());
    pair.pump();
    const auto tick = pair.host.tick();
    const auto paused = scene.play.actor(1)->position();
    for (s32 i = 0; i < 120; ++i) {
        CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
    }
    CHECK(pair.host.tick() == tick);
    CHECK(scene.play.actor(1)->position() == paused);
    CHECK(CombatPacket::encode(*driver.latest()) == beforePause);
    const auto oldSnapshot = *driver.latest();
    pair.prepare(MatchTransition::Resume);
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    CHECK(driver.latest() == nullptr);
    const auto* figureBeforeResume = client.actors().playerFigure(1);
    const auto texturesBeforeResume = scene.device.texturesCreated;
    const auto heldImage = CombatPacket::encode(*client.shown());
    REQUIRE(client.resume(pair.guest.context()));
    REQUIRE(clock.begin(pair.guest));
    CHECK_FALSE(client.resume(pair.guest.context()));
    CHECK(client.actors().playerFigure(1) == figureBeforeResume);
    CHECK(CombatPacket::encode(*client.shown()) == heldImage);
    CHECK_FALSE(client.show(oldSnapshot));
    scene.device.draws.clear();
    client.draw(scene.device, projection, 1920, 1080, 0);
    CHECK_FALSE(scene.device.draws.empty());
    pair.ready();
    REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
    CHECK(driver.latest()->motion.tick == 0);
    CHECK(driver.latest()->motion.epoch > oldSnapshot.motion.epoch);
    CHECK(scene.play.actor(1)->position() == paused); // no stale held movement across resume
    pair.pump();
    REQUIRE(pair.guest.playback().latest());
    REQUIRE(client.show(*pair.guest.playback().latest()));
    CHECK(client.actors().visiblePlayers() == 2);
    receivedShots = 0;
    for (s32 i = 0; i < 60; ++i) {
        REQUIRE(MatchInputs::sample(pair.guest, guestInput));
        pair.pump();
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        pair.pump();
        const auto shown = clock.sample(pair.guest, 1.0 / 60);
        REQUIRE(shown);
        REQUIRE(client.show(*shown));
        receivedShots += client.projectileCount();
        scene.device.draws.clear();
        client.draw(scene.device, projection, 1920, 1080, 0);
    }
    CHECK(receivedShots > 0);
    CHECK(scene.device.texturesCreated == texturesBeforeResume);
    pair.host.disconnected(1);
    CHECK(driver.advance(pair.host, {}, resources) == Result::Held);
}

TEST_CASE("scene capture preserves timer priority pause and runestone visibility",
          "[netplay][play-replication][assets]") {
    for (const std::string_view level : {"G1", "S4"}) {
        CAPTURE(level);
        Scene scene(level, false, true);
        ProjectileResources resources;
        REQUIRE(scene.play.bindPlayerProjectiles(resources));
        Pair pair;
        PlayReplication driver;
        pair.prepare();
        REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
        pair.ready();
        std::vector<std::vector<u8>> history;
        for (s32 tick = 0; tick < 8; ++tick) {
            REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
            const auto encoded = HudPacket::encode(*driver.latest()->hud);
            REQUIRE(encoded);
            history.push_back(*encoded);
            pair.pump();
            // Secret stages suppress Stop Time and show their own challenge
            // timer. Capture must follow the scene's selected HUD, not the item.
            CHECK(scene.play.hud().hourglassLook(scene.play.participants()).has_value() ==
                  !scene.world.ref().isSecret());
            const auto look = scene.play.hourglassLook();
            REQUIRE(look);
            REQUIRE(driver.latest()->hud->hourglass);
            CHECK(driver.latest()->hud->hourglass->elapsed == look->elapsed);
            CHECK(driver.latest()->hud->hourglass->fallingFrame == look->fallingFrame);
            const auto* received = pair.guest.playback().latest();
            REQUIRE(received);
            REQUIRE(received->motion.tick < history.size());
            CHECK(HudPacket::encode(*received->hud) ==
                  history[static_cast<usize>(received->motion.tick)]);
            if (scene.play.runeMeter().visible()) {
                CHECK(driver.latest()->hud->runeFill == scene.play.runeMeter().fill());
            } else {
                CHECK_FALSE(driver.latest()->hud->runeFill);
            }
        }
        const auto held = CombatPacket::encode(*driver.latest());
        REQUIRE(pair.guest.requestPause());
        pair.pump();
        for (s32 tick = 0; tick < 120; ++tick) {
            CHECK(driver.advance(pair.host, {}, {}) == Result::Held);
        }
        CHECK(CombatPacket::encode(*driver.latest()) == held);
        pair.prepare(MatchTransition::Resume);
        REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
        pair.ready();
        // Expire the test inventory without waiting a minute or advancing a client clock.
        for (s32 seat = 0; seat < 2; ++seat) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
            auto* actor = const_cast<PlayerActor*>(scene.play.actor(seat));
            actor->save().progress().inventory.advance(100);
        }
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        if (scene.world.ref().isSecret()) {
            REQUIRE(scene.play.challenge().state() == SecretChallenge::State::Collecting);
            REQUIRE(scene.play.spawning());
            REQUIRE(driver.latest()->hud->hourglass);
            CHECK(driver.latest()->hud->hourglass->elapsed == 0);
            CHECK(driver.latest()->hud->hourglass->fallingFrame == -1);
        } else {
            CHECK_FALSE(driver.latest()->hud->hourglass);
            REQUIRE(scene.play.runeMeter().visible());
            REQUIRE(driver.latest()->hud->runeFill);
        }
    }
}

TEST_CASE("scene binding rejects mismatched ownership clocks and non-host simulation",
          "[netplay][play-replication][assets]") {
    Scene scene;
    Pair pair;
    pair.prepare();
    PlayReplication driver;
    PlayScene closed;
    CHECK_FALSE(driver.bind(closed, pair.host, scene.pickups, scene.fixtures));
    CHECK_FALSE(driver.bind(scene.play, pair.guest, scene.pickups, scene.fixtures));
    scene.config.timing.tickRate = 30;
    CHECK_FALSE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    scene.config.timing.tickRate = 60;
    MatchSession solo;
    REQUIRE(solo.open(1, {1, 0, 0, 0}, {}));
    REQUIRE(solo.prepare(1));
    CHECK_FALSE(driver.bind(scene.play, solo, scene.pickups, scene.fixtures));
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    REQUIRE(pair.host.advance()); // another owner must not also advance the session
    CHECK(driver.advance(pair.host, {}, {}) == Result::Failed);
    CHECK(driver.failed());
    CHECK(driver.latest() == nullptr);
    driver.clear();
    CHECK_FALSE(driver.failed());
}

TEST_CASE("an unregistered real weapon fails capture instead of disappearing remotely",
          "[netplay][play-replication][assets]") {
    Scene scene;
    scene.settle();
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    SessionInputs::Frame input;
    input[0].attack = true;
    input[0].attackPressed = true;
    Result result = Result::Advanced;
    for (s32 i = 0; i < 180 && result == Result::Advanced; ++i) {
        result = driver.advance(pair.host, input, {});
        input[0].attackPressed = false;
    }
    REQUIRE(scene.play.missiles().count() > 0);
    REQUIRE(result == Result::Failed);
    const auto tick = pair.host.tick();
    CHECK(driver.advance(pair.host, input, {}) == Result::Failed);
    CHECK(pair.host.tick() == tick);
}

TEST_CASE("remote continuity uses the same teleport cutoff as local figures",
          "[netplay][play-replication]") {
    PlayerRuntime player;
    player.actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    player.previous.continuous = true;
    player.previous.position = {0.1f, 0, 0};
    CHECK(PartyFigures::presentationContinuous(player));
    player.previous.position = {100, 0, 0};
    CHECK_FALSE(PartyFigures::presentationContinuous(player));
    player.previous.position = player.actor.position();
    player.previous.continuous = false;
    CHECK_FALSE(PartyFigures::presentationContinuous(player));
}

TEST_CASE("entrance camera cuts remain marked when snapshots are sent less often than ticks",
          "[netplay][play-replication][assets]") {
    Scene scene;
    ProjectileResources resources;
    REQUIRE(scene.play.bindPlayerProjectiles(resources));
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    u32 previous = 0;
    for (s32 tick = 0; tick < 7; ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        if (tick > 0) {
            CHECK(driver.latest()->motion.cameraContinuity ==
                  previous + (scene.play.cameraContinuous() ? 0 : 1));
        }
        REQUIRE(driver.latest()->hud);
        CHECK(driver.latest()->hud->screen.titleScale == scene.play.arrival().titleScale());
        CHECK(driver.latest()->hud->screen.transitionOpacity == scene.play.transition().opacity());
        CHECK(driver.latest()->hud->screen.cinematicBars == scene.play.cinematicBars());
        previous = driver.latest()->motion.cameraContinuity;
        pair.pump();
        if (tick % 3 == 0) {
            REQUIRE(pair.guest.playback().latest());
            CHECK(pair.guest.playback().latest()->motion.cameraContinuity == previous);
            CHECK(pair.guest.playback().latest()->motion.tick == static_cast<u64>(tick));
        }
    }
    REQUIRE(pair.guest.requestPause());
    pair.pump();
    const auto held = CombatPacket::encode(*driver.latest());
    for (s32 tick = 0; tick < 120; ++tick) {
        CHECK(driver.advance(pair.host, {}, {}) == Result::Held);
        CHECK(CombatPacket::encode(*driver.latest()) == held);
    }
}
TEST_CASE("a loaded cemetery scene publishes its dormant statues through the real host bridge",
          "[netplay][play-replication][replica-statues][assets]") {
    Scene scene("G4");
    ProjectileResources resources;
    REQUIRE(scene.play.bindPlayerProjectiles(resources));
    REQUIRE(scene.play.statues().count() > 0);
    Pair pair;
    PlayReplication driver;
    pair.prepare();
    REQUIRE(driver.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    for (s32 tick = 0; tick < 4; ++tick) {
        REQUIRE(driver.advance(pair.host, {}, resources) == Result::Advanced);
        REQUIRE(driver.latest());
        const auto& state = *driver.latest();
        CHECK(std::ranges::count_if(state.fixtures, [](const auto& fixture) {
                  return fixture.source == FixtureSource::Statue;
              }) == static_cast<std::ptrdiff_t>(scene.play.statues().count()));
        pair.pump();
        if (tick % 3 == 0) {
            REQUIRE(pair.guest.playback().latest());
            CHECK(CombatPacket::encode(*pair.guest.playback().latest()) ==
                  CombatPacket::encode(state));
        }
    }
}
} // namespace
