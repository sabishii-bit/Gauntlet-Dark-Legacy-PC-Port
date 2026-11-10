#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/shared/app/LegendsCompletion.h"
#include "expansions/shared/app/LegendsConsoleWords.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/app/LevelCompletion.h"
#include "game/characters/AutoSave.h"
#include "game/players/PlayerAnimator.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayerSelectScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct CompletionFixture {
    const std::filesystem::path common = test::assetOrSkip("PLAYERS").parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    GameServices services;
    LevelWorld world;
    PlayScene scene;
    std::shared_ptr<LegendsLevel> native;
    Vec3 exitPosition{0};

    CompletionFixture(LegendsPlatform platform, bool tower, std::string_view mapName = {},
                      std::span<const PartyMember> carried = {}) {
        const auto assets = test::dataDirectory().parent_path() / "assets";
        const bool n64 = platform == LegendsPlatform::N64;
        config.save.directory =
            test::scratchDirectory(n64 ? "completion-n64" : "completion-dreamcast").string();
        config.save.slots = 4;
        const auto directory =
            assets / (n64 ? "n64/extracted/levels" : "dreamcast/extracted/native/levels") /
            (mapName.empty()
                 ? (n64 ? (tower ? "tower2" : "mount1") : (tower ? "levelt1" : "levelb1"))
                 : mapName);
        const auto worldFile = directory / (n64 ? "worlds.wad" : "worlds.rom");
        if (!std::filesystem::is_regular_file(worldFile)) {
            SKIP("Optional native Legends extraction is not installed");
        }
        const auto objects = readFile(directory / (n64 ? "objects.wad" : "objects.rom"));
        auto map = n64 ? formats::LegendsMap::parseN64(readFile(worldFile), objects,
                                                       readFile(directory / "ipr.rom"))
                       : formats::LegendsMap::parseDreamcast(readFile(worldFile), objects);
        auto adapter = LegendsSceneAdapter::load(device, platform, directory, map);
        REQUIRE(adapter);
        const std::string destination = mapName == "levele1"   ? "levele2"
                                        : mapName == "levelf1" ? "levelf2"
                                        : mapName == "levelh1" ? "levelh4"
                                        : mapName == "levelh2" ? "levelh5"
                                        : mapName == "levelh3" ? "levelh6"
                                        : n64                  ? (tower ? "mount1" : "tower2")
                                                               : (tower ? "levelb1" : "levelt1");
        if (n64) {
            const auto found =
                std::ranges::find(adapter->portals(), destination, &formats::LegendsN64Portal::map);
            REQUIRE(found != adapter->portals().end());
            exitPosition = found->position;
        } else if (mapName != "levele2" && mapName != "levelf2") {
            const auto found = std::ranges::find(adapter->dreamcastPortals(), destination,
                                                 &LegendsDreamcastPortal::map);
            REQUIRE(found != adapter->dreamcastPortals().end());
            exitPosition = found->position;
        }
        PartyMember member;
        member.player = 0;
        member.slot = 3;
        member.save.name = "TALLY";
        member.save.gold = 750;
        member.save.progress().health = 9000;
        member.resultsCheckpoint = LevelResults::Checkpoint{100, 0, 7};
        const std::vector<PartyMember> party =
            carried.empty() ? std::vector{member} : std::vector(carried.begin(), carried.end());
        LegendsTraversal entrance;
        entrance.open(map, 0, member.save);
        native = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
        native->initializeParty(party);
        LevelRef ref;
        ref.realmId = -1;
        ref.name = directory.filename().string();
        ref.directory = directory.string();
        REQUIRE(world.loadImported(
            device, common, ref,
            WorldLayout::fromDecoded({},
                                     {{.kind = LocatorKind::Start, .position = entrance.start()}},
                                     map.minBounds, map.maxBounds),
            entrance.collision(), native));
        services.config = &config;
        services.unpackedRoot = common;
        PlayOptions options;
        options.position = entrance.start();
        options.welcome = false;
        options.consoleCheats[0] = {.god = true, .noTarget = true};
        REQUIRE(scene.open(device, services, world, party, options));
        const auto entering = [&] {
            const auto* runtime = scene.runtime(0);
            return scene.spawning() || !native->opponentsReady() ||
                   (runtime != nullptr && runtime->figure &&
                    runtime->figure->animator().entering());
        };
        for (s32 tick = 0; tick < 900 && entering(); ++tick) {
            REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
        REQUIRE(scene.canPause(0));
    }

    void leave() {
        REQUIRE(scene.noclipMove(0, exitPosition - scene.actor(0)->position(), std::nullopt));
        PlayOutcome outcome = PlayOutcome::Running;
        for (s32 tick = 0; tick < 900 && outcome == PlayOutcome::Running; ++tick) {
            // Completion must remain unavailable during admission and animation.
            CHECK_FALSE(completedLegendsTravel(scene));
            outcome = scene.update(1.0 / 60, {});
        }
        REQUIRE(outcome == PlayOutcome::Travel);
        REQUIRE(native->destination());
    }

    void defeatBoss() {
        const auto* const boss = native->importedBoss();
        REQUIRE(boss);
        PlayerActor victor;
        victor.spawn(0, *scene.liveSave(0), nullptr, scene.actor(0)->position(), 0);
        std::array<PlayerActor*, 1> players{&victor};
        native->bossDefeated(boss->position, players);
        *scene.liveSave(0) = victor.save();
        PlayOutcome outcome = PlayOutcome::Running;
        for (s32 tick = 0; tick < 60 * 180 && outcome == PlayOutcome::Running; ++tick) {
            outcome = scene.update(1.0 / 60, {});
        }
        REQUIRE(outcome == PlayOutcome::Travel);
        REQUIRE(native->destination());
        REQUIRE(*native->destination() == "levelt1");
    }
};

TEST_CASE("Dreamcast Skorne chains retain approach results through the boss's final shop",
          "[legends][legends-completion][dreamcast-linked-results][assets]") {
    for (const auto* const approach : {"levele1", "levelf1"}) {
        DYNAMIC_SECTION(approach) {
            CompletionFixture first{LegendsPlatform::Dreamcast, false, approach};
            first.scene.liveSave(0)->gold += 300;
            first.scene.liveSave(0)->progress().experience += 25;
            first.leave();
            CHECK_FALSE(completedLegendsTravel(first.scene));
            auto carried = first.scene.party();
            const auto before = carried;
            first.native->completeTravel(carried);
            REQUIRE(carried[0].resultsCheckpoint);
            CHECK(carried[0].resultsCheckpoint == before[0].resultsCheckpoint);
            CHECK(carried[0].slot == 3);
            CHECK(carried[0]
                      .save.progress()
                      .legendsProgress(LegendsPlatform::Dreamcast)
                      .hasCompleted(approach));
            // Preparing a destination does not rewrite the retained source;
            // a failed load can retry without awarding or adding kills twice.
            CHECK(first.scene.party()[0].save == before[0].save);
            const auto retry = carried;
            first.native->completeTravel(carried);
            CHECK(carried[0].save == retry[0].save);
            CHECK(carried[0].resultsCheckpoint == retry[0].resultsCheckpoint);

            CompletionFixture boss{LegendsPlatform::Dreamcast, false, *first.native->destination(),
                                   carried};
            const auto baseline = *carried[0].resultsCheckpoint;
            CHECK(boss.scene.levelResults()[0].totals == first.scene.levelResults()[0].totals);
            boss.scene.liveSave(0)->gold += 200;
            boss.scene.liveSave(0)->progress().experience += 40;
            boss.defeatBoss();
            const auto final = completedLegendsTravel(boss.scene);
            REQUIRE(final);
            REQUIRE(final->results.size() == 1);
            REQUIRE(final->party.size() == 1);
            CHECK_FALSE(final->party[0].resultsCheckpoint);
            CHECK(final->party[0].slot == 3);
            CHECK(final->results[0].totals[0] == final->party[0].save.gold - baseline.gold);
            CHECK(final->results[0].totals[0] >= 1150);
            CHECK(final->results[0].totals[1] >= baseline.kills);
            CHECK(final->results[0].totals[2] ==
                  final->party[0].save.experience() - baseline.experience);
            CHECK(final->results[0].totals[2] >= 65);
            CHECK(final->party[0]
                      .save.progress()
                      .legendsProgress(LegendsPlatform::Dreamcast)
                      .hasCompleted(*first.native->destination()));
            AfterLevelScene shop;
            REQUIRE(shop.open(boss.device, boss.services, final->party, final->results,
                              {1000, 100, 1000}, approach, ShopVisit::Level));
            CHECK(shop.session().lanes()[0].tally.results().totals == final->results[0].totals);
        }
    }
}

TEST_CASE("Dreamcast Battlefield halves travel directly and combine their final results",
          "[legends][legends-completion][dreamcast-linked-results][assets]") {
    for (const auto* const approach : {"levelh1", "levelh2", "levelh3"}) {
        DYNAMIC_SECTION(approach) {
            CompletionFixture first{LegendsPlatform::Dreamcast, false, approach};
            first.scene.liveSave(0)->gold += 300;
            first.scene.liveSave(0)->progress().experience += 25;
            first.leave();
            CHECK_FALSE(completedLegendsTravel(first.scene));
            auto carried = first.scene.party();
            const auto baseline = carried[0].resultsCheckpoint;
            REQUIRE(baseline);
            first.native->completeTravel(carried);
            CHECK(carried[0].resultsCheckpoint == baseline);
            CHECK(carried[0]
                      .save.progress()
                      .legendsProgress(LegendsPlatform::Dreamcast)
                      .hasCompleted(approach));
            const auto destination = *first.native->destination();
            CompletionFixture second{LegendsPlatform::Dreamcast, false, destination, carried};
            CHECK(second.scene.levelResults()[0].totals == first.scene.levelResults()[0].totals);
            second.scene.liveSave(0)->gold += 200;
            second.scene.liveSave(0)->progress().experience += 40;
            second.leave();
            const auto final = completedLegendsTravel(second.scene);
            REQUIRE(final);
            REQUIRE(final->results.size() == 1);
            REQUIRE(final->party.size() == 1);
            CHECK(final->destination.filename() == "levelt1");
            CHECK_FALSE(final->party[0].resultsCheckpoint);
            CHECK(final->party[0].slot == 3);
            CHECK(final->results[0].totals[0] == final->party[0].save.gold - baseline->gold);
            CHECK(final->results[0].totals[0] >= 1150);
            CHECK(final->results[0].totals[1] >= baseline->kills);
            CHECK(final->results[0].totals[2] ==
                  final->party[0].save.experience() - baseline->experience);
            CHECK(final->results[0].totals[2] >= 65);
            CHECK(final->party[0]
                      .save.progress()
                      .legendsProgress(LegendsPlatform::Dreamcast)
                      .hasCompleted(destination));
        }
    }
}

TEST_CASE("Native level exits retain their results through piles and shopping",
          "[legends][legends-completion][assets][shop]") {
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        DYNAMIC_SECTION(legendsPlatformName(platform)) {
            CompletionFixture fixture{platform, false};
            auto& scene = fixture.scene;
            CHECK_FALSE(completedLegendsTravel(scene)); // Also the Quit Level path.
            scene.liveSave(0)->gold += 300;
            scene.liveSave(0)->progress().experience += 25;
            fixture.leave();
            const auto before = scene.party();
            auto completion = completedLegendsTravel(scene);
            REQUIRE(completion);
            REQUIRE(completion->party.size() == 1);
            REQUIRE(completion->results.size() == 1);
            CHECK(completion->platform == platform);
            CHECK(completion->destination ==
                  fixture.native->directory().parent_path() / *fixture.native->destination());
            CHECK(completion->party[0].slot == 3);
            CHECK_FALSE(completion->party[0].resultsCheckpoint);
            CHECK(completion->results[0].totals == scene.levelResults()[0].totals);
            CHECK(completion->results[0].totals[0] == 950);
            CHECK(completion->results[0].totals[1] >= 7);
            CHECK(completion->results[0].totals[2] >= 25);
            const auto source = fixture.native->directory().filename().string();
            CHECK(completion->party[0].save.progress().legendsProgress(platform).hasCompleted(
                source));
            CHECK(scene.party()[0].save ==
                  before[0].save); // Capture never writes the parked actor.
            const auto repeated = completedLegendsTravel(scene);
            REQUIRE(repeated);
            CHECK(repeated->party[0].save == completion->party[0].save);

            const auto earnedGold = completion->party[0].save.gold;
            const auto earnedExperience = completion->party[0].save.experience();
            LevelCompletion flow{-1, false};
            REQUIRE(flow.stage() == LevelCompletion::Stage::Results);
            REQUIRE(flow.movie().empty());
            REQUIRE(flow.visit() == ShopVisit::Level);
            AfterLevelScene results;
            REQUIRE(results.open(fixture.device, fixture.services, completion->party,
                                 completion->results, {1000, 100, 1000}, source, flow.visit()));
            REQUIRE(results.session().lanes()[0].phase == ShopPhase::Tally);
            CHECK(results.session().lanes()[0].tally.results().totals ==
                  completion->results[0].totals);
            results.render(fixture.device, Mat4{1}, 640, 448);
            CHECK_FALSE(fixture.device.draws.empty());
            ShopSession::Inputs press{};
            press[0].select = true;
            for (s32 count = 0;
                 count < 60 && results.session().lanes()[0].phase != ShopPhase::Shopping; ++count) {
                CHECK_FALSE(results.update(.5, press));
                CHECK_FALSE(results.update(.5, {}));
            }
            REQUIRE(results.session().lanes()[0].phase == ShopPhase::Shopping);
            CHECK(results.session().party()[0].save.gold == earnedGold);
            CHECK(results.session().party()[0].save.experience() == earnedExperience);
            const auto items = results.session().catalog().items();
            const auto key = std::ranges::find(items, 1, &ShopItem::type);
            REQUIRE(key != items.end());
            const s32 initialKeys = results.session().party()[0].save.progress().inventory.keys;
            REQUIRE(results.focusOnline(0, static_cast<usize>(key - items.begin())));
            // Row focus starts the native scrolling animation; buy edges are
            // deliberately discarded until it settles, rather than queued.
            ShopSession::Inputs accepted{};
            CHECK_FALSE(results.update(0, press, nullptr, &accepted));
            CHECK_FALSE(accepted[0].select);
            CHECK_FALSE(results.session().lanes()[0].transacted);
            CHECK(results.session().party()[0].save.gold == earnedGold);
            CHECK_FALSE(results.update(60, {}));
            CHECK_FALSE(results.update(.5, press, nullptr, &accepted));
            REQUIRE(accepted[0].select);
            REQUIRE(results.session().lanes()[0].transacted);
            REQUIRE(results.session().lanes()[0].feedback == ShopResult::Bought);
            CHECK(results.session().party()[0].save.gold == earnedGold - key->price);
            CHECK(results.session().party()[0].save.progress().inventory.keys ==
                  initialKeys + key->amount);
            // Closing the application while this shop is open must save its
            // current purchases, not the retained scene's abandoned-level copy.
            PlayerSelectScene characters;
            auto interrupted = legendsCompletionParty(completion->party, results, characters);
            REQUIRE(interrupted.size() == 1);
            CHECK(interrupted[0].save.gold == earnedGold - key->price);
            CHECK(interrupted[0].save.progress().inventory.keys == initialKeys + key->amount);
            CHECK(interrupted[0].slot == 3);
            SaveSlots slots;
            REQUIRE(slots.open(fixture.config.saveDirectory(), fixture.config.save.slots));
            REQUIRE(autoSaveParty(slots, interrupted).written == 1);
            const auto afterPurchase = slots.load(3);
            REQUIRE(afterPurchase);
            CHECK(afterPurchase->gold == interrupted[0].save.gold);
            CHECK(afterPurchase->progress().inventory.keys == initialKeys + key->amount);
            CHECK(afterPurchase->progress().legendsProgress(platform).hasCompleted(source));
            CHECK(scene.abandonedParty(scene.party())[0].save.gold != afterPurchase->gold);
            REQUIRE(results.focusOnline(0, 0));
            CHECK_FALSE(results.update(60, {}));
            bool done = false;
            for (s32 count = 0; count < 90 && !done; ++count) {
                done = results.update(.5, press);
                done = results.update(.5, {}) || done;
            }
            REQUIRE(done);
            completion->party = results.session().party();
            CHECK(completion->party[0].slot == 3);
            CHECK(completion->party[0].save.progress().legendsProgress(platform).hasCompleted(
                source));
            CHECK_FALSE(completion->party[0].resultsCheckpoint);
            const auto nextResult =
                LevelResults::between(0, completion->party[0].save, completion->party[0].save, 0);
            CHECK(nextResult.totals == std::array<s32, 3>{});
            results.close();
            CHECK(legendsCompletionParty(completion->party, results, characters)[0].save ==
                  completion->party[0].save);
            flow.advance();
            REQUIRE(flow.stage() == LevelCompletion::Stage::Characters);
            REQUIRE(characters.openAfterLevel(fixture.device, fixture.services, completion->party,
                                              &fixture.world));
            interrupted = legendsCompletionParty(before, results, characters);
            REQUIRE(interrupted.size() == 1);
            CHECK(interrupted[0].slot == 3);
            CHECK(interrupted[0].save.gold == afterPurchase->gold);
            CHECK(interrupted[0].save.progress().inventory.keys == initialKeys + key->amount);
            REQUIRE(autoSaveParty(slots, interrupted).written == 1);
            // Quit drops the last selector lane. It must not fall back to the
            // parked actor or recreate that lane in another slot on shutdown.
            PlayerSelectScene::Inputs up{};
            up[0].up = true;
            CHECK(characters.step(1, up) == SelectOutcome::Running);
            PlayerSelectScene::Inputs select{};
            select[0].select = true;
            REQUIRE(characters.step(1, select) == SelectOutcome::Cancelled);
            interrupted = legendsCompletionParty(before, results, characters);
            CHECK(interrupted.empty());
            CHECK(autoSaveParty(slots, interrupted).written == 0);
            const auto afterQuit = slots.load(3);
            REQUIRE(afterQuit);
            CHECK(afterQuit->gold == afterPurchase->gold);
            CHECK(afterQuit->progress().inventory.keys == initialKeys + key->amount);
            CHECK(afterQuit->progress().legendsProgress(platform).hasCompleted(source));
            CHECK_FALSE(slots.slot(0).occupied);
            CHECK_FALSE(slots.slot(1).occupied);
            CHECK_FALSE(slots.slot(2).occupied);
            characters.close();
            flow.advance();
            CHECK(flow.stage() == LevelCompletion::Stage::Done);
        }
    }
}

TEST_CASE("Native completion Done and recovery menus preserve the admitted departure",
          "[legends][legends-completion][legends-completion-done][assets][shop]") {
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        DYNAMIC_SECTION(legendsPlatformName(platform)) {
            CompletionFixture fixture{platform, false};
            fixture.scene.liveSave(0)->gold += 300;
            fixture.leave();
            auto completion = completedLegendsTravel(fixture.scene);
            REQUIRE(completion);
            fixture.scene.suspendForImportedLevel(true);
            const auto source = fixture.world.ref();
            const auto generation = fixture.scene.loadGeneration();
            const auto collisionCount = fixture.world.collision().triangleCount();
            REQUIRE(collisionCount > 0);
            const auto checkDeparture = [&] {
                // Gauntlet's Done dispatch requires the parked source to retain
                // its native owner and the completed departure. Replacing its
                // world with a menu backdrop used to reject every Done retry.
                REQUIRE(fixture.world.imported() == fixture.native.get());
                CHECK(fixture.world.ref() == source);
                CHECK(fixture.world.collision().triangleCount() == collisionCount);
                CHECK(fixture.scene.loadGeneration() == generation);
                CHECK(fixture.scene.leaving());
                CHECK(fixture.native->travelRequested());
                const auto admitted = completedLegendsTravel(fixture.scene);
                REQUIRE(admitted);
                CHECK(admitted->destination == completion->destination);
            };
            LevelCompletion flow{-1, false};
            AfterLevelScene shop;
            REQUIRE(shop.open(fixture.device, fixture.services, completion->party,
                              completion->results, {1000, 100, 1000}, source.name, flow.visit()));
            ShopSession::Inputs accept{};
            accept[0].select = true;
            for (s32 tick = 0; tick < 60 && shop.session().lanes()[0].phase != ShopPhase::Shopping;
                 ++tick) {
                CHECK_FALSE(shop.update(.5, accept));
                CHECK_FALSE(shop.update(.5, {}));
            }
            REQUIRE(shop.session().lanes()[0].phase == ShopPhase::Shopping);
            const auto catalog = shop.session().catalog().items();
            const auto key = std::ranges::find(catalog, 1, &ShopItem::type);
            REQUIRE(key != catalog.end());
            const auto beforePurchase = shop.session().party()[0].save;
            REQUIRE(shop.focusOnline(0, static_cast<usize>(key - catalog.begin())));
            CHECK_FALSE(shop.update(60, {}));
            CHECK_FALSE(shop.update(.5, accept));
            REQUIRE(shop.session().lanes()[0].feedback == ShopResult::Bought);
            const auto purchased = shop.session().party()[0].save;
            CHECK(purchased.gold == beforePurchase.gold - key->price);
            CHECK(purchased.progress().inventory.keys ==
                  beforePurchase.progress().inventory.keys + key->amount);
            REQUIRE(shop.focusOnline(0, 0));
            CHECK_FALSE(shop.update(60, {}));
            bool finished = false;
            for (s32 tick = 0; tick < 90 && !finished; ++tick) {
                finished = shop.update(.5, accept);
                finished = shop.update(.5, {}) || finished;
            }
            REQUIRE(finished);
            auto party = shop.session().party();
            shop.close();
            flow.advance();
            REQUIRE(flow.stage() == LevelCompletion::Stage::Characters);
            checkDeparture();

            PlayerSelectScene characters;
            // The first opening is updateCompletion's Characters dispatch;
            // subsequent openings are finishJourney's failed-destination
            // recovery. Both pass the same parked world and latest menu party.
            for (s32 attempt = 0; attempt < 3; ++attempt) {
                CAPTURE(attempt);
                REQUIRE(characters.openAfterLevel(fixture.device, fixture.services, party,
                                                  &fixture.world));
                checkDeparture();
                CHECK_FALSE(characters.towerVisible());
                REQUIRE(characters.lane(0).state() == SelectLane::State::SaveMenu);
                characters.render(fixture.device, Mat4{1}, 640, 448);
                PlayerSelectScene::Inputs done{};
                done[0].select = true; // Done is the native post-level default.
                auto outcome = characters.step(1, done);
                for (s32 tick = 0; tick < 300 && outcome == SelectOutcome::Running; ++tick) {
                    outcome = characters.step(1, {});
                }
                REQUIRE(outcome == SelectOutcome::Done);
                party = characters.party();
                REQUIRE(party.size() == 1);
                CHECK(party[0].player == 0);
                CHECK(party[0].slot == 3);
                CHECK(party[0].save == purchased);
                CHECK_FALSE(party[0].resultsCheckpoint);
                characters.close();
                flow.advance();
                CHECK(flow.stage() == LevelCompletion::Stage::Done);
                checkDeparture();
            }
        }
    }
}

TEST_CASE("Native tower departures do not open completion menus",
          "[legends][legends-completion][assets]") {
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        DYNAMIC_SECTION(legendsPlatformName(platform)) {
            CompletionFixture fixture{platform, true};
            fixture.leave();
            CHECK_FALSE(completedLegendsTravel(fixture.scene));
        }
    }
}

TEST_CASE("An unopened scene cannot manufacture a native completion",
          "[legends][legends-completion]") {
    const PlayScene scene{};
    CHECK_FALSE(completedLegendsTravel(scene));
}
} // namespace
