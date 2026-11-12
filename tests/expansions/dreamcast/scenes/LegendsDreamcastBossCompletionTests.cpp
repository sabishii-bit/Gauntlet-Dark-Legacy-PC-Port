#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBosses.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/config/GameConfig.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/level/BossKinds.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f32 kStep = 1.0f / 60;

std::filesystem::path nativeRootOrSkip() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levelb6/worlds.rom")) {
        SKIP("Native Dreamcast boss arenas are not installed");
    }
    return root;
}

struct BossFixture {
    test::FakeRenderDevice device;
    std::shared_ptr<LegendsLevel> level;
    formats::LegendsMap map;

    explicit BossFixture(std::string_view name) {
        const auto directory = nativeRootOrSkip() / "levels" / name;
        const auto objects = readFile(directory / "objects.rom");
        map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
        map.textures =
            formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
        level = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::dataDirectory());
    }

    void step(std::span<PlayerActor*> players) {
        level->capturePresentation();
        level->update(kStep, false);
        level->updateParty(players, kStep, false);
    }
};

std::array<PartyMember, 3> bossParty() {
    std::array<PartyMember, 3> party;
    constexpr std::array<s32, 3> kSeats{0, 2, 3};
    for (usize i = 0; i < party.size(); ++i) {
        auto& member = party[i];
        member.player = kSeats[i];
        member.slot = 5 + i;
        member.save.name = "BOSS";
        member.save.character = static_cast<s32>(i);
        member.save.gold = 1000;
        // Match a loaded save: serialization resolves the -1 legacy promotion
        // sentinel and banks the live wallet into the selected class.
        for (auto& progress : member.save.classes) {
            progress.promotedLevel = 1;
        }
        member.save.progress().gold = member.save.gold;
        member.save.progress().health = 9000;
        member.save.progress().relics.runes = 0x123;
        member.save.progress().relics.shards = 0x42;
        member.save.progress().levels.recordBeaten(1, 2);
        auto& n64 = member.save.progress().legendsProgress(LegendsPlatform::N64);
        n64.shards = 0x20;
        n64.recordCompleted("mount6");
        auto& dc = member.save.progress().legendsProgress(LegendsPlatform::Dreamcast);
        dc.runestones = u64{1} << i;
        dc.obelisks = 0x38;
        dc.recordCompleted("levelb5");
        REQUIRE(CharacterSave::fromJson(member.save.toJson()) == member.save);
    }
    party[2].fallen = true;
    return party;
}

struct BossPlayers {
    std::array<PlayerActor, 4> actors;
    std::vector<PlayerActor*> standing;

    BossPlayers(std::span<const PartyMember> party, const Vec3& position) {
        for (const auto& member : party) {
            if (member.fallen) {
                continue;
            }
            auto& actor = actors[static_cast<usize>(member.player)];
            actor.spawn(member.player, member.save, nullptr,
                        position + Vec3{static_cast<f32>(member.player), 0, 8}, 0);
            standing.push_back(&actor);
        }
    }

    void store(std::span<PartyMember> party) const {
        for (auto& member : party) {
            if (!member.fallen) {
                member.save = actors[static_cast<usize>(member.player)].save();
            }
        }
    }
};

TEST_CASE("Every shipped Dreamcast boss arena binds its own native boss placement",
          "[legends][dreamcast-boss-completion][dreamcast-bosses][assets]") {
    const auto root = nativeRootOrSkip();
    struct Arena {
        const char* map;
        const char* world;
        const char* archive;
        s32 native;
        s32 kind;
        Vec3 position;
        f32 yaw;
    };
    const std::array arenas{
        Arena{"levelb6",
              "mount",
              "dragon",
              26,
              boss_type::kDragon,
              {-3.4469852f, 31.02504f, -18.403734f},
              -.096192f},
        Arena{"levela5", "castle", "chimera", 27, boss_type::kChimera, {0, 24, -48}, 0},
        Arena{"levelc5", "desert", "djinn", 28, boss_type::kDjinn, {0, -.65f, .5f}, kPi / 2},
        Arena{"leveld5",
              "forest",
              "drider",
              29,
              boss_type::kDrider,
              {-52.75f, 61.25f, -121},
              .925025f},
        Arena{"levele2",
              "temple",
              "skorne1",
              32,
              boss_type::kSkorne1,
              {-.125f, -25.375f, -19.625f},
              0},
        Arena{
            "levelf2", "hell", "skorne2", 33, boss_type::kSkorne2, {0, -13.34375f, -10}, -.034905f},
    };
    for (const auto& arena : arenas) {
        CAPTURE(arena.map);
        const auto boss =
            legendsDreamcastBoss(readFile(root / "worlddata" / (std::string{arena.world} + ".wad")),
                                 readFile(root / "levels" / arena.map / "worlds.rom"), arena.map);
        REQUIRE(boss);
        CHECK(boss->nativeKind == arena.native);
        CHECK(boss->combat.kind == arena.kind);
        CHECK(boss->archive == arena.archive);
        CHECK(glm::distance(boss->combat.position, arena.position) < .0001f);
        CHECK(std::abs(boss->combat.yaw - arena.yaw) < .00001f);
        // The encounter binds Dreamcast art and routes into the common combat
        // runtime. No N64 extraction or donor boss art is needed for admission.
        const BossFixture fixture{arena.map};
        REQUIRE(fixture.level->importedBoss());
        CHECK(fixture.level->importedBoss()->kind == arena.kind);
        CHECK(fixture.level->importedBoss()->position == boss->combat.position);
        CHECK_FALSE(fixture.level->dreamcastBossCeremony().running());
        CHECK_FALSE(fixture.level->destination());

        auto party = bossParty();
        fixture.level->initializeParty(party);
        BossPlayers players{party, boss->combat.position};
        std::vector<CharacterSave> expected;
        for (const auto* player : players.standing) {
            auto save = player->save();
            auto& progress = save.progress().legendsProgress(LegendsPlatform::Dreamcast);
            REQUIRE(progress.add(LegendsRelic::Shard, boss->nativeRealm));
            REQUIRE(progress.recordCompleted(arena.map));
            expected.push_back(std::move(save));
        }
        fixture.level->bossDefeated(boss->combat.position, players.standing);
        CHECK(fixture.level->dreamcastBossCeremony().stage() ==
              LegendsDreamcastBossCeremony::Stage::Waiting);
        CHECK(fixture.level->dreamcastBossCeremony().boss() == arena.native);
        CHECK(fixture.level->protectsParty());
        CHECK_FALSE(fixture.level->destination());
        // BossEnd 8c071300 calls AddShard(player,currentRealm) before Sumner
        // appears. It is not a pickup and Dragon's native realm is 2, not 1.
        for (usize index = 0; index < players.standing.size(); ++index) {
            CHECK(players.standing[index]->save() == expected[index]);
        }
        const s32 wait = fixture.level->dreamcastBossCeremony().ticksLeft();
        for (s32 repeat = 0; repeat < 64; ++repeat) {
            fixture.level->bossDefeated(boss->combat.position, players.standing);
        }
        CHECK(fixture.level->dreamcastBossCeremony().ticksLeft() == wait);
        for (usize index = 0; index < players.standing.size(); ++index) {
            CHECK(players.standing[index]->save() == expected[index]);
        }
    }
}

TEST_CASE("Dreamcast victory cannot be started by invalid or unowned callbacks",
          "[legends][dreamcast-boss-completion][assets]") {
    const BossFixture fixture{"levelb6"};
    auto party = bossParty();
    fixture.level->initializeParty(party);
    const auto* const boss = fixture.level->importedBoss();
    REQUIRE(boss);
    BossPlayers players{party, boss->position};
    const auto first = players.standing.front()->save();
    std::array<PlayerActor*, 3> absent{nullptr, nullptr, nullptr};
    fixture.level->bossDefeated(boss->position, {});
    fixture.level->bossDefeated(boss->position, absent);
    fixture.level->bossDefeated({0, std::numeric_limits<f32>::quiet_NaN(), 0}, players.standing);
    fixture.level->bossDefeated({std::numeric_limits<f32>::infinity(), 0, 0}, players.standing);
    CHECK_FALSE(fixture.level->dreamcastBossCeremony().running());
    CHECK_FALSE(fixture.level->protectsParty());
    CHECK_FALSE(fixture.level->destination());
    CHECK(players.standing.front()->save() == first);

    // Failed attempts cannot consume the one-shot admission of a real death.
    fixture.level->bossDefeated(boss->position, players.standing);
    CHECK(fixture.level->dreamcastBossCeremony().running());
    CHECK(players.standing.front()
              ->save()
              .progress()
              .legendsProgress(LegendsPlatform::Dreamcast)
              .has(LegendsRelic::Shard, 2));
}

TEST_CASE("Dreamcast boss victory completes its ceremony before returning to its own tower",
          "[legends][dreamcast-boss-completion][dreamcast-travel][assets]") {
    BossFixture fixture{"levelb6"};
    auto party = bossParty();
    const auto original = party;
    fixture.level->initializeParty(party);
    const auto* const boss = fixture.level->importedBoss();
    REQUIRE(boss);
    BossPlayers players{party, boss->position};
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    fixture.level->bindStrings(&strings);

    SECTION("quitting an undefeated arena is not a boss victory") {
        fixture.level->completeTravel(party);
        CHECK_FALSE(fixture.level->dreamcastBossCeremony().running());
        CHECK_FALSE(fixture.level->destination());
        for (usize index = 0; index < party.size(); ++index) {
            CHECK(party[index].save == original[index].save);
        }
    }
    SECTION("admitted victory and reload preserve only this campaign's reward") {
        fixture.level->bossDefeated(boss->position, players.standing);
        CHECK_FALSE(fixture.level->departure());
        CHECK_FALSE(fixture.level->destination());
        players.store(party);
        const auto awarded = party;
        // A caller cannot manufacture an early transition while the native
        // five-second waiting stage and subsequent dialogue are still active.
        fixture.level->completeTravel(party);
        for (usize index = 0; index < party.size(); ++index) {
            CHECK(party[index].save == awarded[index].save);
        }
        for (s32 tick = 0; tick < 299; ++tick) {
            fixture.step(players.standing);
        }
        CHECK(fixture.level->dreamcastBossCeremony().stage() ==
              LegendsDreamcastBossCeremony::Stage::Waiting);
        CHECK_FALSE(fixture.level->destination());
        bool captionSeen = false;
        for (s32 tick = 0; tick < 60 * 180 && !fixture.level->destination(); ++tick) {
            fixture.step(players.standing);
            captionSeen |= fixture.level->dreamcastBossCeremony().caption().has_value();
        }
        CHECK(captionSeen);
        REQUIRE(fixture.level->dreamcastBossCeremony().finished());
        REQUIRE(fixture.level->destination());
        CHECK(*fixture.level->destination() == "levelt1");
        const auto departure = fixture.level->departure();
        REQUIRE(departure);
        CHECK(departure->finished);
        CHECK(departure->keepVisible);
        CHECK_FALSE(departure->transportSound);
        players.store(party);
        fixture.level->completeTravel(party);
        const auto completed = party;
        for (s32 repeat = 0; repeat < 4; ++repeat) {
            fixture.level->bossDefeated(boss->position, players.standing);
            fixture.level->completeTravel(party);
            fixture.step(players.standing);
        }
        for (usize index = 0; index < party.size(); ++index) {
            CHECK(party[index].save == completed[index].save);
            CHECK(party[index].slot == original[index].slot);
            CHECK(party[index].save.progress().relics == original[index].save.progress().relics);
            CHECK(party[index].save.progress().levels == original[index].save.progress().levels);
            CHECK(party[index].save.progress().legendsProgress(LegendsPlatform::N64) ==
                  original[index].save.progress().legendsProgress(LegendsPlatform::N64));
            const auto& progress =
                party[index].save.progress().legendsProgress(LegendsPlatform::Dreamcast);
            CHECK(progress.has(LegendsRelic::Shard, 2) == !party[index].fallen);
            CHECK(progress.hasCompleted("levelb6") == !party[index].fallen);
            CHECK_FALSE(progress.hasCompleted("levelt1"));
            CHECK(progress.runestones == original[index]
                                             .save.progress()
                                             .legendsProgress(LegendsPlatform::Dreamcast)
                                             .runestones);
            CHECK(progress.obelisks == original[index]
                                           .save.progress()
                                           .legendsProgress(LegendsPlatform::Dreamcast)
                                           .obelisks);
            party[index].save = CharacterSave::fromJson(party[index].save.toJson());
            CHECK(party[index].save == completed[index].save);
        }
        const BossFixture tower{"levelt1"};
        tower.level->initializeParty(party);
        CHECK_FALSE(tower.level->importedBoss());
        CHECK_FALSE(tower.level->dreamcastBossCeremony().running());
        CHECK_FALSE(tower.level->destination());
    }
    SECTION("failed destination load keeps the reward and cannot trigger a reload loop") {
        fixture.level->bossDefeated(boss->position, players.standing);
        for (s32 tick = 0; tick < 60 * 180 && !fixture.level->destination(); ++tick) {
            fixture.step(players.standing);
        }
        REQUIRE(fixture.level->destination());
        players.store(party);
        const auto awarded = party;
        REQUIRE(fixture.level->cancelDeparture());
        for (s32 tick = 0; tick < 600; ++tick) {
            fixture.step(players.standing);
        }
        CHECK_FALSE(fixture.level->destination());
        fixture.level->completeTravel(party);
        for (usize index = 0; index < party.size(); ++index) {
            CHECK(party[index].save == awarded[index].save);
        }
    }
}

TEST_CASE("Dreamcast Dragon enters victory through the actual opponent death callback",
          "[legends][dreamcast-boss-completion][dreamcast-boss-integration][assets]") {
    BossFixture fixture{"levelb6"};
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    const auto* const boss = fixture.level->importedBoss();
    REQUIRE(boss);
    CharacterSave save;
    save.name = "DRAGON";
    save.progress().health = 9000;
    save.progress().legendsProgress(LegendsPlatform::N64).shards = 0x40;
    std::array<PartyMember, 1> party;
    party[0].player = 0;
    party[0].save = save;
    fixture.level->initializeParty(party);
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, save);
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "dreamcast:levelb6";
    REQUIRE(world.loadImported(
        fixture.device, common, ref,
        WorldLayout::fromDecoded({}, {}, fixture.map.minBounds, fixture.map.maxBounds),
        traversal.collision(), fixture.level));
    CHECK(world.level() == nullptr);
    ItemArchive weapons;
    REQUIRE(weapons.load(common / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    audio.open(common, nullptr, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, save, nullptr, boss->position + Vec3{0, 0, 10}, 0);
    std::array visitors{&players[0].actor};
    LevelOpponents opponents;
    opponents.open({fixture.device, world, weapons, effects, audio, common}, players);
    REQUIRE(opponents.bosses().present());
    CHECK(opponents.bosses().view().kind == boss_type::kDragon);
    CHECK(opponents.bosses().view().alive);
    CHECK_FALSE(opponents.bosses().view().awake);

    test::OpponentScript events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.destroyedGenerator = [](s32) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    usize victories = 0;
    events.fallen = [&](const Vec3& position) {
        ++victories;
        fixture.level->bossDefeated(opponents.bosses().keySpot().value_or(position), visitors);
    };
    test::ScriptedOpponents host{events};
    const auto tick = [&] {
        const std::array subjects{
            CameraSubject{players[0].actor.position(), players[0].actor.followPoint(), 3}};
        fixture.level->capturePresentation();
        fixture.level->followCamera(subjects, kStep);
        fixture.level->update(kStep, false);
        opponents.update(1, kStep, players, {}, host, host, host);
    };
    fixture.level->resetCamera(
        std::array{CameraSubject{players[0].actor.position(), players[0].actor.followPoint(), 3}});
    for (s32 frame = 0; frame < 1800 && !opponents.bosses().view().awake; ++frame) {
        REQUIRE(opponents.bosses().position());
        players[0].actor.place(*opponents.bosses().position() + Vec3{0, 0, 10});
        tick();
    }
    REQUIRE(opponents.bosses().view().awake);
    CHECK(victories == 0);
    CHECK_FALSE(fixture.level->dreamcastBossCeremony().running());
    CHECK_FALSE(players[0]
                    .actor.save()
                    .progress()
                    .legendsProgress(LegendsPlatform::Dreamcast)
                    .has(LegendsRelic::Shard, 2));

    EnemyHit hit;
    hit.player = 0;
    hit.damage = 100000;
    bool dyingBeforeReward = false;
    for (s32 frame = 0; frame < 1800 && victories == 0; ++frame) {
        if (opponents.bosses().view().alive) {
            opponents.bosses().hurt(hit);
        }
        if (!opponents.bosses().view().alive && opponents.bosses().present()) {
            dyingBeforeReward = true;
            CHECK_FALSE(fixture.level->dreamcastBossCeremony().running());
        }
        tick();
    }
    CHECK(dyingBeforeReward);
    REQUIRE(victories == 1);
    CHECK_FALSE(opponents.bosses().present());
    CHECK(fixture.level->dreamcastBossCeremony().stage() ==
          LegendsDreamcastBossCeremony::Stage::Waiting);
    CHECK_FALSE(fixture.level->destination());
    CHECK(players[0]
              .actor.save()
              .progress()
              .legendsProgress(LegendsPlatform::Dreamcast)
              .has(LegendsRelic::Shard, 2));
    CHECK(players[0].actor.save().progress().legendsProgress(LegendsPlatform::N64).shards == 0x40);
    for (s32 frame = 0; frame < 60 * 60 && !fixture.level->destination(); ++frame) {
        tick();
    }
    REQUIRE(fixture.level->destination());
    CHECK(*fixture.level->destination() == "levelt1");
    CHECK(victories == 1);
    CHECK(fixture.level->dreamcastBossCeremony().finished());
    party[0].save = players[0].actor.save();
    fixture.level->completeTravel(party);
    CHECK(party[0]
              .save.progress()
              .legendsProgress(LegendsPlatform::Dreamcast)
              .hasCompleted("levelb6"));
    opponents.close();
    audio.close();
}

TEST_CASE("Dreamcast E2 entrance cues move the native floor through the real opponent route",
          "[legends][dc-boss-entrance][dreamcast-boss-integration][assets]") {
    BossFixture fixture{"levele2"};
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    const auto* const boss = fixture.level->importedBoss();
    REQUIRE(boss);
    CharacterSave save;
    save.name = "FLOOR";
    save.progress().health = 9000;
    std::array<PartyMember, 1> party;
    party[0].player = 0;
    party[0].save = save;
    fixture.level->initializeParty(party);
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, save);
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "dreamcast:levele2";
    REQUIRE(world.loadImported(
        fixture.device, common, ref,
        WorldLayout::fromDecoded({}, {}, fixture.map.minBounds, fixture.map.maxBounds),
        traversal.collision(), fixture.level));
    ItemArchive weapons;
    REQUIRE(weapons.load(common / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    audio.open(common, nullptr, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, save, nullptr, {.5f, -9.75f, 31}, 0);
    LevelOpponents opponents;
    opponents.open({fixture.device, world, weapons, effects, audio, common}, players);
    REQUIRE(opponents.bosses().present());
    REQUIRE_FALSE(opponents.bosses().view().awake);

    std::vector<std::pair<s32, Vec3>> floors;
    for (s32 index = 21; index <= 44; ++index) {
        if (index == 36 || index == 37) {
            continue; // Separate subtype 40 pieces, not SafeRockSetup's subtype 51 roster.
        }
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + index;
        const auto object = fixture.level->adapter().fixtureObject(id);
        REQUIRE(object);
        REQUIRE(object->visible);
        floors.emplace_back(id, object->position);
    }
    REQUIRE(floors.size() == 22);
    test::OpponentScript events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.destroyedGenerator = [](s32) {};
    events.shake = [] {};
    events.help = [](s32, usize) { return false; };
    events.fallen = [](const Vec3&) {};
    test::ScriptedOpponents host{events};
    const std::array subjects{
        CameraSubject{players[0].actor.position(), players[0].actor.followPoint(), 3}};
    fixture.level->resetCamera(subjects);
    bool changed = false;
    for (s32 frame = 0; frame < 1800 && !changed; ++frame) {
        fixture.level->capturePresentation();
        fixture.level->followCamera(subjects, kStep);
        fixture.level->update(kStep, false);
        opponents.update(1, kStep, players, {}, host, host, host);
        usize moved = 0;
        for (const auto& [id, start] : floors) {
            const auto object = fixture.level->adapter().fixtureObject(id);
            REQUIRE(object);
            if (!opponents.bosses().view().awake) {
                CHECK(object->position == start);
            }
            moved += static_cast<usize>(object->position.y > start.y + .01f);
        }
        changed = moved == floors.size();
    }
    REQUIRE(opponents.bosses().view().awake);
    CHECK(changed);
    // No manual bossArenaCue or bossIntro invocation: the START move's SFXX
    // flags must travel Combatant -> LevelOpponents -> LevelWorld -> adapter.
    CHECK(opponents.bosses().fighter().moveName() == "START");
}

TEST_CASE("Dreamcast Skorne arenas keep the live player in view through entrance and startup",
          "[legends][dreamcast-camera][dreamcast-boss-integration][assets]") {
    const auto common = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    for (const auto* name : {"levele2", "levelf2"}) {
        CAPTURE(name);
        BossFixture fixture{name};
        REQUIRE_FALSE(fixture.map.starts.empty());
        CharacterSave save;
        save.name = "CAMERA";
        save.progress().health = 9000;
        const std::array party{PartyMember{0, save}};
        fixture.level->initializeParty(party);
        LegendsTraversal traversal;
        traversal.open(fixture.map, 0, save);
        const auto& start = fixture.map.starts.front();
        LevelWorld world;
        LevelRef ref;
        ref.realmId = -1;
        ref.name = std::string{"dreamcast:"} + name;
        REQUIRE(world.loadImported(
            fixture.device, common, ref,
            WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Start, .position = start.position}},
                                     fixture.map.minBounds, fixture.map.maxBounds),
            traversal.collision(), fixture.level));
        const AssetLocator assets{common};
        const GameConfig config;
        StringTable strings;
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        GameServices services;
        services.config = &config;
        services.strings = &strings;
        services.assets = &assets;
        services.unpackedRoot = common;
        PlayOptions options;
        options.welcome = false;
        PlayScene play;
        REQUIRE(play.open(fixture.device, services, world, party, options));
        bool entered = false;
        s32 awakeFrames = 0;
        const auto checkPlayer = [&] {
            REQUIRE(play.actor(0));
            const Vec3 position = play.actor(0)->position();
            const auto camera = play.viewCamera();
            const Vec4 clip =
                WorldCamera::projection(kPi / 3, 640.0f / 416) * camera.view() * Vec4{position, 1};
            CAPTURE(position.x, position.y, position.z, camera.position.x, camera.position.y,
                    camera.position.z, clip.x, clip.y, clip.w);
            REQUIRE(clip.w > 0);
            CHECK(std::abs(clip.x / clip.w) < 1);
            CHECK(std::abs(clip.y / clip.w) < 1);
        };
        for (s32 tick = 0; tick < 1800 && awakeFrames < 360; ++tick) {
            REQUIRE(play.update(kStep, {}) == PlayOutcome::Running);
            if (!entered && fixture.level->opponentsReady()) {
                entered = true;
            }
            if (entered) {
                CAPTURE(tick, awakeFrames, play.bosses().fighter().moveName());
                checkPlayer();
            }
            if (play.bosses().view().awake) {
                ++awakeFrames;
            }
        }
        REQUIRE(entered);
        REQUIRE(awakeFrames == 360);
        checkPlayer();
    }
}

TEST_CASE("Dreamcast victory speech reaches the mixer once and survives an offline pause",
          "[legends][dreamcast-boss-completion][dreamcast-sounds][assets]") {
    const auto native = nativeRootOrSkip();
    if (!std::filesystem::is_regular_file(native.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
        SKIP("Dreamcast speech banks are not installed");
    }
    AudioMixer mixer{48000};
    AudioMixer referenceMixer{48000};
    SoundPlayer output{mixer};
    SoundPlayer reference{referenceMixer};
    LegendsDreamcastSounds bank;
    REQUIRE(bank.open(native));
    std::vector<SoundPlayer::Event> referenceEvents;
    const SoundPlayer::Capture referenceCapture{
        &reference, [&](const SoundPlayer::Event& event) { referenceEvents.push_back(event); }};
    REQUIRE(bank.play(reference, 0x27000a) != kNoSound);
    REQUIRE(referenceEvents.size() == 1);
    const auto target = referenceEvents.front();
    REQUIRE(target.kind == SoundPlayer::Event::Kind::Play);
    CHECK(target.bank.starts_with("dreamcast/"));

    BossFixture fixture{"levelb6"};
    auto party = bossParty();
    fixture.level->initializeParty(party);
    const auto* boss = fixture.level->importedBoss();
    REQUIRE(boss);
    BossPlayers players{party, boss->position};
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    fixture.level->bindStrings(&strings);
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture{
        &output, [&](const SoundPlayer::Event& event) { events.push_back(event); }};
    const auto isDefeatVoice = [&](const SoundPlayer::Event& event) {
        return event.kind == SoundPlayer::Event::Kind::Play && event.bank == target.bank &&
               event.call == target.call;
    };
    const auto defeatVoices = [&] { return std::ranges::count_if(events, isDefeatVoice); };
    std::array<f32, 1600> samples{}; // one 60 Hz frame of stereo output at 48 kHz
    const auto serviceAudio = [&] {
        fixture.level->playSounds(output, players.standing.front()->position());
        output.update();
        mixer.mix(samples);
    };
    fixture.level->bossDefeated(boss->position, players.standing);
    for (s32 tick = 0; tick < 1800 && defeatVoices() == 0; ++tick) {
        fixture.step(players.standing);
        serviceAudio();
    }
    REQUIRE(defeatVoices() == 1);
    const auto event = std::ranges::find_if(events, isDefeatVoice);
    REQUIRE(event != events.end());
    const SoundHandle voice = event->handle;
    REQUIRE(output.isPlaying(voice));
    const auto stage = fixture.level->dreamcastBossCeremony().stage();
    const s32 wait = fixture.level->dreamcastBossCeremony().ticksLeft();
    const auto caption = fixture.level->dreamcastBossCeremony().caption();
    REQUIRE(caption);
    fixture.level->pauseSounds();
    output.setCategoryPaused(SoundCategory::Effects, true);
    for (s32 tick = 0; tick < 180; ++tick) {
        // The paused scene does not advance world time; servicing audio and
        // zero-tick updates must not restart narration or consume its cursor.
        fixture.level->update(0, false);
        serviceAudio();
        CHECK(std::ranges::all_of(samples, [](f32 sample) { return sample == 0; }));
    }
    CHECK(output.isPlaying(voice));
    CHECK(defeatVoices() == 1);
    CHECK(fixture.level->dreamcastBossCeremony().stage() == stage);
    CHECK(fixture.level->dreamcastBossCeremony().ticksLeft() == wait);
    REQUIRE(fixture.level->dreamcastBossCeremony().caption());
    CHECK(fixture.level->dreamcastBossCeremony().caption()->shown == caption->shown);
    CHECK(fixture.level->dreamcastBossCeremony().caption()->page == caption->page);
    CHECK(fixture.level->dreamcastBossCeremony().caption()->group == caption->group);
    output.setCategoryPaused(SoundCategory::Effects, false);
    f64 energy = 0;
    for (s32 tick = 0; tick < 60 * 180 && !fixture.level->destination(); ++tick) {
        fixture.step(players.standing);
        serviceAudio();
        for (const f32 sample : samples) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .001);
    CHECK(defeatVoices() == 1);
    REQUIRE(fixture.level->destination());
    CHECK(*fixture.level->destination() == "levelt1");
    CHECK_FALSE(output.isPlaying(voice));
    // All three authored speech groups use this boss's Dreamcast bank; none
    // may silently fall back to a similarly named Dark Legacy/N64 sample.
    for (const u32 call : {0x0aU, 0x0bU, 0x0fU}) {
        CHECK(std::ranges::count_if(events, [&](const SoundPlayer::Event& value) {
                  return value.kind == SoundPlayer::Event::Kind::Play &&
                         value.bank == target.bank && value.call == call;
              }) == 1);
    }
    fixture.level->stopSounds();
    reference.stopAll();
}

TEST_CASE("Absent Dreamcast speech and wizard resources cannot strand boss completion",
          "[legends][dreamcast-boss-completion][dreamcast-sounds]") {
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    LegendsDreamcastSounds missing;
    REQUIRE_FALSE(missing.open(test::scratchDirectory("dc-victory-no-audio") / "native"));
    LegendsDreamcastBossVisit visit;
    const std::array players{Vec3{3, 0, 10}};
    REQUIRE(visit.begin(26, 2, 0, 4, Vec3{0}, players));
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture{
        &output, [&](const SoundPlayer::Event& event) { events.push_back(event); }};
    std::array<f32, 1600> samples{};
    for (s32 tick = 0; tick < 60 * 60 && !visit.state().finished(); ++tick) {
        visit.update(kStep);
        visit.playSounds(output, missing);
        output.update();
        mixer.mix(samples);
    }
    CHECK(visit.state().finished());
    CHECK(events.empty());
    CHECK(output.voiceCount() == 0);
    CHECK(std::ranges::all_of(samples, [](f32 sample) { return sample == 0; }));
    visit.stopSounds();
}

TEST_CASE("Dreamcast Chimera follows the actual A5 elevator before awakening",
          "[legends][dc-boss-ai][dc-boss-elevator][dreamcast-boss-integration][assets]") {
    BossFixture fixture{"levela5"};
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    const auto* const encounter = fixture.level->importedBoss();
    REQUIRE(encounter);
    const auto* motion = fixture.level->adapter().dreamcastWorldMotion();
    REQUIRE(motion);
    const auto pad = std::ranges::find(motion->switches(), 8U,
                                       &LegendsDreamcastWorldMotion::Switch::nativeIndex);
    REQUIRE(pad != motion->switches().end());
    REQUIRE(pad->target == 15);
    CharacterSave save;
    save.name = "RIDER";
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, save);
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "dreamcast:levela5";
    REQUIRE(world.loadImported(
        fixture.device, common, ref,
        WorldLayout::fromDecoded({}, {}, fixture.map.minBounds, fixture.map.maxBounds),
        traversal.collision(), fixture.level));
    Bosses bosses;
    bosses.open(fixture.device, common, &world.collision(), {}, 'A');
    REQUIRE(bosses.preload(encounter->kind, fixture.level->loadBossAssets(fixture.device, common)));
    REQUIRE(bosses.spawn(encounter->kind, encounter->position, encounter->yaw));
    const auto initial = world.collision().floorAt(bosses.fighter().position(), 4, 1000);
    REQUIRE(initial);
    CHECK(initial->object >= 15);
    CHECK(initial->object <= 20);
    const Vec3 bossStart = bosses.fighter().position();
    REQUIRE((bosses.fighter().data()->typeFlags() & 0x80U) != 0);
    const std::array closePlayer{EnemyView{.player = 0, .position = bossStart + Vec3{0, 0, 10}}};
    // Mere proximity cannot start TYPE0x80: the support's moving trigger must
    // arm the native two-second deadline first.
    for (s32 tick = 0; tick < 180; ++tick) {
        world.update(kStep, false);
        CHECK_FALSE(fixture.level->bossActivationReady(bosses.fighter().position()));
        bosses.update(1, kStep, closePlayer,
                      fixture.level->bossActivationReady(bosses.fighter().position()));
        CHECK_FALSE(bosses.view().awake);
    }

    // Touch the authored switch with an actually grounded player, not a direct
    // world-matrix mutation. Its target and all five floor children come from A5.
    Vec3 standing = motion->switchPosition(static_cast<usize>(pad - motion->switches().begin()));
    const auto padFloor = world.collision().floorAt(standing, 4, 1000);
    REQUIRE(padFloor);
    standing.y = padFloor->y;
    PlayerActor visitor;
    visitor.spawn(0, save, nullptr, standing, 0);
    std::array visitors{&visitor};
    fixture.level->updateParty(visitors, kStep, false);
    REQUIRE(pad->active);
    const std::array distantPlayer{EnemyView{.player = 0, .position = {0, 0, 500}}};
    bool descended = false;
    for (s32 tick = 0; tick < 360; ++tick) {
        world.update(kStep, false);
        const Vec3 expected{motion->riderTransform(initial->object) * Vec4{bossStart, 1}};
        if (tick == 0) {
            CHECK_FALSE(world.collision().solid(initial->object));
            CHECK(world.collision().solid(initial->object, 2));
            CHECK(fixture.level->bossActivationReady(bosses.fighter().position()));
        }
        bosses.update(1, kStep, distantPlayer,
                      fixture.level->bossActivationReady(bosses.fighter().position()));
        CAPTURE(tick, expected.y, bosses.fighter().position().y);
        CHECK_FALSE(bosses.view().awake);
        CHECK(std::abs(bosses.fighter().position().y - expected.y) < .002f);
        for (s32 child = 1; child <= static_cast<s32>(bosses.fighter().childCount()); ++child) {
            const auto* part = bosses.fighter().child(bosses.fighter().id() + child);
            REQUIRE(part);
            CHECK(std::abs(part->position().y - expected.y) < .002f);
        }
        descended |= expected.y < bossStart.y - 1;
    }
    CHECK(descended);
    CHECK(std::abs(bosses.fighter().position().y - (bossStart.y - 23)) < .002f);
    CHECK_FALSE(fixture.level->bossActivationReady(bosses.fighter().position()));
    // A stopped platform does not disarm the elapsed deadline. A late-arriving
    // party starts the fight immediately after reaching the native wake range.
    const std::array arrived{
        EnemyView{.player = 0, .position = bosses.fighter().position() + Vec3{0, 0, 10}}};
    bosses.update(1, kStep, arrived, false);
    CHECK(bosses.view().awake);
}

TEST_CASE("Dreamcast Chimera entrance cannot pre-arm the platform activation deadline",
          "[legends][dc-boss-ai][dc-boss-elevator][dc-boss-entrance-arm][assets]") {
    BossFixture fixture{"levela5"};
    const auto common = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    REQUIRE_FALSE(fixture.map.starts.empty());
    CharacterSave save;
    save.name = "RIDER";
    save.progress().health = 9000;
    const std::array party{PartyMember{0, save}};
    fixture.level->initializeParty(party);
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, save);
    const auto& start = fixture.map.starts.front();
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "dreamcast:levela5";
    REQUIRE(world.loadImported(
        fixture.device, common, ref,
        WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Start, .position = start.position}},
                                 fixture.map.minBounds, fixture.map.maxBounds),
        traversal.collision(), fixture.level));
    const AssetLocator assets{common};
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameServices services;
    services.config = &config;
    services.strings = &strings;
    services.assets = &assets;
    services.unpackedRoot = common;
    PlayOptions options;
    options.welcome = false;
    PlayScene play;
    REQUIRE(play.open(fixture.device, services, world, party, options));
    REQUIRE(play.spawning());
    for (s32 tick = 0; tick < 900 && play.spawning(); ++tick) {
        REQUIRE(play.update(kStep, {}) == PlayOutcome::Running);
        CHECK_FALSE(play.bosses().view().awake);
    }
    REQUIRE_FALSE(play.spawning());
    REQUIRE(play.bosses().position());
    const Vec3 boss = *play.bosses().position();
    // This would wake immediately if the held entrance had incremented the
    // sticky clock. No platform signal has occurred, even after three seconds.
    REQUIRE(play.relocate(boss + Vec3{0, 0, 10}));
    for (s32 tick = 0; tick < 180; ++tick) {
        REQUIRE(play.update(kStep, {}) == PlayOutcome::Running);
        CHECK_FALSE(fixture.level->bossActivationReady(*play.bosses().position()));
        CHECK_FALSE(play.bosses().view().awake);
        CHECK_FALSE(play.bossSubject().engaged);
    }

    const auto* motion = fixture.level->adapter().dreamcastWorldMotion();
    REQUIRE(motion);
    const auto pad = std::ranges::find(motion->switches(), 8U,
                                       &LegendsDreamcastWorldMotion::Switch::nativeIndex);
    REQUIRE(pad != motion->switches().end());
    REQUIRE_FALSE(pad->active);
    Vec3 switchPoint = motion->switchPosition(static_cast<usize>(pad - motion->switches().begin()));
    const auto floor = world.collision().floorAt(switchPoint, 4, 1000);
    REQUIRE(floor);
    switchPoint.y = floor->y;
    REQUIRE(play.relocate(switchPoint));
    bool moving = false;
    // ProcessCritter's arming calls BossActivate (0x8c08b7c4), whose gBossActive turns the
    // arena camera to the boss (0x8c082d8c) two seconds before the deadline it set can pass.
    s32 engaged = -1;
    s32 woke = -1;
    for (s32 tick = 0; tick < 600 && !play.bosses().view().awake; ++tick) {
        REQUIRE(play.update(kStep, {}) == PlayOutcome::Running);
        moving |= fixture.level->bossActivationReady(*play.bosses().position());
        if (engaged < 0 && play.bossSubject().engaged) {
            engaged = tick;
            CHECK_FALSE(play.bosses().view().awake);
        }
        if (play.bosses().view().awake) {
            woke = tick;
        }
    }
    CHECK(pad->active);
    CHECK(moving);
    CHECK(play.bosses().view().awake);
    CHECK(play.bosses().position()->y < boss.y - 1);
    REQUIRE(engaged >= 0);
    // f32 sums of 1/60 first reach two seconds at 121 additions.
    CHECK(woke - engaged >= 121);
}

} // namespace
