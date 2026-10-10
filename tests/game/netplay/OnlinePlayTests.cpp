#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OnlinePlay.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct RuntimePair {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    GameConfig config;
    LevelCatalog catalog;
    GameContext context;
    test::OnlinePair wire{{}, false, 2, 2};
    OnlineParty hostParty;
    OnlineParty guestParty;
    OnlinePlay host;
    OnlinePlay guest;

    explicit RuntimePair(s32 classOffset = 0) {
        REQUIRE(catalog.load(root));
        context.config = &config;
        context.levels = &catalog;
        context.unpackedRoot = root;
        const auto choice = [](s32 device, s32 character, s32 color) {
            CharacterSave save;
            save.name = "ONLINE";
            save.character = character;
            save.color = color;
            save.progress().experience = levelExperience(60);
            save.progress().health = 9000;
            save.classUnlock = 0xFFFF;
            save.moviesSeen = {"PRIVATE"};
            save.classes[static_cast<usize>((character + 1) % kClassCount)].experience = 123456;
            return PartyMember{device, save, static_cast<usize>(10 + device)};
        };
        REQUIRE(hostParty.select(wire.hostSession, std::array{choice(1, classOffset, 0),
                                                              choice(3, classOffset + 1, 1)}));
        REQUIRE(guestParty.select(wire.guestSession, std::array{choice(0, classOffset + 2, 2),
                                                                choice(2, classOffset + 3, 3)}));
        wire.start();
    }
    void open(const LevelRef& level = LevelRef::tower(), PlayOptions options = {}) {
        options.welcome = false;
        if (level.isTower() && !options.position) {
            options.position = Vec3{0, 0, 0}; // bypass the tower entry pan for immediate input
        }
        REQUIRE(guest.open(guestDevice, context, wire.guestSession, guestParty));
        REQUIRE(host.open(hostDevice, context, wire.hostSession, hostParty, level, options));
        // The host cannot simulate before the guest has loaded its own resources.
        CHECK(host.update({}) == OnlinePlay::Phase::Loading);
        CHECK(wire.host.tick() == 0);
        CHECK_FALSE(host.shown());
        for (s32 i = 0; i < 3600 && host.phase() != OnlinePlay::Phase::Playing; ++i) {
            step();
        }
        REQUIRE(host.phase() == OnlinePlay::Phase::Playing);
        REQUIRE(guest.phase() == OnlinePlay::Phase::Playing);
    }
    void step(const SessionInputs::Frame& hostInputs = {},
              const SessionInputs::Frame& guestInputs = {}) {
        REQUIRE(host.update(hostInputs) != OnlinePlay::Phase::Failed);
        REQUIRE(guest.update(guestInputs) != OnlinePlay::Phase::Failed);
        if (host.entryVisible()) {
            hostDevice.draws.clear();
            host.render(hostDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        }
        if (guest.entryVisible()) {
            draw();
        }
    }
    void draw() {
        guestDevice.draws.clear();
        guest.render(guestDevice, Mat4{1}, 1280, 720, 1.0 / 60, 1);
        REQUIRE(guest.phase() != OnlinePlay::Phase::Failed);
    }
};

TEST_CASE("an online tower portal loads the same stage and retained party on both machines",
          "[netplay][online-play][online-travel][assets]") {
    RuntimePair pair;
    PlayOptions options;
    options.position = Vec3{45.1f, -6.5f, -112.7f};
    pair.open(LevelRef::tower(), options);
    const auto before = pair.host.hostScene()->party();
    const auto initialEpoch = pair.wire.host.context().epoch;
    for (s32 tick = 0; tick < 3600; ++tick) {
        pair.step();
        pair.draw();
        if (pair.wire.host.context().epoch > initialEpoch &&
            pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair.wire.host.context().epoch > initialEpoch);
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.host.hostScene()->world()->ref().name == "G1");
    CHECK(OnlinePlay::levelOf(pair.catalog, pair.wire.guest.context().scene)->name == "G1");
    const auto after = pair.host.hostScene()->party();
    REQUIRE(after.size() == before.size());
    for (usize i = 0; i < before.size(); ++i) {
        CHECK(after[i].player == before[i].player);
        CHECK(after[i].save.name == before[i].save.name);
        CHECK(after[i].save.gold == before[i].save.gold);
        CHECK(after[i].save.character == before[i].save.character);
    }
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.epoch == pair.wire.host.context().epoch);
    // Loading a stage and drawing its walls is not enough: its exit artwork
    // must survive travel into the independently loaded guest resource catalog.
    pair.hostDevice.draws.clear();
    pair.host.hostScene()->portals().draw(pair.hostDevice, Mat4{1}, {});
    REQUIRE_FALSE(pair.hostDevice.draws.empty());
    for (const auto& expected : pair.hostDevice.draws) {
        const auto& texture = dynamic_cast<const test::FakeTexture&>(*expected.texture);
        CHECK(std::ranges::any_of(pair.guestDevice.draws, [&](const auto& actual) {
            return actual.vertices.size() == expected.vertices.size() &&
                   actual.blend() == expected.blend() &&
                   dynamic_cast<const test::FakeTexture&>(*actual.texture).pixels == texture.pixels;
        }));
    }
}

TEST_CASE("G1 plays through arrival and accepts remote movement and attacks",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    const auto level = pair.catalog.byName("G1");
    REQUIRE(level);
    pair.open(*level);
    for (s32 tick = 0; tick < 600; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.host.shown());
    const auto before = pair.host.shown()->motion.players[2]->position;
    SessionInputs::Frame guest;
    guest[0].move = {{1, 0}, 1}; // guest device 0 owns room seat 2, not host seat 0
    for (s32 tick = 0; tick < 45; ++tick) {
        pair.step({}, guest);
        pair.draw();
    }
    CHECK(glm::distance(pair.host.shown()->motion.players[2]->position, before) > 0.05f);
    guest[0].move = {};
    guest[0].attack = true;
    usize projectiles = 0;
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step({}, guest);
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            projectiles = std::max(projectiles, shown->projectiles.size());
        }
    }
    CHECK(projectiles > 0);
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}

TEST_CASE("online entry shows the native map before either peer can simulate",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    const auto level = pair.catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *level));
    for (s32 tick = 0; tick < 120; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Loading);
    CHECK(pair.wire.host.tick() == 0);
    CHECK_FALSE(pair.host.hostScene());
    REQUIRE_FALSE(pair.guestDevice.draws.empty());
    CHECK(pair.guestDevice.draws.front().vertices.front().color == Color::black());
}

TEST_CASE("the guest renders every native golden arrival circle",
          "[netplay][online-arrival][assets]") {
    RuntimePair pair;
    pair.open();
    for (s32 tick = 0; tick < 12; ++tick) {
        pair.step();
        pair.draw();
    }
    // ClientClock intentionally presents several ticks behind the host. Exact
    // geometry/flipbook parity at the same tick is checked in ReplicaProjectilesTests.
    REQUIRE(pair.guest.shown());
    CHECK(std::ranges::count_if(pair.guest.shown()->projectiles, [](const auto& effect) {
              return effect.source == ProjectileSource::Arrival;
          }) == 4);
    for (const auto& effect : pair.guest.shown()->projectiles) {
        if (effect.source == ProjectileSource::Arrival) {
            REQUIRE(effect.instance >= 1);
            REQUIRE(effect.instance <= 4);
            CHECK(Vec3{effect.placement[3]} ==
                  pair.host.hostScene()->actor(static_cast<s32>(effect.instance - 1))->position());
        }
    }
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
        pair.draw();
    }
    const auto frozen = pair.guest.shown()->projectiles;
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown()->projectiles.size() == frozen.size());
    for (usize i = 0; i < frozen.size(); ++i) {
        CHECK(pair.guest.shown()->projectiles[i].animation.frame == frozen[i].animation.frame);
    }
    REQUIRE(pair.host.resume());
    for (s32 tick = 0; tick < 150; ++tick) {
        pair.step();
        pair.draw();
    }
    CHECK(std::ranges::none_of(pair.guest.shown()->projectiles, [](const auto& effect) {
        return effect.source == ProjectileSource::Arrival;
    }));
}

TEST_CASE("entry movies keep playing until all four local and remote players vote to skip",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("G1")));
    for (s32 tick = 0; tick < 900 && pair.guest.entryPhase() != OnlineLevelEntry::Phase::Movie;
         ++tick) {
        pair.step();
        CHECK(pair.wire.host.tick() == 0);
    }
    REQUIRE(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    REQUIRE(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    const auto uploads = pair.guestDevice.textureUpdates;
    // An unassigned device does not get to skip either machine's movie.
    SessionInputs::Frame keys;
    keys[0].menu.start = true;
    keys[0].movieSkipPressed = true;
    for (s32 tick = 0; tick < 15; ++tick) {
        pair.step(keys);
    }
    CHECK(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    CHECK(pair.guestDevice.textureUpdates > uploads);
    keys[1].menu.pointerPressed = true;
    keys[1].movieSkipPressed = true;
    pair.step(keys);
    REQUIRE(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
    CHECK(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    for (s32 tick = 0; tick < 60; ++tick) {
        pair.step();
        CHECK(pair.wire.host.tick() == 0);
        CHECK(pair.host.phase() == OnlinePlay::Phase::Loading);
    }
    SessionInputs::Frame guestKeys;
    guestKeys[0].menu.select = true;
    guestKeys[0].movieSkipPressed = true;
    pair.step({}, guestKeys);
    keys = {};
    keys[3].menu.start = true;
    keys[3].movieSkipPressed = true;
    pair.step(keys);
    for (s32 tick = 0; tick < 15; ++tick) {
        pair.step();
        CHECK(pair.host.entryPhase() == OnlineLevelEntry::Phase::Movie);
        CHECK(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
        CHECK(pair.wire.host.tick() == 0);
    }
    CHECK(pair.wire.host.movieSkipVotes() == 7);
    // The last guest controller owns room seat 3, not physical device 3.
    guestKeys = {};
    guestKeys[2].menu.start = true;
    guestKeys[2].movieSkipPressed = true;
    pair.step({}, guestKeys);
    for (s32 tick = 0; tick < 15 && pair.host.phase() != OnlinePlay::Phase::Playing; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    bool circle = false;
    REQUIRE(pair.host.hostScene()->arrival().camera().active());
    const auto openingCamera = pair.host.hostScene()->viewCamera();
    bool cameraMoved = false;
    bool title = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        pair.step();
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            circle = circle || std::ranges::any_of(shown->projectiles, [](const auto& effect) {
                         return effect.source == ProjectileSource::Arrival;
                     });
            title = title || (shown->hud && shown->hud->screen.titleScale > 0);
            if (tick < 90) {
                CHECK(shown->motion.camera.position == openingCamera.position);
            }
            cameraMoved = cameraMoved || shown->motion.camera.position != openingCamera.position;
        }
    }
    CHECK(circle);
    CHECK(title);
    CHECK(cameraMoved);
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.resume());
    for (s32 tick = 0; tick < 60; ++tick) {
        pair.step();
        pair.draw();
        CHECK_FALSE(pair.host.entryVisible());
        CHECK_FALSE(pair.guest.entryVisible());
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}

TEST_CASE("an online boss entry camera needs every player's vote to skip its hold",
          "[netplay][online-entry][movie-skip][assets]") {
    RuntimePair pair;
    pair.open(*pair.catalog.byName("G5"));
    const auto& camera = pair.host.hostScene()->arrival().camera();
    REQUIRE(camera.mode() == StartCamera::Mode::Legacy);
    for (s32 tick = 0; tick < 180 && camera.ticksLeft() > 35; ++tick) {
        pair.step();
    }
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    REQUIRE(camera.ticksLeft() > 20);
    SessionInputs::Frame hostKeys;
    SessionInputs::Frame guestKeys;
    hostKeys[1].menu.select = true;
    hostKeys[3].menu.select = true;
    guestKeys[0].menu.select = true;
    pair.step(hostKeys, guestKeys);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
        CHECK(camera.phase() == StartCamera::Phase::Hold);
    }
    guestKeys = {};
    guestKeys[2].menu.select = true;
    pair.step({}, guestKeys);
    for (s32 tick = 0; tick < 10 && camera.phase() == StartCamera::Phase::Hold; ++tick) {
        pair.step();
    }
    CHECK(camera.phase() == StartCamera::Phase::Ride);
    CHECK(camera.ticksLeft() == 0);
}

TEST_CASE("disconnect during an entry movie releases the local presentation",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty,
                           *pair.catalog.byName("G1")));
    for (s32 tick = 0; tick < 900 && pair.guest.entryPhase() != OnlineLevelEntry::Phase::Movie;
         ++tick) {
        pair.step();
    }
    REQUIRE(pair.guest.entryPhase() == OnlineLevelEntry::Phase::Movie);
    pair.wire.guestSession.leave();
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.host.update({});
        pair.guest.update({});
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Failed);
    CHECK_FALSE(pair.host.entryVisible());
    CHECK_FALSE(pair.guest.entryVisible());
    CHECK(pair.wire.host.tick() == 0);
}

TEST_CASE("native online encounter movies fit inside the bounded loading barrier",
          "[netplay][online-entry][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const AssetLocator assets(root);
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize movies = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level);
            if (level->isTower()) {
                continue;
            }
            WorldData data;
            REQUIRE(data.load(root / level->worldDataFile()));
            const auto* info = data.level(name);
            REQUIRE(info);
            if (info->movie.empty()) {
                continue;
            }
            CAPTURE(name, info->movie);
            const auto file = assets.find("VQMOVIES/" + info->movie + ".avi");
            REQUIRE(file);
            MoviePlayback movie;
            REQUIRE(movie.open(*file));
            const auto& video = movie.info();
            REQUIRE(video.framesPerSecond > 0);
            const auto seconds = static_cast<f64>(video.frameCount) / video.framesPerSecond;
            // Longest possible route, the movie, and 30 seconds for resource loading.
            CHECK(seconds + 10 + 30 < MatchSession::kLoadTimeout);
            ++movies;
        }
    }
    CHECK(movies > 0);
}

TEST_CASE("missing native encounter media fails closed instead of entering unseen gameplay",
          "[netplay][online-entry][assets]") {
    RuntimePair pair;
    const AssetLocator missing(pair.root / "missing-movies");
    auto context = pair.context;
    context.assets = &missing;
    OnlineLevelEntry entry;
    const auto members = pair.hostParty.members(pair.wire.hostSession);
    REQUIRE(members);
    entry.open(pair.hostDevice, context, *pair.catalog.byName("G1"), *members);
    for (s32 tick = 0; tick < 900 && entry.phase() != OnlineLevelEntry::Phase::Failed; ++tick) {
        pair.hostDevice.draws.clear();
        entry.render(pair.hostDevice, Mat4{1}, 1280, 720);
        entry.update(pair.hostDevice, context, true);
    }
    CHECK(entry.phase() == OnlineLevelEntry::Phase::Failed);
    entry.close();
    CHECK_FALSE(entry.visible());
}

TEST_CASE("online linked boss entrances carry stage rewards and private local save state",
          "[netplay][online-play][online-travel][assets]") {
    const std::string stage = GENERATE(std::string{"E1"}, std::string{"F1"});
    const std::string arena = stage == "E1" ? "E2" : "F2";
    RuntimePair pair;
    const auto level = pair.catalog.byName(stage);
    REQUIRE(level);
    PlayOptions options;
    options.position = stage == "E1" ? Vec3{0.25f, 5.12f, -175.57f} : Vec3{-21.75f, 5.04f, -421.0f};
    pair.open(*level, options);
    const auto original = pair.host.hostScene()->party();
    for (s32 seat = 0; seat < 4; ++seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable scene test fixture
        auto& save = const_cast<PlayerActor*>(pair.host.hostScene()->actor(seat))->save();
        save.gold += 250 + seat;
        save.progress().experience += 500;
        save.progress().inventory.keys = 8;
        save.progress().inventory.potions = {1, 3};
        save.progress().relics.runes = 3;
    }
    const auto initialEpoch = pair.wire.host.context().epoch;
    for (s32 tick = 0; tick < 3600; ++tick) {
        pair.step();
        pair.draw();
        if (pair.wire.host.context().epoch > initialEpoch &&
            pair.host.phase() == OnlinePlay::Phase::Playing &&
            pair.guest.phase() == OnlinePlay::Phase::Playing) {
            break;
        }
    }
    REQUIRE(pair.wire.host.context().epoch > initialEpoch);
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.host.hostScene()->world()->ref().name == arena);
    REQUIRE(pair.wire.guest.travelParty());
    const auto carried = pair.host.hostScene()->party();
    REQUIRE(carried.size() == 4);
    const auto results = pair.host.hostScene()->levelResults();
    REQUIRE(results.size() == 4);
    for (usize seat = 0; seat < carried.size(); ++seat) {
        const auto& member = carried[seat];
        const auto& profile = (*pair.wire.guest.travelParty())[seat];
        REQUIRE(profile);
        CHECK(member.save.gold == 250 + static_cast<s32>(seat));
        CHECK(profile->gold == member.save.gold);
        CHECK(profile->progress.inventory.keys == 8);
        CHECK(profile->progress.inventory.potions == std::vector<s32>{1, 3});
        CHECK(profile->progress.relics.runes == 3);
        CHECK(member.save.progress().levels.beaten[static_cast<usize>(level->realmId)] != 0);
        CHECK(member.slot == original[seat].slot);
        CHECK(member.save.classUnlock == original[seat].save.classUnlock);
        CHECK(member.save.moviesSeen == original[seat].save.moviesSeen);
        REQUIRE(member.resultsCheckpoint);
        CHECK(member.resultsCheckpoint->gold == 0);
        CHECK(results[seat].totals[0] == 250 + static_cast<s32>(seat));
        CHECK(results[seat].totals[2] == 500);
        const auto other = static_cast<usize>((member.save.character + 1) % kClassCount);
        CHECK(member.save.classes[other].experience ==
              original[seat].save.classes[other].experience);
        CHECK(profile->gameplayCopy().classes[other].experience == 0);
        CHECK(profile->gameplayCopy().classUnlock == 0);
    }
    // A resume after travel must retain the newly loaded scene and its checkpoint.
    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.resume());
    for (s32 tick = 0; tick < 90; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.guest.shown());
    CHECK(pair.host.hostScene()->world()->ref().name == arena);
    CHECK(pair.host.hostScene()->actor(0)->save().gold == 250);
    CHECK(pair.guest.shown()->motion.epoch == pair.wire.host.context().epoch);
}

TEST_CASE("online scene IDs round-trip every playable native level without accepting paths",
          "[netplay][online-play][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::set<u32> seen;
    const auto tower = OnlinePlay::sceneId(catalog, LevelRef::tower());
    REQUIRE(tower);
    seen.insert(*tower);
    CHECK(OnlinePlay::levelOf(catalog, *tower) == LevelRef::tower());
    for (const auto& realm : catalog.realms()) {
        if (realm.id == LevelRef::kTowerRealm) {
            continue;
        }
        for (const auto& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level);
            const auto id = OnlinePlay::sceneId(catalog, *level);
            REQUIRE(id);
            CHECK(seen.insert(*id).second);
            CHECK(OnlinePlay::levelOf(catalog, *id) == level);
            auto spoof = *level;
            spoof.directory = "../../foreign";
            CHECK_FALSE(OnlinePlay::sceneId(catalog, spoof));
        }
    }
    for (u32 id = 0; id <= 65536; ++id) {
        CHECK(OnlinePlay::levelOf(catalog, id).has_value() == seen.contains(id));
    }
}

TEST_CASE("the online scene owner drives four native actors while the guest only presents",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    pair.open();
    REQUIRE(pair.host.hostScene());
    REQUIRE(pair.host.hostScene()->actorCount() == 4);
    CHECK_FALSE(pair.guest.hostScene());
    REQUIRE(pair.guest.replica());
    const auto created = pair.guestDevice.texturesCreated;
    SessionInputs::Frame hostInputs;
    SessionInputs::Frame guestInputs;
    // Non-selected physical slots cannot accidentally drive any party member.
    hostInputs[0].move = {{1, 0}, 1};
    guestInputs[1].move = {{1, 0}, 1};
    hostInputs[1].attack = true;
    guestInputs[2].attack = true;
    usize projectiles = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick);
        pair.step(hostInputs, guestInputs);
        pair.draw();
        if (const auto* shown = pair.guest.shown()) {
            CHECK(shown->motion.players[0]);
            CHECK(shown->motion.players[1]);
            CHECK(shown->motion.players[2]);
            CHECK(shown->motion.players[3]);
            projectiles = std::max(projectiles, shown->projectiles.size());
        }
    }
    CHECK(projectiles > 0);
    CHECK_FALSE(pair.guestDevice.draws.empty());
    CHECK(pair.guestDevice.texturesCreated == created);
    REQUIRE(pair.host.shown());
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.tick <= pair.host.shown()->motion.tick);
    CHECK(pair.guest.hostScene() == nullptr);

    REQUIRE(pair.guest.pause());
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Paused);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Paused);
    const auto pausedTick = pair.wire.host.tick();
    const auto oldEpoch = pair.wire.host.context().epoch;
    for (s32 tick = 0; tick < 180; ++tick) {
        pair.step(hostInputs, guestInputs);
        pair.draw();
    }
    CHECK(pair.wire.host.tick() == pausedTick);
    CHECK_FALSE(pair.guest.resume());
    REQUIRE(pair.host.resume());
    for (s32 tick = 0; tick < 30; ++tick) {
        pair.step();
        pair.draw();
    }
    REQUIRE(pair.host.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.phase() == OnlinePlay::Phase::Playing);
    REQUIRE(pair.guest.shown());
    CHECK(pair.guest.shown()->motion.epoch > oldEpoch);
    CHECK(pair.guestDevice.texturesCreated == created);
    pair.wire.guestSession.leave();
    CHECK(pair.guest.update({}) == OnlinePlay::Phase::Failed);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.host.update({});
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    const auto stoppedTick = pair.wire.host.tick();
    pair.host.update(hostInputs);
    CHECK(pair.wire.host.tick() == stoppedTick);
}

TEST_CASE("a failed guest native load cannot release the host simulation barrier",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    auto missing = pair.context;
    missing.unpackedRoot /= "missing-online-assets";
    REQUIRE(pair.guest.open(pair.guestDevice, missing, pair.wire.guestSession, pair.guestParty));
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty));
    CHECK(pair.host.update({}) == OnlinePlay::Phase::Loading);
    CHECK(pair.guest.update({}) == OnlinePlay::Phase::Failed);
    CHECK(pair.guest.failure() == OnlinePlay::Failure::Assets);
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.guest.update({});
        pair.host.update({});
    }
    CHECK(pair.wire.host.tick() == 0);
    CHECK(pair.host.phase() == OnlinePlay::Phase::Failed);
    CHECK_FALSE(pair.guest.shown());
}
TEST_CASE("all sixteen online classes can repeatedly throw and recover on an independent client",
          "[netplay][online-play][assets]") {
    for (s32 offset = 0; offset < 16; offset += 4) {
        CAPTURE(offset);
        RuntimePair pair(offset);
        pair.open();
        SessionInputs::Frame inputs;
        for (auto& input : inputs) {
            input.attack = true;
        }
        for (s32 tick = 0; tick < 360; ++tick) {
            CAPTURE(tick);
            pair.step(inputs, inputs);
            pair.draw();
        }
        REQUIRE(pair.guest.shown());
        CHECK(pair.host.outcome() == PlayReplication::Result::Advanced);
    }
}

TEST_CASE("guest scene attachment accepts an initial Prepare that overtook local setup",
          "[netplay][online-play][assets]") {
    RuntimePair pair;
    REQUIRE(pair.host.open(pair.hostDevice, pair.context, pair.wire.hostSession, pair.hostParty));
    pair.wire.pump();
    REQUIRE(pair.wire.guest.phase() == MatchSession::Phase::Loading);
    REQUIRE(
        pair.guest.open(pair.guestDevice, pair.context, pair.wire.guestSession, pair.guestParty));
    for (s32 tick = 0; tick < 10; ++tick) {
        pair.step();
    }
    CHECK(pair.host.phase() == OnlinePlay::Phase::Playing);
    CHECK(pair.guest.phase() == OnlinePlay::Phase::Playing);
}
} // namespace
