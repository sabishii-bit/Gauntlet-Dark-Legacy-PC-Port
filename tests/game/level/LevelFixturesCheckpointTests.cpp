#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/FixtureHost.h"
#include "game/level/LevelFixturesCheckpoint.h"
#include "game/level/ScriptedFixtures.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    LevelFixturesCheckpoint owner{fixtures};
    std::array<PlayerRuntime, 2> players;
    std::vector<std::pair<usize, f32>> hits;
    test::FixtureScript events{
        .hurt = [&](usize index, f32 damage, HurtKind, bool,
                    const PlayerImpact&) { hits.emplace_back(index, damage); },
        .opponents =
            [](const Vec3&, f32, f32, std::vector<s32>& reached, u32) {
                if (std::ranges::find(reached, 17) == reached.end()) {
                    reached.push_back(17);
                }
            }};
    test::ScriptedFixtures host{events};
    Fixture() {
        bind();
        players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, {6, 0, 0}, 0);
    }
    void bind() {
        fixtures.bind({device, world, weapons, effects, audio, 1});
        REQUIRE(owner.bindResources());
    }
};

// Outputs belong to the caller's confirmed tick, not to an owner's rewind data.
struct NativeContacts {
    std::vector<std::tuple<usize, f32, HurtKind, bool, u32, Vec3>> hurts;
    std::vector<std::tuple<Vec3, f32, f32, std::vector<s32>, u32>> opponents;
    std::vector<std::pair<s32, usize>> help;
    std::vector<std::pair<s32, std::string>> cards;
    std::vector<std::tuple<s32, Vec3, s32>> releases;
    std::vector<std::pair<s32, Vec3>> potions;
    bool operator==(const NativeContacts&) const = default;

    test::FixtureScript events() {
        return {
            .hurt =
                [&](usize player, f32 damage, HurtKind kind, bool directed,
                    const PlayerImpact& impact) {
                    hurts.emplace_back(player, damage, kind, directed, impact.flags,
                                       impact.direction);
                },
            .help =
                [&](s32 id, usize player) {
                    help.emplace_back(id, player);
                    return true;
                },
            .card = [&](s32 player, std::string_view text) { cards.emplace_back(player, text); },
            .opponents =
                [&](const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                    u32 flags) {
                    opponents.emplace_back(position, radius, damage, reached, flags);
                    if (std::ranges::find(reached, 17) == reached.end()) {
                        reached.push_back(17);
                    }
                    if (radius >= 6 && std::ranges::find(reached, 31) == reached.end()) {
                        reached.push_back(31);
                    }
                },
            .releaseEnemy =
                [&](s32 record, const Vec3& position, s32 count) {
                    releases.emplace_back(record, position, count);
                    return false;
                },
            .shatterPotion =
                [&](s32 subtype, const Vec3& position) {
                    potions.emplace_back(subtype, position);
                }};
    }
};

struct NativeHistory {
    LevelFixtures::LocalSnapshot fixtures;
    LevelWorld::LocalSnapshot world;
    EffectTrees::LocalSnapshot effects;
    std::array<PlayerSimulation, 2> players;

    void save(const Fixture& source) {
        source.fixtures.saveLocal(fixtures);
        source.world.saveLocal(world);
        source.effects.saveLocal(effects);
        for (usize i = 0; i < players.size(); ++i) {
            players[i] = source.players[i].simulation();
        }
    }
    void restore(Fixture& target) const {
        REQUIRE(target.fixtures.canRestoreLocal(fixtures));
        REQUIRE(target.world.canRestoreLocal(world));
        REQUIRE(target.effects.canRestoreLocal(effects));
        REQUIRE(target.world.restoreLocal(world));
        REQUIRE(target.fixtures.restoreLocal(fixtures));
        REQUIRE(target.effects.restoreLocal(effects));
        for (usize i = 0; i < players.size(); ++i) {
            target.players[i].simulation() = players[i];
        }
    }
};

void checkNativeParticipants(const Fixture& actual, const Fixture& expected) {
    for (usize i = 0; i < actual.players.size(); ++i) {
        CHECK(actual.players[i].actor.checkpoint() == expected.players[i].actor.checkpoint());
        CHECK(actual.players[i].knockback.checkpoint() ==
              expected.players[i].knockback.checkpoint());
        CHECK(actual.players[i].reaction == expected.players[i].reaction);
        CHECK(actual.players[i].fixtureSpot == expected.players[i].fixtureSpot);
        CHECK(actual.players[i].cloudGap == expected.players[i].cloudGap);
        CHECK(actual.players[i].hitSoundGap == expected.players[i].hitSoundGap);
    }
}

TEST_CASE("native fixture rewind discards wrong blast contacts before replaying one to eight ticks",
          "[rollback][local-rewind][level-fixtures-local-rewind]") {
    Fixture uninterrupted;
    Fixture replay;
    NativeContacts expected;
    NativeContacts actual;
    const auto expectedEvents = expected.events();
    const auto actualEvents = actual.events();
    test::ScriptedFixtures expectedHost{expectedEvents};
    test::ScriptedFixtures actualHost{actualEvents};
    NativeHistory history;
    std::array<LevelFixtures::LocalSnapshot, 8> expectedLocal;
    LevelFixtures::LocalSnapshot observed;
    constexpr f32 kSeconds = 1.0f / 30.0f;
    usize delayedContacts = 0;
    s32 frame = 0;
    const auto step = [&](Fixture& fixture, FixtureHost& host, s32 tick) {
        if (tick % 32 == 0) {
            fixture.fixtures.blast({0, 0, 0}, 12, 30, fixture.players, host);
        }
        fixture.fixtures.advanceBlasts(kSeconds, fixture.players, host);
        const auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        REQUIRE(state->valid());
        for (const auto& blast : state->blasts) {
            CHECK(std::ranges::count(blast.players, usize{0}) <= 1);
            CHECK(std::ranges::count(blast.players, usize{1}) <= 1);
            CHECK(std::ranges::count(blast.opponents, 17) == 1);
            CHECK(std::ranges::count(blast.opponents, 31) <= 1);
        }
    };
    for (s32 window = 0; window < 96; ++window) {
        const s32 length = 1 + window % 8;
        CAPTURE(window, length, frame);
        history.save(replay);
        const auto before = replay.owner.checkpoint();
        REQUIRE(before);
        expected = {};
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(replay, actualHost, frame + i);
            replay.fixtures.saveLocal(expectedLocal[static_cast<usize>(i)]);
        }
        history.restore(replay);
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(uninterrupted, expectedHost, frame + i);
            replay.fixtures.blast({6, 0, 0}, 24, 80, replay.players, actualHost);
            replay.fixtures.advanceBlasts(kSeconds * 2, replay.players, actualHost);
        }
        REQUIRE(replay.owner.checkpoint() != before);
        const auto rejected = actual;
        const auto uploads = replay.device.texturesCreated;
        history.restore(replay);
        CHECK(actual == rejected); // Rewinding does not emit or clear caller-owned output.
        CHECK(replay.owner.checkpoint() == before);
        CHECK(replay.device.texturesCreated == uploads);
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(replay, actualHost, frame + i);
            replay.fixtures.saveLocal(observed);
            CHECK(replay.fixtures.equalLocal(observed, expectedLocal[static_cast<usize>(i)]));
        }
        CHECK(replay.owner.checkpoint() == uninterrupted.owner.checkpoint());
        CHECK(actual == expected);
        checkNativeParticipants(replay, uninterrupted);
        delayedContacts += static_cast<usize>(std::ranges::count_if(
            actual.hurts, [](const auto& hit) { return std::get<0>(hit) == 1; }));
        frame += length;
    }
    CHECK(delayedContacts >= 10);
}

TEST_CASE("native fixture aggregate refuses foreign and stale children before changing siblings",
          "[rollback][local-rewind][level-fixtures-local-rewind]") {
    Fixture source;
    Fixture other;
    LevelFixtures::LocalSnapshot saved;
    CHECK_FALSE(source.fixtures.equalLocal(saved, saved));
    source.fixtures.saveLocal(saved);
    CHECK(source.fixtures.equalLocal(saved, saved));
    const auto untouched = other.owner.checkpoint();
    CHECK_FALSE(other.fixtures.canRestoreLocal(saved));
    CHECK_FALSE(other.fixtures.equalLocal(saved, saved));
    CHECK_FALSE(other.fixtures.restoreLocal(saved));
    CHECK(other.owner.checkpoint() == untouched);
    source.fixtures.blast({0, 0, 0}, 12, 30, source.players, source.host);
    const auto advanced = source.owner.checkpoint();
    REQUIRE(advanced);
    REQUIRE(advanced->blasts.size() == 1);
    source.fixtures.gates().clear();
    CHECK_FALSE(source.fixtures.equalLocal(saved, saved));
    CHECK_FALSE(source.fixtures.canRestoreLocal(saved));
    CHECK_FALSE(source.fixtures.restoreLocal(saved));
    const auto refused = source.owner.checkpoint();
    REQUIRE(refused);
    CHECK(refused->blasts == advanced->blasts);
    CHECK(refused->chests == advanced->chests);
    source.bind();
    CHECK_FALSE(source.fixtures.restoreLocal(saved));
    CHECK_FALSE(source.fixtures.equalLocal(saved, saved));
}

TEST_CASE("native fixture equality distinguishes reached identities with the same blast geometry",
          "[rollback][local-rewind][level-fixtures-local-rewind][native-equality]") {
    Fixture fixture;
    LevelFixtures::LocalSnapshot initial;
    LevelFixtures::LocalSnapshot first;
    LevelFixtures::LocalSnapshot second;
    LevelFixtures::LocalSnapshot repeated;
    fixture.fixtures.saveLocal(initial);
    const auto hit = [&](s32 identity) {
        test::FixtureScript events;
        events.opponents = [identity](const Vec3&, f32, f32, std::vector<s32>& reached, u32) {
            reached.push_back(identity);
        };
        test::ScriptedFixtures host{events};
        fixture.fixtures.blast({0, 0, 0}, 12, 30, {}, host);
    };
    hit(17);
    fixture.fixtures.saveLocal(first);
    const auto firstDiagnostic = fixture.owner.checkpoint();
    REQUIRE(firstDiagnostic);
    REQUIRE(firstDiagnostic->blasts.size() == 1);
    REQUIRE(fixture.fixtures.restoreLocal(initial));
    hit(31);
    fixture.fixtures.saveLocal(second);
    const auto secondDiagnostic = fixture.owner.checkpoint();
    REQUIRE(secondDiagnostic);
    REQUIRE(secondDiagnostic->blasts.size() == 1);
    CHECK(firstDiagnostic->blasts.front().position == secondDiagnostic->blasts.front().position);
    CHECK(firstDiagnostic->blasts.front().elapsed == secondDiagnostic->blasts.front().elapsed);
    CHECK(firstDiagnostic->blasts.front().opponents.size() ==
          secondDiagnostic->blasts.front().opponents.size());
    CHECK_FALSE(fixture.fixtures.equalLocal(first, second));
    REQUIRE(fixture.fixtures.restoreLocal(initial));
    hit(17);
    fixture.fixtures.saveLocal(repeated);
    CHECK(fixture.fixtures.equalLocal(first, repeated));
}

TEST_CASE("native G2 fixture and world rewind retains compound children and speculative births",
          "[rollback][local-rewind][level-fixtures-local-rewind][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G2");
    REQUIRE(level);
    Fixture uninterrupted;
    Fixture replay;
    for (Fixture* fixture : {&uninterrupted, &replay}) {
        fixture->fixtures.clear();
        REQUIRE(fixture->world.load(fixture->device, root, *level));
        REQUIRE(fixture->weapons.load(root / "WEAPONS"));
        fixture->bind();
        fixture->world.setPlayerCount(2);
        fixture->fixtures.setPlayerCount(2);
    }
    REQUIRE(uninterrupted.fixtures.barrels().size() > 0);
    REQUIRE(uninterrupted.fixtures.chests().size() > 0);
    const auto barrels = std::views::iota(usize{0}, uninterrupted.fixtures.barrels().size());
    const auto barrel = std::ranges::find_if(barrels, [&](usize index) {
        const auto& candidate = uninterrupted.fixtures.barrels().barrel(index);
        return uninterrupted.fixtures.barrels().standing(index) && candidate.armor >= 0 &&
               candidate.kind == BreakableStrike::Kind::Exploding;
    });
    REQUIRE(barrel != barrels.end());
    const usize explosive = *barrel;
    REQUIRE(explosive < uninterrupted.fixtures.barrels().size());
    const Vec3 origin = uninterrupted.fixtures.barrels().barrel(explosive).figure.position();
    const auto contactBarrel = std::ranges::find_if(barrels, [&](usize index) {
        return index != explosive && uninterrupted.fixtures.barrels().standing(index);
    });
    REQUIRE(contactBarrel != barrels.end());
    const Vec3 contact = uninterrupted.fixtures.barrels().barrel(*contactBarrel).figure.position();
    for (Fixture* fixture : {&uninterrupted, &replay}) {
        fixture->players[0].actor.spawn(3, {}, nullptr, origin + Vec3{4, 0, 0}, 0);
        fixture->players[1].actor.spawn(1, {}, nullptr, origin + Vec3{8, 0, 0}, 0);
    }
    NativeContacts expected;
    NativeContacts actual;
    const auto expectedEvents = expected.events();
    const auto actualEvents = actual.events();
    test::ScriptedFixtures expectedHost{expectedEvents};
    test::ScriptedFixtures actualHost{actualEvents};
    NativeHistory history;
    std::array<LevelFixtures::LocalSnapshot, 8> expectedLocal;
    LevelFixtures::LocalSnapshot observed;
    constexpr f32 kSeconds = 1.0f / 30.0f;
    bool sawEffect = false;
    bool sawRubble = false;
    bool sawReached = false;
    s32 frame = 0;
    const auto step = [&](Fixture& fixture, FixtureHost& host, s32 tick) {
        fixture.world.capturePresentation();
        fixture.fixtures.capturePresentation();
        fixture.effects.capturePresentation();
        if (tick == 0) {
            fixture.fixtures.strikeBarrel(explosive, 10000, 3, fixture.players, host);
            REQUIRE_FALSE(fixture.fixtures.barrels().standing(explosive));
            fixture.fixtures.blast(contact, 12, 30, fixture.players, host);
        }
        fixture.world.update(kSeconds);
        fixture.fixtures.update(1, kSeconds, fixture.players, host);
        fixture.effects.update(kSeconds);
    };
    for (s32 window = 0; window < 24; ++window) {
        const s32 length = 1 + window % 8;
        CAPTURE(window, length, frame);
        history.save(replay);
        expected = {};
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(replay, actualHost, frame + i);
            replay.fixtures.saveLocal(expectedLocal[static_cast<usize>(i)]);
        }
        history.restore(replay);
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(uninterrupted, expectedHost, frame + i);
            replay.fixtures.strikeBarrel(static_cast<usize>(window) %
                                             replay.fixtures.barrels().size(),
                                         10000, 3, replay.players, actualHost);
            replay.fixtures.worldExplosion(origin, replay.players, actualHost);
            step(replay, actualHost, frame + i);
        }
        const auto uploads = replay.device.texturesCreated;
        const auto updates = replay.device.textureUpdates;
        const auto rejected = actual;
        history.restore(replay);
        CHECK(actual == rejected);
        CHECK(replay.device.texturesCreated == uploads);
        CHECK(replay.device.textureUpdates == updates);
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(replay, actualHost, frame + i);
            replay.fixtures.saveLocal(observed);
            CHECK(replay.fixtures.equalLocal(observed, expectedLocal[static_cast<usize>(i)]));
        }
        const auto state = uninterrupted.owner.checkpoint();
        REQUIRE(state);
        REQUIRE(state->valid());
        CHECK(replay.owner.checkpoint() == state);
        CHECK(actual == expected);
        checkNativeParticipants(replay, uninterrupted);
        REQUIRE(replay.effects.count() == uninterrupted.effects.count());
        for (usize i = 0; i < replay.effects.count(); ++i) {
            const auto expectedEffect =
                uninterrupted.effects.checkpoint(uninterrupted.effects.effect(i).id);
            REQUIRE(expectedEffect);
            CHECK(replay.effects.checkpoint(replay.effects.effect(i).id) == expectedEffect);
        }
        sawEffect |= uninterrupted.effects.count() > 0;
        sawRubble |= !state->rubble.pieces.empty();
        sawReached |= std::ranges::any_of(state->blasts,
                                          [](const auto& ring) { return !ring.reached.empty(); });
        frame += length;
    }
    CHECK(sawEffect);
    CHECK(sawRubble);
    CHECK(sawReached);
}

TEST_CASE("fixture audio identities survive a corrected away hit on another barrel",
          "[fixture-audio-rewind][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    Fixture fixture;
    fixture.fixtures.clear();
    REQUIRE(fixture.world.load(fixture.device, root, *level));
    fixture.audio.open(root, nullptr, fixture.world.audio(), 'G', false);
    fixture.bind();
    fixture.fixtures.setPlayerCount(2);
    std::vector<usize> barrels;
    for (usize index = 0; index < fixture.fixtures.barrels().size(); ++index) {
        const auto& barrel = fixture.fixtures.barrels().barrel(index);
        if (fixture.fixtures.barrels().standing(index) && barrel.health > 1) {
            barrels.push_back(index);
        }
    }
    REQUIRE(barrels.size() >= 2);
    SceneSoundEvents sounds;
    sounds.open(44, nullptr);
    fixture.audio.recordTo(&sounds);
    LevelFixtures::LocalSnapshot before;
    fixture.fixtures.saveLocal(before);
    sounds.beginTick(1);
    fixture.fixtures.strikeBarrel(barrels[0], 0.1f, 3, fixture.players, fixture.host);
    fixture.fixtures.strikeBarrel(barrels[1], 0.1f, 3, fixture.players, fixture.host);
    const auto original = sounds.endFrame();
    REQUIRE(original.finite.events.size() == 2);
    REQUIRE(fixture.fixtures.restoreLocal(before));
    sounds.beginTick(1);
    fixture.fixtures.strikeBarrel(barrels[1], 0.1f, 3, fixture.players, fixture.host);
    const auto corrected = sounds.endFrame();
    REQUIRE(corrected.finite.events.size() == 1);
    CHECK(corrected.finite.events.front() == original.finite.events.back());
    CHECK(corrected.finite.events.front().id != original.finite.events.front().id);
    const std::array initial{original};
    REQUIRE(sounds.reconcileFrames(1, initial));
    const std::array replay{corrected};
    const auto reconciled = sounds.reconcileFrames(1, replay);
    REQUIRE(reconciled);
    CHECK(reconciled.actions.empty());
    REQUIRE(reconciled.unretractable.size() == 1);
    CHECK(reconciled.unretractable.front() == original.finite.events.front().id);
    // Release the recorder before the borrowed native sound banks.
    fixture.audio.recordTo(nullptr);
    sounds.close();
    fixture.audio.close();
}
TEST_CASE("fixture aggregate rewinds complete delayed blast contacts without duplicate output",
          "[rollback][level-fixtures-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first;
    Fixture second;
    const auto empty = first.owner.checkpoint();
    REQUIRE(empty);
    first.fixtures.blast({0, 0, 0}, 12, 30, first.players, first.host);
    REQUIRE(first.hits.size() == 1);
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    REQUIRE(state->blasts.size() == 1);
    REQUIRE(state->blasts[0].players.size() == 1);
    const auto textures = second.device.texturesCreated;
    REQUIRE(second.owner.restore(*state));
    CHECK(second.hits.empty());
    CHECK(second.owner.checkpoint() == state);
    first.hits.clear();
    for (s32 tick = 0; tick < rate; ++tick) {
        first.fixtures.advanceBlasts(seconds, first.players, first.host);
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
    }
    REQUIRE(first.hits.size() == 1);
    CHECK(first.hits[0].first == 1);
    CHECK(second.device.texturesCreated == textures);
    auto undo = second.owner.prepareRestore(*empty);
    REQUIRE(undo);
    second.fixtures.blast({0, 0, 0}, 12, 30, second.players, second.host);
    const auto calls = second.hits.size();
    REQUIRE(second.owner.commitRestore(std::move(*undo)));
    CHECK(second.owner.checkpoint() == empty);
    CHECK(second.hits.size() == calls);
}
TEST_CASE("fixture aggregate preflights every child and invalidates stale siblings",
          "[rollback][level-fixtures-checkpoint]") {
    Fixture fixture;
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    for (const int mutation : {0, 1, 2, 3, 4}) {
        auto invalid = *state;
        switch (mutation) {
        case 0: invalid.traps.random = 0; break;
        case 1: invalid.rubble.resources.push_back({0, "NOT_BOUND"}); break;
        case 2: invalid.doomedChests.push_back(0); break;
        case 3:
            invalid.clouds.push_back({{0, 0, 0}, 1, std::numeric_limits<f32>::quiet_NaN(), 0});
            break;
        case 4: invalid.difficultyGain = 2; break;
        default: break;
        }
        CHECK_FALSE(fixture.owner.restore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
        CHECK(fixture.hits.empty());
    }
    auto stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    ChestsCheckpoint child{fixture.fixtures.chests()};
    REQUIRE(child.restore(state->chests));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*stale)));
    CHECK(fixture.owner.checkpoint() == state);
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    LevelFixturesCheckpoint sibling{fixture.fixtures};
    REQUIRE(sibling.bindResources());
    REQUIRE(sibling.restore(*state));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    fixture.bind();
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK(fixture.owner.checkpoint() == state);
}

TEST_CASE("fixture capture and full validation agree without trusting incoming child states",
          "[rollback][level-fixtures-checkpoint][checkpoint-capture-validation]") {
    Fixture fixture;
    fixture.fixtures.blast({0, 0, 0}, 12, 30, fixture.players, fixture.host);
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    REQUIRE(baseline->valid());
    LevelFixturesCheckpoint independent{fixture.fixtures};
    REQUIRE(independent.bindResources());
    CHECK(independent.checkpoint() == baseline);
    for (s32 mutation = 0; mutation < 7; ++mutation) {
        CAPTURE(mutation);
        auto invalid = *baseline;
        switch (mutation) {
        case 0:
            invalid.chests.chests.emplace_back().refusalLeft =
                std::numeric_limits<f32>::quiet_NaN();
            break;
        case 1:
            invalid.gates.gates.emplace_back().refusalLeft = std::numeric_limits<f32>::quiet_NaN();
            break;
        case 2: invalid.traps.random = 0; break;
        case 3: invalid.barrels.barrels.emplace_back(); break;
        case 4: invalid.rocks.rocks.emplace_back(); break;
        case 5: invalid.scenery.props.emplace_back(); break;
        case 6:
            invalid.rubble.pieces.push_back(
                {static_cast<u32>(invalid.rubble.resources.size()), Mat4{1}});
            break;
        default: break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.owner.checkpoint() == baseline);
    }
    const auto emitted = fixture.hits.size();
    CHECK(independent.checkpoint() == fixture.owner.checkpoint());
    CHECK(fixture.hits.size() == emitted);
}

TEST_CASE(
    "fixture cooldown validation accepts unordered bounded ledgers and rejects bad references",
    "[rollback][level-fixtures-checkpoint][fixture-timers]") {
    Fixture fixture;
    const auto empty = fixture.owner.checkpoint();
    REQUIRE(empty);
    auto state = *empty;
    LevelFixturesCheckpoint::Blast blast;
    blast.radius = 12;
    blast.damage = 30;
    blast.seconds = 3;
    blast.flags = 0x800;
    blast.started = true;
    blast.players = {3, 0, 2, 1};
    blast.playerReady = {{2, 0.5f}, {0, 0.75f}, {1, 0.0f}, {3, 1.0f}};
    // Preserve the actual aggregate contact bound, including non-contiguous
    // opponent identities and contact/timer orders that differ from each other.
    constexpr s32 kContacts = 65535;
    blast.opponents.reserve(kContacts);
    blast.opponentReady.reserve(kContacts);
    for (s32 index = 0; index < kContacts; ++index) {
        blast.opponents.push_back((kContacts - index) * 2);
        blast.opponentReady.emplace_back((index + 1) * 2, 0.5f);
    }
    state.blasts.push_back(std::move(blast));
    REQUIRE(state.valid());
    REQUIRE(fixture.owner.restore(state));
    CHECK(fixture.owner.checkpoint() == state);
    CHECK(fixture.hits.empty());
    for (const s32 mutation : {0, 1, 2, 3, 4, 5, 6, 7}) {
        CAPTURE(mutation);
        auto invalid = state;
        auto& ring = invalid.blasts[0];
        switch (mutation) {
        case 0: ring.opponentReady[0].first = 1; break; // Not an admitted contact.
        case 1: ring.opponentReady[1].first = ring.opponentReady[0].first; break;
        case 2: ring.opponentReady[0].second = std::numeric_limits<f32>::quiet_NaN(); break;
        case 3: ring.opponentReady[0].second = -0.1f; break;
        case 4: ring.playerReady[0].first = 4; break;
        case 5: ring.playerReady[1].first = ring.playerReady[0].first; break;
        case 6: ring.opponentReady.emplace_back(2, 0.5f); break;
        case 7: ring.reached.push_back(1); break; // Aggregate contact bound exceeded.
        default: break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
        CHECK(fixture.hits.empty());
    }
}

TEST_CASE("fixture poison cooldown replay preserves expiry order and repeated native hits",
          "[rollback][level-fixtures-checkpoint][fixture-timers]") {
    const s32 rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first;
    Fixture second;
    auto state = first.owner.checkpoint();
    REQUIRE(state);
    // This is an incoming authoritative poison-ring checkpoint. The production
    // blast phase below, not a test-side timer simulation, expires and refills it.
    LevelFixturesCheckpoint::Blast ring;
    ring.radius = 12;
    ring.damage = 30;
    ring.seconds = 3;
    ring.elapsed = 0.25f;
    ring.flags = 0x800;
    ring.started = true;
    ring.players = {0};
    ring.opponents = {17};
    ring.playerReady = {{0, 0.5f}};
    ring.opponentReady = {{17, 0.5f}};
    state->blasts.push_back(ring);
    REQUIRE(first.owner.restore(*state));
    REQUIRE(second.owner.restore(*state));
    const auto textures = second.device.texturesCreated;
    usize repeatedHits = 0;
    usize refreshedOpponents = 0;
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(rate, tick);
        first.hits.clear();
        second.hits.clear();
        const auto before = first.owner.checkpoint();
        REQUIRE(before);
        auto undo = second.owner.prepareRestore(*before);
        REQUIRE(undo);
        first.fixtures.advanceBlasts(seconds, first.players, first.host);
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
        repeatedHits += static_cast<usize>(
            std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first));
        const auto after = first.owner.checkpoint();
        REQUIRE(after);
        REQUIRE(after->blasts.size() == 1);
        REQUIRE(after->blasts[0].opponentReady.size() == 1);
        refreshedOpponents +=
            after->blasts[0].opponentReady[0].second > before->blasts[0].opponentReady[0].second
                ? 1U
                : 0U;
        if (tick + 1 < rate / 4) {
            CHECK(std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first) == 0);
        }
        const auto emitted = second.hits.size();
        REQUIRE(second.owner.commitRestore(std::move(*undo)));
        CHECK(second.owner.checkpoint() == before);
        CHECK(second.hits.size() == emitted); // Restore itself emits nothing.
        // The owning caller discards speculative output before replay instead
        // of delivering the same accepted tick twice.
        second.hits.clear();
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
    }
    CHECK(repeatedHits >= 2);
    CHECK(refreshedOpponents >= 2);
    CHECK(second.device.texturesCreated == textures);
}

TEST_CASE("native fixture aggregate retains all seven rosters and restores without uploads",
          "[rollback][level-fixtures-checkpoint][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto* const stage = GENERATE("G1", "F1", "J4");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    Fixture first;
    Fixture second;
    first.fixtures.clear();
    second.fixtures.clear();
    REQUIRE(first.world.load(first.device, root, *level));
    REQUIRE(second.world.load(second.device, root, *level));
    first.bind();
    second.bind();
    first.fixtures.setPlayerCount(3);
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    CHECK(state->chests.chests.size() == first.fixtures.chests().size());
    CHECK(state->gates.gates.size() == first.fixtures.gates().size());
    CHECK(state->traps.traps.size() == first.fixtures.traps().size());
    CHECK(state->barrels.barrels.size() == first.fixtures.barrels().size());
    CHECK(state->rocks.rocks.size() == first.fixtures.safeRocks().size());
    CHECK(state->scenery.props.size() == first.fixtures.scenery().size());
    CHECK(state->rubble.pieces.size() == first.fixtures.rubble().size());
    const auto textures = second.device.texturesCreated;
    const auto updates = second.device.textureUpdates;
    REQUIRE(second.owner.restore(*state));
    CHECK(second.owner.checkpoint() == state);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == updates);
    CHECK(second.hits.empty());
}
} // namespace
