#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Mt19937.h"

#include "FakeMechanic.h"
#include "NativeStateSupport.h"
#include "game/config/GameConfig.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/mechanics/MechanicsFacade.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::FakeMechanic;

constexpr std::array kSources{BlowSource::Swing,  BlowSource::Shot,  BlowSource::SuperShot,
                              BlowSource::Potion, BlowSource::Turbo, BlowSource::Charge,
                              BlowSource::Breath, BlowSource::Shield};
constexpr std::array kOutcomes{BlowOutcome::Hurt, BlowOutcome::Killed, BlowOutcome::Destroyed};
constexpr std::array kTargets{BlowTarget::Enemy, BlowTarget::Critter, BlowTarget::Boss,
                              BlowTarget::Generator};

/** The digest of what `facade` holds now, or `saved` when given. */
NativeStateDigest::Digest digestOf(const MechanicsFacade& facade,
                                   const MechanicsFacade::LocalSnapshot* saved = nullptr) {
    NativeStateDigest digest;
    REQUIRE((saved != nullptr ? facade.appendLocalDigest(digest, *saved)
                              : facade.appendNativeDigest(digest)));
    const auto finished = digest.checkedFinish();
    REQUIRE(finished.has_value());
    return *finished;
}

TEST_CASE("with no mechanic the facade answers as the original does", "[game][mechanics]") {
    MechanicsFacade facade;
    const MechanicCatalog none;
    facade.open(&none, {});
    CHECK(facade.empty());
    const std::array<const MechanicsFacade*, 2> facades{&facade, &MechanicsFacade::standard()};
    for (const MechanicsFacade* answering : facades) {
        for (const BlowSource source : kSources) {
            for (const BlowOutcome outcome : kOutcomes) {
                for (const BlowTarget target : kTargets) {
                    CHECK_FALSE(answering->runEffect({source, outcome, target}).has_value());
                }
            }
            CHECK(answering->generatorTakes(source));
        }
        CHECK_FALSE(answering->quietWindow(30).has_value());
        CHECK(answering->tuning() == SwarmTuning{});
        Mt19937 random{7};
        Mt19937 untouched{7};
        CHECK(answering->traitsFor({.kind = 0, .frenzied = true}, random) == BodyTraits{});
        CHECK(random() == untouched()); // nothing drawn from the swarm's stream
        CHECK(answering->look(12).tint == Color::white());
        CHECK_FALSE(answering->look(12).marked);
        CHECK(answering->bodyScale(3) == 1.0f);
    }
}

TEST_CASE("the original counts close swings on the swarm, the great ones and the bosses",
          "[game][mechanics][streak]") {
    for (const BlowSource source : kSources) {
        for (const BlowOutcome outcome : kOutcomes) {
            for (const BlowTarget target : kTargets) {
                CAPTURE(static_cast<int>(source), static_cast<int>(outcome),
                        static_cast<int>(target));
                const bool counted = source == BlowSource::Swing && target != BlowTarget::Generator;
                CHECK(originalRunEffect({source, outcome, target}) ==
                      (counted ? RunEffect::CountAsOriginal : RunEffect::Nothing));
            }
        }
    }
}

TEST_CASE("the catalog keeps its mechanics by id and gives the room's rules a bit each",
          "[game][mechanics]") {
    MechanicCatalog catalog;
    CHECK(catalog.add(FakeMechanic::descriptor("zeta")));
    CHECK(catalog.add(FakeMechanic::descriptor("alpha", true, 3)));
    CHECK(catalog.add(FakeMechanic::descriptor("local-only", false)));
    CHECK_FALSE(catalog.add(FakeMechanic::descriptor("alpha"))); // one of each
    CHECK_FALSE(catalog.add(FakeMechanic::descriptor("Upper")));
    CHECK_FALSE(catalog.add(FakeMechanic::descriptor("")));
    CHECK_FALSE(catalog.add(FakeMechanic::descriptor("has space")));
    MechanicDescriptor unmade = FakeMechanic::descriptor("unmade");
    unmade.create = nullptr;
    CHECK_FALSE(catalog.add(unmade));
    REQUIRE(catalog.all().size() == 3);
    CHECK(catalog.all()[0].id == "alpha");
    CHECK(catalog.all()[1].id == "local-only");
    CHECK(catalog.all()[2].id == "zeta");
    CHECK(catalog.find("zeta") != nullptr);
    CHECK(catalog.find("omega") == nullptr);
    CHECK(catalog.roster() == "alpha@3\nlocal-only@1\nzeta@1\n");
    // The room's rules in id order, the local one left out.
    const auto rules = catalog.roomRules();
    REQUIRE(rules.size() == 2);
    CHECK(rules[0]->id == "alpha");
    CHECK(rules[1]->id == "zeta");
    CHECK(catalog.knownRoomBits() == 0b11);
    GameOptionsConfig options;
    CHECK(catalog.roomBits(options) == 0);
    options.mechanics["zeta"] = true;
    options.mechanics["local-only"] = true;
    CHECK(catalog.roomBits(options) == 0b10);
    // Online the room's bits alone switch them: a machine's own choice falls away.
    applyRoomRules(&catalog, 0b01, options);
    CHECK(MechanicCatalog::enabled(options, "alpha"));
    CHECK_FALSE(MechanicCatalog::enabled(options, "zeta"));
    CHECK_FALSE(MechanicCatalog::enabled(options, "local-only"));
    applyRoomRules(nullptr, 0b11, options);
    CHECK(options.mechanics.empty());
    // A room holds at most eight rules, one bit each.
    MechanicCatalog full;
    std::vector<std::string> ids;
    for (char letter = 'a'; letter <= 'i'; ++letter) {
        ids.emplace_back(1, letter);
    }
    for (usize i = 0; i < ids.size(); ++i) {
        CHECK(full.add(FakeMechanic::descriptor(ids[i])) == (i < MechanicCatalog::kMostRoomRules));
    }
    CHECK(full.add(FakeMechanic::descriptor(ids.back(), false)));
}

TEST_CASE("the facade builds one of each mechanic a level, switched by the settings",
          "[game][mechanics]") {
    MechanicCatalog catalog;
    REQUIRE(catalog.add(FakeMechanic::descriptor("beta")));
    REQUIRE(catalog.add(FakeMechanic::descriptor("alpha")));
    MechanicsFacade facade;
    facade.open(&catalog, {.tower = true});
    REQUIRE(FakeMechanic::made().size() == 2);
    FakeMechanic& alpha = *FakeMechanic::made()[0];
    FakeMechanic& beta = *FakeMechanic::made()[1];
    CHECK(facade.find("alpha") == &alpha);
    CHECK(facade.find("beta") == &beta);
    CHECK(facade.find("gamma") == nullptr);
    CHECK(alpha.opened == 1);
    CHECK(alpha.tower);
    CHECK_FALSE(alpha.enabled);
    GameOptionsConfig options;
    options.mechanics["beta"] = true;
    facade.configure(options);
    CHECK_FALSE(alpha.enabled);
    CHECK(beta.enabled);
    options.mechanics["beta"] = false;
    facade.configure(options);
    CHECK_FALSE(beta.enabled);
    // The next level has its own, the level before's closed on the way.
    const s32 closings = FakeMechanic::closings();
    facade.open(&catalog, {});
    CHECK(FakeMechanic::closings() == closings + 2);
    REQUIRE(FakeMechanic::made().size() == 2);
    CHECK(FakeMechanic::made()[0]->opened == 1);
    CHECK_FALSE(FakeMechanic::made()[0]->tower);
    facade.close();
    CHECK(FakeMechanic::closings() == closings + 4);
    CHECK(facade.empty());
    CHECK(FakeMechanic::made().empty());
}

TEST_CASE("the facade folds its mechanics' answers in catalog order", "[game][mechanics]") {
    MechanicCatalog catalog;
    REQUIRE(catalog.add(FakeMechanic::descriptor("first")));
    REQUIRE(catalog.add(FakeMechanic::descriptor("second")));
    MechanicsFacade facade;
    facade.open(&catalog, {});
    FakeMechanic& first = *FakeMechanic::made()[0];
    FakeMechanic& second = *FakeMechanic::made()[1];
    // The first with something to say about a blow or a quiet decides it.
    second.answers.runEffect = RunEffect::Count;
    second.answers.quiet = 1.5f;
    const BlowReport blow{BlowSource::Shot, BlowOutcome::Killed, BlowTarget::Enemy};
    CHECK(facade.runEffect(blow) == RunEffect::Count);
    first.answers.runEffect = RunEffect::Nothing;
    CHECK(facade.runEffect(blow) == RunEffect::Nothing);
    CHECK(facade.quietWindow(4) == 1.5f);
    first.answers.quiet = 2.5f;
    CHECK(facade.quietWindow(4) == 2.5f);
    // Any may keep a blow from a generator.
    CHECK(facade.generatorTakes(BlowSource::Potion));
    second.answers.generatorTakes = false;
    CHECK_FALSE(facade.generatorTakes(BlowSource::Potion));
    // Tunings multiply; the frenzy reaches as far as the farthest; the whole pool if any asks.
    first.answers.tuning = {.breedingPace = 2.0f, .healthScale = 3.0f, .frenzyReach = 40.0f};
    second.answers.tuning = {.breedingPace = 1.5f, .frenzyReach = 60.0f, .wholePool = true};
    const SwarmTuning tuning = facade.tuning();
    CHECK(tuning.breedingPace == 3.0f);
    CHECK(tuning.broodScale == 1.0f);
    CHECK(tuning.healthScale == 3.0f);
    CHECK(tuning.frenzyReach == 60.0f);
    CHECK(tuning.wholePool);
    // The first to give a birth something decides it, drawing as it must.
    second.answers.traits = {.affinity = 2, .healthScale = 3.5f, .sizeScale = 1.5f};
    second.answers.draws = 2;
    Mt19937 random{11};
    Mt19937 expected{11};
    CHECK(facade.traitsFor({.kind = 1, .frenzied = true}, random) == second.answers.traits);
    (void)expected();
    (void)expected();
    CHECK(random() == expected());
    // Every tint is laid on; the first marked look rims the swarm; body scales multiply.
    first.answers.look = {.tint = Color::rgba(255, 128, 255)};
    second.answers.look = {.tint = Color::rgba(128, 255, 255),
                           .marked = true,
                           .rim = Color::rgba(200, 0, 0),
                           .wash = Color::rgba(100, 0, 0),
                           .rimWidth = 0.01f};
    const SwarmLook look = facade.look(3);
    CHECK(look.tint == Color::rgba(128, 128, 255));
    CHECK(look.marked);
    CHECK(look.rim == Color::rgba(200, 0, 0));
    CHECK(look.rimWidth == 0.01f);
    first.answers.scale = 2.0f;
    second.answers.scale = 1.5f;
    CHECK(facade.bodyScale(1) == 3.0f);
    CHECK(facade.bodyScale(0) == 1.0f);
    // Every one hears of a step and a fall.
    const std::array<SeatRun, 2> runs{SeatRun{.count = 3, .running = true}, SeatRun{}};
    facade.step({.seconds = 0.5f, .runs = runs});
    CHECK(first.steps == 1);
    CHECK(second.steps == 1);
    REQUIRE(second.runsStepped.size() == 2);
    CHECK(second.runsStepped[0].count == 3);
}

TEST_CASE("the mechanics' state rewinds and is agreed with the rest of the level",
          "[game][mechanics][rollback]") {
    MechanicCatalog catalog;
    REQUIRE(catalog.add(FakeMechanic::descriptor("alpha")));
    MechanicsFacade facade;
    facade.open(&catalog, {});
    const FakeMechanic& alpha = *FakeMechanic::made()[0];
    GameOptionsConfig on;
    on.mechanics["alpha"] = true;
    facade.configure(on);
    facade.step({.seconds = 0.25f});
    const auto before = digestOf(facade);
    MechanicsFacade::LocalSnapshot saved;
    facade.saveLocal(saved);
    REQUIRE(facade.canRestoreLocal(saved));
    CHECK(digestOf(facade, &saved) == before);
    facade.step({.seconds = 0.25f});
    facade.configure({});
    CHECK(alpha.steps == 2);
    CHECK_FALSE(alpha.enabled);
    CHECK(digestOf(facade) != before);
    REQUIRE(facade.restoreLocal(saved));
    CHECK(alpha.steps == 1);
    CHECK(alpha.seconds == 0.25f);
    CHECK(alpha.enabled); // the switch rewinds with the state it ruled
    CHECK(digestOf(facade) == before);
    // A snapshot belongs to its level alone.
    facade.open(&catalog, {});
    CHECK_FALSE(facade.canRestoreLocal(saved));
    CHECK_FALSE(facade.restoreLocal(saved));
    // With nothing laid over the game, the digest still says so.
    MechanicsFacade bare;
    const MechanicCatalog none;
    bare.open(&none, {});
    CHECK(digestOf(bare) != digestOf(facade));
}

TEST_CASE("the mechanics' digest bytes stay pinned after their steps and what they saved",
          "[game][mechanics][rollback][digest-golden]") {
    MechanicCatalog catalog;
    REQUIRE(catalog.add(FakeMechanic::descriptor("alpha")));
    REQUIRE(catalog.add(FakeMechanic::descriptor("beta", true, 3)));
    MechanicsFacade facade;
    facade.open(&catalog, {});
    GameOptionsConfig options;
    options.mechanics["alpha"] = true;
    facade.configure(options);
    facade.step({.seconds = 0.25f, .runs = {}});
    facade.step({.seconds = 0.5f, .runs = {}});
    MechanicsFacade::LocalSnapshot saved;
    facade.saveLocal(saved);
    options.mechanics["beta"] = true;
    facade.configure(options);
    facade.step({.seconds = 0.125f, .runs = {}});
    // SHA-256 of the mechanics' native digest after three steps, both switched on, and of what
    // they saved after two, the first alone on, which netplay peers must hash alike: a literal
    // moves only when what is visited changes on purpose.
    test::checkPinnedDigests("MechanicsFacade", digestOf(facade), digestOf(facade, &saved),
                             "0727899895e350b40c37f5b7136574022eac4834bf7da320c20b96a413c9185f",
                             "79408224be29049313c558602e0332fe774b5065ecc23c66f60d1cfa2f2296f8");
}

} // namespace
