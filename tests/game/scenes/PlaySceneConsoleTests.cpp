#include <array>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/console/NoclipMotion.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/Critters.h"
#include "game/enemies/Generators.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/ExitPortals.h"
#include "game/level/SpawnedObjects.h"
#include "game/players/ItemPickup.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/ConsoleStage.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f64 kStep = 1.0 / 60.0;
constexpr s32 kSeats = 2;
constexpr u8 kNobody = 3;                                        ///< a seat nobody holds
constexpr s32 kNearlyFull = std::numeric_limits<s32>::max() - 5; ///< a counter with no room left
constexpr f32 kChargeNearlyFull = 9999999.0f;   ///< a powerup charge just under ten million
constexpr f32 kStrengthNearlyFull = 9999999.0f; ///< seconds of a powerup just under ten million
constexpr f32 kForGood = -1.0f;                 ///< a powerup's strength that never runs out
constexpr s32 kDeathTicks = 600; ///< long enough for a death to play out to its question
constexpr s32 kExitTicks = 1200; ///< long enough for a portal to take a party standing on it
constexpr usize kLongLine = 170; ///< a console line past the wire's ConsoleAction::kMaxLine
constexpr std::string_view kOnItsWay = "The party is already on its way.";
constexpr std::string_view kNoScene = "No active scene for that console command.";
constexpr std::string_view kLegendsOffline = "Legends maps are offline only.";
constexpr std::string_view kNotInParty = "That player is no longer in the party.";

// What get, goto and spawn answer words they cannot read, word for word.
constexpr std::string_view kGetUsage =
    "get <item> [amount]   keys, potions, gold, health or a powerup\n"
    "get exp <points>      experience to add\n"
    "get level <level>     the level to be, 1 to 99\n"
    "get info [$SELECTED]  what the thing clicked in the view is\n"
    "list items names the items.";
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]            travels to a level, standing at a point when given\n"
    "goto x y z                    moves the party to that point here\n"
    "goto <legends map> [preview]  walks an installed Legends map, or flies through it\n"
    "goto n64|dreamcast <map>      the same, naming the map's platform\n"
    "goto back                     returns from a Legends map to the level left for it\n"
    "list maps names the levels and the Legends maps installed.";
constexpr std::string_view kSpawnUsage =
    "spawn <enemy> [tier] [count] [map]            so many of the swarm's kind, at a tier\n"
    "spawn generator <enemy>|special [tier] [map]  a generator breeding that kind, or the "
    "temple's or underworld's mixed brood\n"
    "spawn golem|general|gargoyle [form] [map]     a great one; a gargoyle's form is eagl, lion "
    "or serp\n"
    "spawn item <name>                             a pickup of this level, by its record\n"
    "spawn object <name> [map]                     a copy of a level's object\n"
    "A map (E2) brings the kind's voices, a great one's costume or the object from that level; "
    "list enemies, critters, items and objects name what there is.";

/** A level open for two seats (the tower for "L1"), stepped a tick at a time the way every
 * peer steps it, with the console lines of that tick. */
struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    PlayScene scene;
    GameContext context;
    u64 tick = 0;

    explicit Fixture(std::string_view levelName,
                     const std::array<ConsoleCheats, PlayOptions::kSeats>& cheats = {}) {
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", levelName));
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level =
            levelName == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(levelName);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        context.stopTimeTotal = &stopTimeTotal;
        std::vector<PartyMember> party;
        for (s32 seat = 0; seat < kSeats; ++seat) {
            CharacterSave save;
            save.name = "TESTER";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            save.progress().inventory.addKeys(9);
            save.progress().inventory.addPotions(seat + 1, 9);
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        PlayOptions options;
        options.welcome = false;
        options.consoleCheats = cheats;
        REQUIRE(scene.open(device, context, world, party, options));
        REQUIRE(scene.nativeAgreementDigest());
    }

    /** One tick of the shared game, with `actions` run in it. */
    PlayScene::RecordedStep sharedStep(std::span<const ConsoleAction> actions = {},
                                       const PlayScene::Inputs& inputs = {}) {
        return scene.recordStep(1, ++tick, kStep, inputs, actions);
    }

    /** One tick of offline play, with `actions` run in it. */
    PlayScene::LocalStep offlineStep(std::span<const ConsoleAction> actions = {},
                                     const PlayScene::Inputs& inputs = {}) {
        return scene.step(kStep, inputs, actions);
    }

    /** One tick carrying `line` for `seat` alone. */
    PlayScene::RecordedStep sayStep(std::string line, u8 seat = 0) {
        const std::array actions{ConsoleAction{seat, std::move(line)}};
        auto result = sharedStep(actions);
        REQUIRE(result.consoleReplies.size() == 1);
        CHECK(result.consoleReplies.front().seat == seat);
        return result;
    }

    /** What the scene answers `line` said for `seat`, in a tick of its own. */
    std::string say(std::string line, u8 seat = 0) {
        return sayStep(std::move(line), seat).consoleReplies.front().text;
    }

    CharacterSave& save(s32 seat) {
        CharacterSave* live = scene.liveSave(seat);
        REQUIRE(live != nullptr);
        return *live;
    }
};

/** Every verb's line, the same verbs with words they cannot read, and a seat nobody holds. */
std::vector<ConsoleAction> awayLines() {
    return {{0, "god"},        {0, "notarget"},       {0, "noclip"},
            {0, "get gold 1"}, {0, "spawn gru"},      {0, "suicide"},
            {0, "goto 1 2 3"}, {0, "unlock_tower"},   {0, "god x"},
            {0, "get"},        {0, "spawn"},          {0, "suicide x"},
            {0, "goto"},       {0, "unlock_tower x"}, {kNobody, "god"}};
}

/** A party on its way refuses every line once its seat is found, before the verb reads its
 * words. */
void checkRefusedAway(std::span<const ConsoleAction> lines,
                      std::span<const PlayScene::ConsoleReply> replies) {
    REQUIRE(replies.size() == lines.size());
    for (usize i = 0; i < lines.size(); ++i) {
        CAPTURE(lines[i].seat, lines[i].line);
        CHECK(replies[i].seat == lines[i].seat);
        CHECK(replies[i].text == (lines[i].seat == kNobody ? kNotInParty : kOnItsWay));
    }
}
} // namespace

TEST_CASE("every shared verb answers with its words today", "[native-console][assets]") {
    test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2");
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));
    const auto reopen = [&] { REQUIRE(scene.restoreLocal(opened)); };

    // The toggles flip the seat's own cheat; words after one change nothing.
    CHECK(fixture->say("god") == "god on");
    CHECK(scene.cheats(0).god);
    CHECK_FALSE(scene.cheats(1).god);
    CHECK(fixture->say("god x") == "This command toggles without arguments.");
    CHECK(scene.cheats(0).god);
    CHECK(fixture->say("god") == "god off");
    CHECK_FALSE(scene.cheats(0).god);
    CHECK(fixture->say("notarget", 1) == "notarget on");
    CHECK(scene.cheats(1).noTarget);
    CHECK(fixture->say("notarget x", 1) == "This command toggles without arguments.");
    CHECK(fixture->say("notarget", 1) == "notarget off");
    CHECK_FALSE(scene.cheats(1).noTarget);
    CHECK(fixture->say("noclip", 1) == "noclip on");
    CHECK(scene.cheats(1).noclip);
    CHECK(scene.runtime(1)->ghost);
    CHECK(fixture->say("noclip x", 1) == "This command toggles without arguments.");
    CHECK(fixture->say("noclip", 1) == "noclip off");
    CHECK_FALSE(scene.cheats(1).noclip);
    CHECK_FALSE(scene.runtime(1)->ghost);

    // Gifts, and their refusals: words get cannot read, the cap and counters left no room.
    const s32 gold = fixture->save(0).gold;
    const s32 otherGold = fixture->save(1).gold;
    CHECK(fixture->say("get gold 37") == "Got 37 gold");
    CHECK(fixture->save(0).gold == gold + 37);
    CHECK(fixture->save(1).gold == otherGold);
    const s32 experience = fixture->save(1).experience();
    CHECK(fixture->say("get exp 10", 1) == "10 experience: level 1");
    CHECK(fixture->save(1).experience() > experience);
    CHECK(fixture->say("get level 5", 1) == "Level 5");
    CHECK(fixture->save(1).experience() == levelExperience(5));
    CHECK(fixture->say("get") == kGetUsage);
    CHECK(fixture->say("get exp 0") == "get exp <points>: how much to add");
    CHECK(fixture->say("get level 100") == "get level <level>: 1 to 99");
    CHECK(fixture->say("get banana") == "No item named banana; list items names them");
    CHECK(fixture->say("get potion purple") ==
          "No potion kind named purple; fire, electric, lightning or acid");
    CHECK(fixture->say("get levitate 0") ==
          "get levitate [seconds]: how long for; under nought for good");
    CHECK(fixture->say("get gold 1000001") ==
          "A console grant must be finite and no larger than 1000000.");
    CHECK(fixture->say("get levitate nan") ==
          "A console grant must be finite and no larger than 1000000.");
    CHECK(fixture->say("get levitate 1000001") ==
          "A console grant must be finite and no larger than 1000000.");
    CHECK(fixture->save(0).gold == gold + 37);
    // get info stays on the asking machine: the wire never carries it.
    CHECK(fixture->say("get info") == kNoScene);
    fixture->save(0).progress().experience = kNearlyFull;
    CHECK(fixture->say("get exp 1000000") ==
          "That experience grant would exceed the character's counter.");
    CHECK(fixture->save(0).experience() == kNearlyFull);
    reopen();
    fixture->save(0).gold = kNearlyFull;
    CHECK(fixture->say("get gold 37") == "That item grant would exceed the character's counter.");
    CHECK(fixture->save(0).gold == kNearlyFull);
    reopen();
    fixture->save(0).progress().health = kNearlyFull;
    CHECK(fixture->say("get health 37") == "That item grant would exceed the character's counter.");
    CHECK(fixture->save(0).progress().health == kNearlyFull);
    reopen();
    fixture->save(0).progress().inventory.powerups.front() = {.strength = kForGood,
                                                              .kind = powerup::kWeapon,
                                                              .charge = kChargeNearlyFull,
                                                              .flags = damage::kSuperShot};
    CHECK(fixture->say("get supershot 10") ==
          "That item grant would exceed the powerup's counter.");
    CHECK(fixture->save(0).progress().inventory.powerups.front().charge == kChargeNearlyFull);
    reopen();
    fixture->save(0).progress().lifetime.goldFound = kNearlyFull;
    CHECK(fixture->say("get gold 37") == "That item grant would exceed the character's counter.");
    CHECK(fixture->save(0).gold == gold);
    reopen();
    fixture->save(0).progress().inventory.powerups.front() = {
        .strength = kStrengthNearlyFull, .kind = powerup::kSpecial, .flags = powerup::kLevitation};
    CHECK(fixture->say("get levitate 10") == "That item grant would exceed the powerup's counter.");
    CHECK(fixture->save(0).progress().inventory.powerups.front().strength == kStrengthNearlyFull);
    reopen();
    CHECK(fixture->save(0).gold == gold);

    // suicide fells everyone standing, then nobody.
    CHECK(fixture->say("suicide x") == "suicide takes no arguments.");
    CHECK_FALSE(scene.fallen(0));
    CHECK(fixture->say("suicide") == "2 fallen");
    CHECK(scene.fallen(0));
    CHECK(scene.fallen(1));
    CHECK(fixture->say("suicide", 1) == "0 fallen");
    CHECK(fixture->say("goto 1 2 3") == "Nobody is standing.");
    reopen();

    // goto: a point, a level, a level and a point, the tower, a challenge.
    CHECK(fixture->say("goto 1 2 3") == "Moved the party.");
    CHECK(scene.actor(0)->position().x == Catch::Approx(1.0f));
    CHECK(scene.actor(0)->position().z == Catch::Approx(3.0f));
    CHECK(scene.actor(1)->position().x == Catch::Approx(3.0f));
    CHECK(scene.actor(1)->position().z == Catch::Approx(3.0f));
    CHECK(fixture->say("goto nan 0 0") == "Coordinates must be finite and within +/-1000000.");
    CHECK(fixture->say("goto 0 1e7 0") == "Coordinates must be finite and within +/-1000000.");
    CHECK(fixture->say("goto Z9") == "No map named Z9");
    // A word that does not look like a level's name is a Legends map's.
    CHECK(fixture->say("goto ZZ9") == kLegendsOffline);
    CHECK(fixture->say("goto lab") == kLegendsOffline);
    CHECK(fixture->say("goto back") == kLegendsOffline);
    CHECK(fixture->say("goto n64 castle") == kLegendsOffline);
    CHECK(fixture->say("goto") == kGotoUsage);
    CHECK(fixture->say("goto 1 2") == kGotoUsage);
    CHECK_FALSE(scene.consoleTravelRequest());
    const auto g1 = fixture->catalog.byName("G1");
    REQUIRE(g1);
    const auto toG1 = fixture->sayStep("goto G1");
    CHECK(toG1.consoleReplies.front().text == "Going to G1 (Fields)");
    CHECK(toG1.outcome == PlayOutcome::Travel);
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{*g1, std::nullopt, std::nullopt});
    CHECK(scene.destination() == *g1);
    CHECK_FALSE(scene.secretTravel());
    reopen();
    const auto toPoint = fixture->sayStep("goto g1 4 5 6");
    CHECK(toPoint.consoleReplies.front().text == "Going to G1 (Fields)");
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{*g1, Vec3{4, 5, 6}, std::nullopt});
    reopen();
    const auto toTower = fixture->sayStep("goto tower");
    CHECK(toTower.consoleReplies.front().text == "Going to L1 (Tower)");
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{LevelRef::tower(), std::nullopt, std::nullopt});
    CHECK_FALSE(scene.secretTravel());
    reopen();
    const auto s1 = fixture->catalog.byName("S1");
    REQUIRE(s1);
    const Vec3 asked = scene.actor(1)->position();
    const Vec3 beside = scene.actor(0)->position();
    // The asker stands apart from the first seat, so the way back shows whose spot it is.
    REQUIRE((asked.x != beside.x || asked.z != beside.z));
    const auto toS1 = fixture->sayStep("goto S1", 1);
    CHECK(toS1.consoleReplies.front().text == "Going to S1 (Lab)");
    CHECK(scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{*s1, std::nullopt, std::nullopt});
    CHECK(scene.secretTravel());
    // The way back is where the one who asked stood.
    scene.resumeFromChallenge(scene.party());
    CHECK(scene.actor(0)->position().x == Catch::Approx(asked.x));
    CHECK(scene.actor(0)->position().z == Catch::Approx(asked.z));
    reopen();

    // unlock_tower in a level opens every save still in and goes nowhere.
    CHECK(fixture->say("unlock_tower x") == "unlock_tower takes no arguments.");
    CHECK(fixture->save(0).progress().relics.runeCount() == 0);
    const auto unlocked = fixture->sayStep("unlock_tower");
    CHECK(unlocked.consoleReplies.front().text == "The tower stands open to the party.");
    CHECK(unlocked.outcome == PlayOutcome::Running);
    CHECK_FALSE(scene.consoleTravelRequest());
    for (s32 seat = 0; seat < kSeats; ++seat) {
        CHECK(fixture->save(seat).progress().relics.runeCount() == Relics::kRuneCount);
    }
    reopen();

    // A seat that left the game is passed over, though its own line still reaches the body.
    REQUIRE(scene.kill(1));
    for (s32 i = 0; i < kDeathTicks && !scene.status(1).towerPrompt; ++i) {
        fixture->sharedStep();
    }
    REQUIRE(scene.status(1).towerPrompt);
    PlayScene::Inputs quit{};
    quit[1].menu.back = true;
    fixture->sharedStep({}, quit);
    REQUIRE(scene.runtime(1)->departed);
    CHECK(fixture->say("unlock_tower") == "The tower stands open to the party.");
    CHECK(fixture->save(0).progress().relics.runeCount() == Relics::kRuneCount);
    CHECK(fixture->save(1).progress().relics.runeCount() == 0);
    CHECK(fixture->say("god", 1) == "god on");
    reopen();

    // A seat nobody holds, and lines the wire would not carry.
    CHECK(fixture->say("god", kNobody) == kNotInParty);
    CHECK_FALSE(scene.cheats(kNobody).god);
    CHECK(fixture->say("god", 4) == kNoScene);
    CHECK(fixture->say("god" + std::string(ConsoleAction::kMaxLine - 2, ' ')) == kNoScene);
    CHECK_FALSE(scene.cheats(0).god);

    // spawn: every kind, the words it cannot read and the ones a level refuses.
    CHECK(fixture->say("spawn") == kSpawnUsage);
    CHECK(fixture->say("spawn grunt 1 ZZ9") == kSpawnUsage);
    CHECK(fixture->say("spawn gru 1 ZZ9") == "No map named ZZ9");
    CHECK(fixture->say("spawn gru 4") == "A tier is 1 to 3");
    CHECK(fixture->say("spawn gru 1 21") == "A count is 1 to 20");
    CHECK(fixture->say("spawn enemy zz") == "No enemy kind named ZZ; list enemies names them");
    CHECK(fixture->say("spawn critter dragon") == "A critter is a golem, a general or a gargoyle");
    CHECK(fixture->say("spawn gargoyle eagl x y") ==
          "A gargoyle is an eagl, a lion or a serp, then a map at most");
    // What a map lends the swarm's voices shows in the roster the level's sounds are found by.
    const auto roster = [&scene] {
        const LevelInfo* now = scene.opponents().levelNow();
        REQUIRE(now != nullptr);
        return now->enemies.size();
    };
    const usize enemies = scene.enemies().count();
    const usize own = roster();
    CHECK(fixture->say("spawn gru 2 3") == "Spawned 3 GRU at tier 2");
    CHECK(scene.enemies().count() == enemies + 3);
    CHECK(roster() == own);
    CHECK(fixture->say("spawn enemy gru 2 2 G1") == "Spawned 2 GRU at tier 2");
    CHECK(scene.enemies().count() == enemies + 5);
    const std::vector<ConsoleAction> swarm(5, ConsoleAction{0, "spawn gru 1 20"});
    const auto crowded = fixture->sharedStep(swarm);
    REQUIRE(crowded.consoleReplies.size() == swarm.size());
    CHECK(crowded.consoleReplies.front().text.starts_with("Spawned "));
    CHECK(crowded.consoleReplies.back().text == "Spawned 0 GRU at tier 1");
    CHECK(scene.enemies().count() == static_cast<usize>(scene.enemies().most()));
    // A map whose spawn stands nothing lends nothing; one whose spawn stands lends its kinds.
    const usize crowdedRoster = roster();
    CHECK(fixture->say("spawn gru 1 1 E1") == "Spawned 0 GRU at tier 1");
    CHECK(roster() == crowdedRoster);
    reopen();
    const usize openedRoster = roster();
    CHECK(fixture->say("spawn gru 1 1 E1") == "Spawned 1 GRU at tier 1");
    CHECK(roster() > openedRoster);
    reopen();
    // Each realm letter dresses its golems from a stock of its own.
    const auto golemCostumes = [&scene] {
        std::set<const ItemArchive*> costumes;
        for (s32 id = 0; id < Critters::kMost; ++id) {
            const ItemArchive* costume = scene.critters().archiveOf(id);
            if (costume != nullptr && scene.critters().kindOf(id) == CombatantKind::Golem) {
                costumes.insert(costume);
            }
        }
        return costumes.size();
    };
    const usize critters = scene.critters().count();
    CHECK(fixture->say("spawn golem") == "Spawned a golem");
    CHECK(golemCostumes() == 1);
    CHECK(fixture->say("spawn general") == "Spawned a general");
    CHECK(fixture->say("spawn gargoyle lion") == "Spawned a gargoyle");
    CHECK(fixture->say("spawn critter golem B1") == "Spawned a golem");
    CHECK(golemCostumes() == 2);
    CHECK(scene.critters().count() == critters + 4);
    const std::vector<ConsoleAction> great(Critters::kMost, ConsoleAction{0, "spawn golem"});
    const auto full = fixture->sharedStep(great);
    REQUIRE(full.consoleReplies.size() == great.size());
    CHECK(full.consoleReplies.back().text == "No room or artwork for golem");
    reopen();
    const usize generators = scene.generators().count();
    CHECK(fixture->say("spawn generator rat 2") == "Stood a tier 2 RAT generator");
    CHECK(scene.generators().count() == generators + 1);
    CHECK(fixture->say("spawn generator special") ==
          "A mixed brood needs a Temple (E) or Underworld (F) map.");
    const usize before = roster();
    CHECK(fixture->say("spawn generator special E1") == "Stood a tier 1 SPECIAL generator");
    CHECK(scene.generators().count() == generators + 2);
    CHECK(scene.generators().kindOf(static_cast<s32>(scene.generators().count()) - 1) ==
          Generators::kTempleBrood);
    CHECK(roster() > before);
    CHECK(fixture->say("spawn generator zz") ==
          "No enemy kind named ZZ for a generator; list enemies names them");
    reopen();
    const std::string item = fixture->world.placedItems().item(0).name;
    const std::string object = fixture->world.layout().objects().front().name;
    CHECK(item == "GAPPLE");
    CHECK(object == "G2POLYSURF#000");
    const usize placed = fixture->world.placedItems().size();
    CHECK(fixture->say("spawn item " + item) == "Dropped GAPPLE");
    CHECK(fixture->world.placedItems().size() == placed + 1);
    CHECK(fixture->say("spawn item " + item + " G1") ==
          "Pickups come from the current level; omit the map.");
    CHECK(fixture->say("spawn item NOPE") == "No pickup named NOPE");
    CHECK(fixture->say("spawn " + item) == "Dropped GAPPLE");
    CHECK(fixture->world.placedItems().size() == placed + 2);
    CHECK(fixture->say("spawn " + object) == "Stood G2POLYSURF#000 of G2");
    CHECK(scene.spawnedObjectCount() == 1);
    CHECK(fixture->say("spawn object " + object) == "Stood G2POLYSURF#000 of G2");
    CHECK(scene.spawnedObjectCount() == 2);
    CHECK(fixture->say("spawn object NOPE") == "No object named NOPE in G2");
    CHECK(fixture->say("spawn NOPE") == "No object named NOPE in G2");
    CHECK(fixture->say("spawn object NOPE G1") == "No object named NOPE in G1");
    CHECK(fixture->say("spawn " + item + " G1") == "No object named GAPPLE in G1");
    CHECK(scene.spawnedObjectCount() == 2);
    WorldLayout fieldsLayout;
    REQUIRE(fieldsLayout.load(fixture->root / g1->directory));
    const std::string borrowed = fieldsLayout.objects().front().name;
    CHECK(fixture->say("spawn object " + borrowed + " G1") == "Stood " + borrowed + " of G1");
    CHECK(scene.spawnedObjectCount() == 3);
    reopen();

    // In the tower nobody falls, and unlock_tower reopens it where the asker stands.
    fixture.reset();
    auto tower = std::make_unique<Fixture>("L1");
    CHECK(tower->say("suicide") == "Nobody falls in the tower.");
    CHECK_FALSE(tower->scene.fallen(0));
    const PlayerActor* owner = tower->scene.actor(1);
    REQUIRE(owner != nullptr);
    const Vec3 at = owner->position();
    const f32 yaw = owner->yaw();
    const PlayerActor* first = tower->scene.actor(0);
    REQUIRE(first != nullptr);
    REQUIRE(first->position() != at);
    const auto reopened = tower->sayStep("unlock_tower", 1);
    CHECK(reopened.consoleReplies.front().text == "The tower stands open to the party.");
    CHECK(reopened.outcome == PlayOutcome::Travel);
    CHECK(tower->scene.consoleTravelRequest() ==
          PlayScene::ConsoleTravelRequest{LevelRef::tower(), at, yaw});
    CHECK(tower->scene.destination() == LevelRef::tower());
    CHECK_FALSE(tower->scene.secretTravel());
    CHECK(tower->save(0).progress().relics.runeCount() == Relics::kRuneCount);
}

TEST_CASE("while the party is on its way every verb is refused", "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    const s32 gold = fixture->save(0).gold;
    const std::array leaving{ConsoleAction{0, "goto G1"}, ConsoleAction{1, "god"},
                             ConsoleAction{0, "get gold 1"}};
    const auto first = fixture->sharedStep(leaving);
    CHECK(first.outcome == PlayOutcome::Travel);
    REQUIRE(first.consoleReplies.size() == leaving.size());
    CHECK(first.consoleReplies[0].text == "Going to G1 (Fields)");
    CHECK(first.consoleReplies[1].text == kOnItsWay);
    CHECK(first.consoleReplies[2].text == kOnItsWay);
    CHECK(first.consoleReplies[1].seat == 1);
    CHECK_FALSE(scene.cheats(1).god);
    CHECK(fixture->save(0).gold == gold);
    const auto bound = scene.nativeAgreementDigest();
    REQUIRE(bound);
    const auto every = awayLines();
    const auto later = fixture->sharedStep(every);
    CHECK(later.outcome == PlayOutcome::Travel);
    checkRefusedAway(every, later.consoleReplies);
    CHECK(scene.consoleTravelRequest()->destination.name == "G1");
    CHECK(scene.nativeAgreementDigest() == bound);
}

TEST_CASE("while the party goes through an exit every verb is refused",
          "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    const ExitPortals& portals = scene.portals();
    std::optional<Vec3> exit;
    for (usize i = 0; i < portals.size() && !exit; ++i) {
        if (!portals.portal(i).secret) {
            exit = portals.portal(i).position;
        }
    }
    REQUIRE(exit);
    REQUIRE(scene.relocate(*exit));
    for (s32 i = 0; i < kExitTicks && !scene.leaving(); ++i) {
        REQUIRE(fixture->sharedStep().outcome == PlayOutcome::Running);
    }
    REQUIRE(scene.leaving());
    REQUIRE_FALSE(scene.consoleTravelRequest());
    // The lines change nothing: the tick goes as one with no lines does.
    PlayScene::LocalSnapshot going;
    REQUIRE(scene.saveLocal(going));
    const u64 tick = fixture->tick;
    const auto every = awayLines();
    const auto asked = fixture->sharedStep(every);
    checkRefusedAway(every, asked.consoleReplies);
    CHECK_FALSE(scene.consoleTravelRequest());
    const auto refused = scene.nativeAgreementDigest();
    REQUIRE(refused);
    REQUIRE(scene.restoreLocal(going));
    fixture->tick = tick;
    const auto plain = fixture->sharedStep();
    CHECK(asked.outcome == plain.outcome);
    CHECK(scene.nativeAgreementDigest() == refused);
}

TEST_CASE("every verb the wire admits reaches its branch", "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));
    struct Line {
        std::string_view line;
        std::string_view reply;
    };
    constexpr std::array<Line, 8> kLines{{{"GOD", "god on"},
                                          {"NoTarget", "notarget on"},
                                          {"noclip", "noclip on"},
                                          {"Get gold 1", "Got 1 gold"},
                                          {"spawn gru", "Spawned 1 GRU at tier 1"},
                                          {"Suicide", "2 fallen"},
                                          {"goto 1 2 3", "Moved the party."},
                                          {"UNLOCK_TOWER", "The tower stands open to the party."}}};
    for (const Line& line : kLines) {
        CAPTURE(line.line);
        REQUIRE(scene.restoreLocal(opened));
        REQUIRE(ConsoleAction{0, std::string{line.line}}.valid());
        const std::string reply = fixture->say(std::string{line.line});
        CHECK(reply != "Unknown shared console command.");
        CHECK(reply == line.reply);
    }
}

TEST_CASE("shared noclip flies at full speed from the first tick", "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    const Vec3 start = scene.actor(1)->position();
    const Vec3 still = scene.actor(0)->position();
    const Vec3 wish{0.5f, 0.0f, -0.5f};
    PlayScene::Inputs inputs;
    inputs[1].consoleFlight = wish;
    const std::array actions{ConsoleAction{1, "noclip"}};
    REQUIRE(fixture->sharedStep(actions, inputs).outcome == PlayOutcome::Running);
    const Vec3 offset = wish * NoclipMotion::kSpeed * static_cast<f32>(kStep);
    CHECK(scene.actor(1)->position() == start + offset);
    CHECK(scene.actor(1)->yaw() == Catch::Approx(portable::atan2(wish.x, wish.z)));
    CHECK(scene.actor(0)->position() == still);
    REQUIRE(fixture->sharedStep({}, inputs).outcome == PlayOutcome::Running);
    CHECK(scene.actor(1)->position() == start + offset + offset);
}

TEST_CASE("the console's effects equal the same effects applied directly",
          "[native-console][assets]") {
    auto console = std::make_unique<Fixture>("G2");
    auto direct = std::make_unique<Fixture>("G2");
    REQUIRE(console->scene.nativeAgreementDigest() == direct->scene.nativeAgreementDigest());
    const Vec3 spot = console->scene.actor(0)->position() + Vec3{2.0f, 0.0f, 0.0f};
    struct Effect {
        ConsoleAction action;
        std::function<void(PlayScene&)> apply;
    };
    const std::array<Effect, 6> script{{
        {{0, "god"},
         [](PlayScene& scene) {
             auto cheats = scene.cheats(0);
             cheats.god = true;
             scene.setCheats(0, cheats);
         }},
        {{1, "notarget"},
         [](PlayScene& scene) {
             auto cheats = scene.cheats(1);
             cheats.noTarget = true;
             scene.setCheats(1, cheats);
         }},
        {{1, "get gold 37"},
         [](PlayScene& scene) {
             takeItem(*scene.liveSave(1), ItemOffer{static_cast<s32>(ItemKind::Gold), 37}, 1.0f,
                      true);
         }},
        {{0, "get supershot 3"},
         [](PlayScene& scene) {
             takeItem(*scene.liveSave(0),
                      ItemOffer{powerup::kWeapon, 3, damage::kSuperShot, kForGood}, 1.0f, true);
         }},
        {{0, std::format("goto {} {} {}", spot.x, spot.y, spot.z)},
         [spot](PlayScene& scene) { REQUIRE(scene.relocate(spot)); }},
        {{1, "suicide"},
         [](PlayScene& scene) {
             for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
                 scene.kill(seat);
             }
         }},
    }};
    // After every step the digest the peers compare is the same on both: the line did what
    // the primitive does, and nothing besides.
    for (const Effect& effect : script) {
        CAPTURE(effect.action.line);
        const std::array actions{effect.action};
        const auto stepped = console->sharedStep(actions);
        REQUIRE(stepped.consoleReplies.size() == 1);
        CAPTURE(stepped.consoleReplies.front().text);
        effect.apply(direct->scene);
        const auto applied = direct->sharedStep();
        CHECK(stepped.outcome == applied.outcome);
        const auto expected = direct->scene.nativeAgreementDigest();
        REQUIRE(expected);
        CHECK(console->scene.nativeAgreementDigest() == expected);
    }
    CHECK(console->scene.cheats(0).god);
    CHECK(console->scene.fallen(1));
}

TEST_CASE("an offline step applies the effects the shared step applies",
          "[native-console][assets]") {
    auto offline = std::make_unique<Fixture>("G2");
    auto shared = std::make_unique<Fixture>("G2");
    REQUIRE(offline->scene.nativeAgreementDigest() == shared->scene.nativeAgreementDigest());
    const std::string item = offline->world.placedItems().item(0).name;
    const std::string object = offline->world.layout().objects().front().name;
    const Vec3 spot = offline->scene.actor(0)->position() + Vec3{2.0f, 0.0f, 0.0f};
    const std::vector<std::vector<ConsoleAction>> script{
        {{0, "god"}, {1, "notarget"}, {1, "noclip"}},
        {{1, "get gold 37"}, {0, "get exp 10"}, {0, "get supershot 3"}, {1, "unlock_tower"}},
        {{0, std::format("goto {} {} {}", spot.x, spot.y, spot.z)}},
        {{0, "spawn gru 2 3"},
         {1, "spawn item " + item},
         {0, "spawn object " + object},
         {0, "spawn golem"}},
        {{1, "suicide"}},
        {{0, "goto G1"}},
    };
    // After every tick both scenes are alike to the digest the peers compare, and bound alike.
    for (const std::vector<ConsoleAction>& actions : script) {
        CAPTURE(actions.front().line);
        const auto local = offline->offlineStep(actions);
        const auto recorded = shared->sharedStep(actions);
        CHECK(local.outcome == recorded.outcome);
        REQUIRE(local.replies.size() == actions.size());
        REQUIRE(recorded.consoleReplies.size() == actions.size());
        for (usize i = 0; i < actions.size(); ++i) {
            CHECK(local.replies[i].seat == actions[i].seat);
            CHECK(recorded.consoleReplies[i].seat == actions[i].seat);
        }
        CHECK(offline->scene.consoleTravelRequest() == shared->scene.consoleTravelRequest());
        const auto expected = shared->scene.nativeAgreementDigest();
        REQUIRE(expected);
        CHECK(offline->scene.nativeAgreementDigest() == expected);
    }
    CHECK(offline->scene.consoleTravelRequest());
}

TEST_CASE("an offline step has no console spawn budget",
          "[native-console][native-console-spawn-limit][assets]") {
    auto offline = std::make_unique<Fixture>("G2");
    auto shared = std::make_unique<Fixture>("G2");
    const std::string object = offline->world.layout().objects().front().name;
    const std::string item = offline->world.placedItems().item(0).name;
    std::vector<ConsoleAction> crowded(ConsoleStage::kMaximumConsoleSpawns,
                                       ConsoleAction{0, "spawn object " + object});
    crowded.push_back({0, "spawn item " + item});
    // Offline every spawn stands while there is room for it: the objects' room, then the item.
    const auto local = offline->offlineStep(crowded);
    REQUIRE(local.replies.size() == crowded.size());
    CHECK(offline->scene.spawnedObjectCount() == SpawnedObjects::kMaxObjects);
    CHECK(local.replies.back().text.starts_with("Dropped "));
    // The shared game stops at its budget.
    const auto recorded = shared->sharedStep(crowded);
    REQUIRE(recorded.consoleReplies.size() == crowded.size());
    CHECK(shared->scene.spawnedObjectCount() == SpawnedObjects::kMaxObjects);
    CHECK(recorded.consoleReplies.back().text == "The stage's console spawn limit is reached.");
}

TEST_CASE("an offline step takes a console line the wire would not carry",
          "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    const std::array actions{ConsoleAction{0, "god" + std::string(kLongLine - 3, ' ')}};
    REQUIRE(actions.front().admitted());
    REQUIRE_FALSE(actions.front().valid());
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));
    const u64 tick = fixture->tick;
    // The shared game refuses it whole: the tick goes as one with no line does.
    const auto refused = fixture->sharedStep(actions);
    REQUIRE(refused.consoleReplies.size() == 1);
    CHECK(refused.consoleReplies.front().text == kNoScene);
    CHECK_FALSE(scene.cheats(0).god);
    const auto digest = scene.nativeAgreementDigest();
    REQUIRE(digest);
    REQUIRE(scene.restoreLocal(opened));
    fixture->tick = tick;
    REQUIRE(fixture->sharedStep().outcome == PlayOutcome::Running);
    CHECK(scene.nativeAgreementDigest() == digest);
    // Offline play runs it.
    REQUIRE(scene.restoreLocal(opened));
    const auto taken = fixture->offlineStep(actions);
    REQUIRE(taken.replies.size() == 1);
    CHECK(taken.replies.front().seat == 0);
    CHECK(scene.cheats(0).god);
}

TEST_CASE("a scene opened with carried cheats holds them from its first tick",
          "[native-console][assets]") {
    const ConsoleCheats every{.god = true, .noTarget = true, .noclip = true};
    std::array<ConsoleCheats, PlayOptions::kSeats> carried{};
    carried[0] = every;
    auto fixture = std::make_unique<Fixture>("G2", carried);
    auto& scene = fixture->scene;
    // Seat 0 is a ghost the swarm does not see; seat 1 is as it came.
    const auto holds = [&scene, &every] {
        CHECK(scene.cheats(0) == every);
        CHECK(scene.runtime(0)->ghost);
        CHECK(scene.opponents().unseen(0));
        CHECK(scene.cheats(1) == ConsoleCheats{});
        CHECK_FALSE(scene.runtime(1)->ghost);
        CHECK_FALSE(scene.opponents().unseen(1));
    };
    holds();
    REQUIRE(fixture->offlineStep().outcome == PlayOutcome::Running);
    holds();
}

TEST_CASE("cheats set on a scene resumed from a challenge hold", "[native-console][assets]") {
    auto fixture = std::make_unique<Fixture>("G2");
    auto& scene = fixture->scene;
    CHECK(fixture->say("god") == "god on");
    REQUIRE(fixture->sayStep("goto S1", 1).outcome == PlayOutcome::Travel);
    REQUIRE(scene.secretTravel());
    scene.suspendForChallenge();
    scene.resumeFromChallenge(scene.party());
    CHECK_FALSE(scene.consoleTravelRequest());
    // What the scene held through the challenge it still holds; what is set on its return holds.
    const ConsoleCheats every{.god = true, .noTarget = true, .noclip = true};
    scene.setCheats(1, every);
    const auto holds = [&scene, &every] {
        CHECK(scene.cheats(0) == ConsoleCheats{.god = true});
        CHECK_FALSE(scene.runtime(0)->ghost);
        CHECK(scene.cheats(1) == every);
        CHECK(scene.runtime(1)->ghost);
        CHECK(scene.opponents().unseen(1));
    };
    holds();
    REQUIRE(fixture->sharedStep().outcome == PlayOutcome::Running);
    holds();
}
