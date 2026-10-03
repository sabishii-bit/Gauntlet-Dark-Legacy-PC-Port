#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerSelectScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

const PartyMember& memberOf(const std::vector<PartyMember>& party, s32 player) {
    const auto found = std::ranges::find(party, player, &PartyMember::player);
    REQUIRE(found != party.end());
    return *found;
}

TEST_CASE("four controllers retain distinct characters through death shop save and tower return",
          "[game][multiplayer-journey][multiplayer][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("SHPDATA/SHOP.WAD");
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("multiplayer-journey").string();
    config.save.slots = 4;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    context.levels = &levels;
    PlayerSelectScene select;
    REQUIRE(select.open(device, context, 2));
    PlayerSelectScene::Inputs join{};
    for (auto& input : join) {
        input.start = true;
    }
    select.join(join);
    PlayerSelectScene::Inputs acceptAll{};
    for (auto& input : acceptAll) {
        input.select = true;
    }
    select.step(60, {});
    select.step(1, acceptAll); // New
    for (s32 player = 0; player < 4; ++player) {
        REQUIRE(select.lane(player).state() == SelectLane::State::NameEntry);
    }
    select.step(1, acceptAll); // Finish each name using its own lane.
    select.step(NameEntry::kFlashTicks + 1, {});
    PlayerSelectScene::Inputs colors{};
    for (s32 player = 0; player < 4; ++player) {
        REQUIRE(select.lane(player).state() == SelectLane::State::ClassPick);
        for (s32 step = 0; step < player; ++step) {
            colors = {};
            colors[static_cast<usize>(player)].up = true;
            select.step(1, colors);
        }
    }
    select.step(1, acceptAll);
    SelectOutcome selected = SelectOutcome::Running;
    for (s32 frame = 0; frame < 240 && selected == SelectOutcome::Running; ++frame) {
        selected = select.step(1, {});
    }
    REQUIRE(selected == SelectOutcome::Done);
    auto initial = select.party();
    REQUIRE(initial.size() == 4);
    for (s32 player = 0; player < 4; ++player) {
        CHECK(memberOf(initial, player).save.color == player);
    }
    select.close();

    // The party vector deliberately differs from controller ordering.
    std::ranges::reverse(initial);
    CHECK_FALSE(TowerAccess{initial}.worldOpen(7));
    for (auto& member : initial) {
        member.save.gold = 100 + member.player * 10;
        member.save.progress().health = 1000;
        member.helpHeard = {10 + member.player};
        // Province entry requires its fifteen crystals, independently of beaten levels.
        // One survivor holds them; the others rely on the joined party's shared access.
        if (member.player == 2) {
            constexpr auto kProvinceGate = static_cast<usize>(TowerAccess::kWorldGates[7]);
            member.save.progress().crystals[kProvinceGate] =
                LevelTriggers::crystalsNeeded(static_cast<s32>(kProvinceGate));
        }
    }
    REQUIRE(TowerAccess{initial}.worldOpen(7));
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-36.2f, 26.5f, -125.3f};
    PlayScene play;
    REQUIRE(play.open(device, context, world, initial, options));
    for (s32 frame = 0; frame < 240; ++frame) {
        REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE(world.placeItem(device, "RUNEC2", play.actor(3)->position()));
    play.update(1.0 / 60, {});
    for (s32 player = 0; player < 4; ++player) {
        REQUIRE(play.actor(player)->save().progress().relics.hasRune(7));
    }
    const auto fall = [&](s32 player, bool quit) {
        play.hurtPlayer(player, 10000, HurtKind::Burn);
        for (s32 frame = 0; frame < 600 && !play.status(player).towerPrompt; ++frame) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.status(player).towerPrompt);
        PlayScene::Inputs answer{};
        answer[static_cast<usize>(player)].menu.select = !quit;
        answer[static_cast<usize>(player)].menu.back = quit;
        REQUIRE(play.update(1.0 / 60, answer) == PlayOutcome::Running);
        CHECK_FALSE(play.status(player).towerPrompt);
    };
    fall(0, false);
    fall(1, true);
    const s32 experience = play.actor(3)->save().experience();
    play.awardExperience(3, 25);
    CHECK(play.actor(3)->save().experience() > experience);
    CHECK(play.actor(2)->save().experience() == memberOf(initial, 2).save.experience());
    auto afterPlay = play.party();
    REQUIRE(afterPlay.size() == 3);
    CHECK(std::ranges::find(afterPlay, 1, &PartyMember::player) == afterPlay.end());
    CHECK(memberOf(afterPlay, 0).fallen);
    CHECK(memberOf(afterPlay, 0).save.gold == memberOf(initial, 0).save.gold);
    CHECK_FALSE(memberOf(afterPlay, 0).save.progress().relics.hasRune(7));
    CHECK(memberOf(afterPlay, 2).save.progress().relics.hasRune(7));
    const auto results = play.levelResults();
    REQUIRE(results.size() == 2);
    CHECK(std::ranges::find(results, 0, &LevelResults::player) == results.end());
    CHECK(std::ranges::find(results, 1, &LevelResults::player) == results.end());
    play.close();

    ExitPortals exits;
    REQUIRE(exits.bind(device, world.layout(), world.items(), levels, &world.collision(),
                       &world.realmItems()));
    usize exitIndex = 0;
    while (exitIndex < exits.size() && exits.portal(exitIndex).secret) {
        ++exitIndex;
    }
    REQUIRE(exitIndex < exits.size());
    const Vec3 exitPosition = exits.portal(exitIndex).position;
    std::array visitors{PortalVisitor{exitPosition, 0.75f, 0, true},
                        PortalVisitor{exitPosition + Vec3{50, 0, 0}, 0.75f, 1, true}};
    for (s32 frame = 0; frame < 180; ++frame) {
        CHECK_FALSE(exits.update(2, 1.0f / 30, visitors).has_value());
    }
    visitors[1].position = exitPosition;
    std::optional<usize> ready;
    for (s32 frame = 0; frame < 600 && !ready.has_value(); ++frame) {
        ready = exits.update(2, 1.0f / 30, visitors);
    }
    REQUIRE(ready == exitIndex);
    REQUIRE(exits.portal(*ready).destination.has_value());
    exits.clear();

    REQUIRE(recordLevelBeaten(afterPlay, 7, 0, 0, 0) == 2);
    AfterLevelScene shop;
    REQUIRE(shop.open(device, context, afterPlay, results, {1000, 100, 1000}, "G1"));
    bool finished = false;
    for (s32 press = 0; press < 50 && !finished; ++press) {
        ShopSession::Inputs inputs{};
        // Controller 0 is fallen and 1 has left: neither drives vector lane zero.
        inputs[2].select = true;
        inputs[3].select = true;
        finished = shop.update(1.0 / 60, inputs);
        finished = shop.update(0.5, {}) || finished;
    }
    REQUIRE(finished);
    const auto afterShop = shop.session().party();
    REQUIRE(afterShop.size() == 3);
    for (const auto& member : afterShop) {
        CHECK(member.save.toJson() == memberOf(afterPlay, member.player).save.toJson());
    }
    shop.close();

    REQUIRE(select.openAfterLevel(device, context, afterShop));
    REQUIRE(select.lane(0).lockedIn());
    REQUIRE_FALSE(select.lane(1).active());
    select.step(60, {});
    const auto press = [&](s32 player, const MenuInput& button) {
        PlayerSelectScene::Inputs inputs{};
        inputs[static_cast<usize>(player)] = button;
        select.step(1, inputs);
        select.step(1, {});
    };
    MenuInput down;
    down.down = true;
    MenuInput accept;
    accept.select = true;
    for (const s32 player : {2, 3}) {
        press(player, down); // Done -> Save.
        press(player, accept);
        REQUIRE(select.lane(player).state() == SelectLane::State::SavePick);
        if (player == 3) {
            press(player, accept); // Slot zero belongs to the other participant.
            REQUIRE(select.lane(player).state() == SelectLane::State::SavePick);
            press(player, down);
        }
        press(player, accept);
        REQUIRE(select.lane(player).state() == SelectLane::State::Saving);
        for (s32 tick = 0; tick < 240; ++tick) {
            select.step(1, {});
        }
        REQUIRE(select.lane(player).state() == SelectLane::State::SaveMenu);
        REQUIRE(select.lane(player).slotInUse().has_value());
        CharacterSave disk;
        REQUIRE(select.saves().load(*select.lane(player).slotInUse(), disk));
        CHECK(disk.toJson() == memberOf(afterShop, player).save.toJson());
    }
    CHECK(select.lane(2).slotInUse() != select.lane(3).slotInUse());
    MenuInput up;
    up.up = true;
    press(3, up); // Done -> Quit -> Load, without confirming Quit.
    press(3, up);
    press(3, accept);
    REQUIRE(select.lane(3).state() == SelectLane::State::LoadPick);
    press(3, accept); // Its own saved slot, not the other survivor's slot zero.
    REQUIRE(select.lane(3).state() == SelectLane::State::Loading);
    for (s32 tick = 0; tick < 240; ++tick) {
        select.step(1, {});
    }
    REQUIRE(select.lane(3).state() == SelectLane::State::ClassPick);
    press(3, accept);
    REQUIRE(select.lane(3).state() == SelectLane::State::SaveMenu);
    CHECK(memberOf(select.party(), 3).helpHeard.empty());
    CHECK(memberOf(select.party(), 2).helpHeard == memberOf(afterShop, 2).helpHeard);
    press(2, accept);
    CHECK(select.lane(2).lockedIn());
    CHECK(select.step(60, {}) == SelectOutcome::Running);
    press(3, accept);
    selected = SelectOutcome::Running;
    for (s32 frame = 0; frame < 240 && selected == SelectOutcome::Running; ++frame) {
        selected = select.step(1, {});
    }
    REQUIRE(selected == SelectOutcome::Done);
    const auto returning = select.party();
    REQUIRE(returning.size() == 3);
    for (const auto& member : returning) {
        CHECK(member.helpHeard == (member.player == 3
                                       ? std::vector<s32>{}
                                       : memberOf(afterShop, member.player).helpHeard));
        CHECK(member.save.toJson() == memberOf(afterShop, member.player).save.toJson());
    }
    select.close();

    REQUIRE(world.load(device, root));
    options.position.reset();
    options.arrivalWorld = 7;
    options.arriving = true;
    REQUIRE(play.open(device, context, world, returning, options));
    REQUIRE(play.actorCount() == 3);
    CHECK(play.actor(1) == nullptr);
    CHECK_FALSE(play.fallen(0));
    CHECK(play.actor(0)->save().gold == memberOf(initial, 0).save.gold);
    CHECK(play.startCamera().active());
    const TowerAccess access{returning};
    REQUIRE(access.worldOpen(7));
    const auto* marker = world.arrivalPoint(7, &access);
    REQUIRE(marker != nullptr);
    REQUIRE(marker != world.startPoint(0));
    for (const s32 player : {0, 2, 3}) {
        REQUIRE(play.actor(player) != nullptr);
        CHECK(glm::distance(play.actor(player)->position(), marker->position) < 7);
    }
    play.close();
}
} // namespace
