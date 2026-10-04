#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/codec/MoviePlayback.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/LevelCompletion.h"
#include "game/app/Scenario.h"
#include "game/players/Party.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/PlayerSelectScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Garm death reaches the wizard farewell and hands travel to the victory movie",
          "[level-completion][garm][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELH4/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto scenario =
        Scenario::load(test::dataDirectory().parent_path() / "tests/scenarios/level-h4-garm.json");
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto level = levels.byName("H4");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    context.levels = &levels;
    PlayScene play;
    REQUIRE(play.open(device, context, world, scenario.partyMembers(), scenario.tower));
    play.bosses().wake();
    for (s32 frame = 0; frame < 300; ++frame) {
        play.update(1.0 / 30, {});
    }
    REQUIRE(play.bossView());
    REQUIRE(play.bossView()->kind == 44);
    EnemyHit hit;
    hit.damage = 100000;
    hit.player = 0;
    REQUIRE(play.bosses().hurt(hit) > 0);
    REQUIRE_FALSE(play.bossView()->alive);
    CHECK_FALSE(play.victory().finished()); // lethal damage is not the end of the death sequence
    bool farewell = false;
    PlayOutcome outcome = PlayOutcome::Running;
    for (s32 frame = 0; frame < 3600 && outcome == PlayOutcome::Running; ++frame) {
        outcome = play.update(1.0 / 30, {});
        if (play.victory().caption()) {
            farewell |= play.victory().caption()->message == "GARM2_SPEECH";
        }
    }
    REQUIRE(farewell);
    REQUIRE(play.victory().finished());
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(play.destination().isTower());
    CHECK(play.party().front().save.progress().relics.hasShard(LevelRef::orderOf(8)));
    const LevelCompletion flow(world.level()->bossType, play.victory().finished());
    CHECK(flow.stage() == LevelCompletion::Stage::Movie);
    CHECK(flow.movie() == "victory");
    play.close();
}

TEST_CASE("only completed final bosses play ending movies before results and save",
          "[game][level-completion]") {
    for (s32 kind = -1; kind <= 44; ++kind) {
        for (const bool defeated : {false, true}) {
            CAPTURE(kind, defeated);
            LevelCompletion flow(kind, defeated);
            const bool movie = defeated && (kind == 43 || kind == 44);
            REQUIRE(flow.stage() ==
                    (movie ? LevelCompletion::Stage::Movie : LevelCompletion::Stage::Results));
            REQUIRE(flow.movie() == (movie ? (kind == 44 ? "victory" : "garm") : ""));
            REQUIRE(flow.visit() ==
                    (defeated && kind == 44 ? ShopVisit::Completion : ShopVisit::Level));
            if (movie) {
                flow.advance(); // end of media OR missing media: never loop/replay or skip save
            }
            REQUIRE(flow.stage() == LevelCompletion::Stage::Results);
            flow.advance();
            REQUIRE(flow.stage() == LevelCompletion::Stage::Characters);
            flow.advance();
            REQUIRE(flow.stage() == LevelCompletion::Stage::Done);
            flow.advance();
            REQUIRE(flow.stage() == LevelCompletion::Stage::Done);
        }
    }
}

TEST_CASE("both final boss movies decode their native video and audio to completion",
          "[level-completion][assets][movie]") {
    for (const s32 kind : {43, 44}) {
        const LevelCompletion flow(kind, true);
        const auto file = test::assetOrSkip("VQMOVIES/" + std::string(flow.movie()) + ".avi");
        MoviePlayback movie;
        REQUIRE(movie.open(file));
        REQUIRE(movie.info().frameCount > 0);
        REQUIRE(movie.info().framesPerSecond > 0);
        const auto duration = movie.info().frameCount / movie.info().framesPerSecond;
        std::vector<f32> samples;
        usize audible = 0;
        bool playing = true;
        for (s32 frame = 0; playing && frame < static_cast<s32>((duration + 2) * 30); ++frame) {
            playing = movie.update(1.0 / 30);
            samples.clear();
            movie.takeAudio(samples);
            audible += std::ranges::count_if(samples, [](f32 value) { return value != 0; });
        }
        REQUIRE_FALSE(playing);
        CHECK(movie.decodedFrames() == movie.info().frameCount);
        CHECK(movie.info().audioReady);
        CHECK(audible > 0);
    }
}

TEST_CASE("Garm results retain victory rewards and offer saving without a shopping phase",
          "[level-completion][assets][shop][select]") {
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/after-garm-victory.json");
    REQUIRE(scenario.ending);
    REQUIRE(scenario.afterLevel);
    auto party = scenario.partyMembers();
    REQUIRE_FALSE(party.front().slot);
    recordLevelBeaten(party, 8, 3, 0, 0);
    const auto saved = party.front().save.toJson();
    LevelCompletion flow(44, true);
    REQUIRE(flow.movie() == "victory");
    flow.advance();
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("garm-ending-save").string();
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    AfterLevelScene results;
    REQUIRE(results.open(device, context, party, scenario.results, {1000, 100, 1000}, "H4",
                         flow.visit()));
    bool finalStats = false;
    bool done = false;
    for (s32 press = 0; press < 60 && !done; ++press) {
        const auto phase = results.session().lanes().front().phase;
        CHECK(phase != ShopPhase::Shopping);
        CHECK(phase != ShopPhase::Inventory);
        finalStats |= phase == ShopPhase::FinalStats;
        ShopSession::Inputs inputs{};
        inputs[0].select = true;
        done = results.update(1.0 / 60, inputs);
        done = results.update(0.5, {}) || done;
    }
    REQUIRE(done);
    REQUIRE(finalStats);
    party = results.session().party();
    results.close();
    CHECK(party.front().save.toJson() == saved); // tally never grants the rewards twice
    flow.advance();
    REQUIRE(flow.stage() == LevelCompletion::Stage::Characters);
    PlayerSelectScene select;
    REQUIRE(select.openAfterLevel(device, context, party));
    REQUIRE(select.lane(0).state() == SelectLane::State::SaveMenu);
    select.step(60, {});
    PlayerSelectScene::Inputs inputs{};
    inputs[0].down = true;
    select.step(1, inputs); // Done -> Save
    inputs = {};
    select.step(1, inputs);
    inputs[0].select = true;
    select.step(1, inputs);
    REQUIRE(select.lane(0).state() == SelectLane::State::SavePick);
    select.step(1, {});
    select.step(1, inputs);
    REQUIRE(select.lane(0).state() == SelectLane::State::Saving);
    for (s32 tick = 0; tick < 180; ++tick) {
        select.step(1, {});
    }
    CharacterSave stored;
    REQUIRE(select.saves().load(0, stored));
    CHECK(stored.toJson() == saved);
    select.close();
}
} // namespace
