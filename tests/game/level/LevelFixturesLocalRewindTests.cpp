#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/level/FixtureHost.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelFixtures.h"
#include "game/level/NativeStateResources.h"
#include "game/level/ScriptedFixtures.h"
#include "game/level/StageRegistry.h"

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
    void bind() { fixtures.bind({device, world, weapons, effects, audio, 1}); }
    /** The fixtures' native digest over their own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() {
        NativeStateResources resources;
        test::registerStage(resources, world, device);
        if (weapons.loaded()) {
            REQUIRE(resources.registerArchive(weapons, "weapons"));
        }
        return test::liveDigest(fixtures, resources);
    }
};

/** A copy of the blasts under way, to compare once the fixtures have moved on. */
std::vector<LevelFixtures::Blast> blastsOf(const LevelFixtures& fixtures) {
    const auto blasts = fixtures.blasts();
    return {blasts.begin(), blasts.end()};
}

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
        for (const auto& blast : fixture.fixtures.blasts()) {
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
        replay.fixtures.saveLocal(observed);
        REQUIRE_FALSE(replay.fixtures.equalLocal(observed, history.fixtures));
        const auto rejected = actual;
        const auto uploads = replay.device.texturesCreated;
        history.restore(replay);
        CHECK(actual == rejected); // Rewinding does not emit or clear caller-owned output.
        replay.fixtures.saveLocal(observed);
        CHECK(replay.fixtures.equalLocal(observed, history.fixtures));
        CHECK(replay.device.texturesCreated == uploads);
        actual = {};
        for (s32 i = 0; i < length; ++i) {
            step(replay, actualHost, frame + i);
            replay.fixtures.saveLocal(observed);
            CHECK(replay.fixtures.equalLocal(observed, expectedLocal[static_cast<usize>(i)]));
        }
        CHECK(replay.digest() == uninterrupted.digest());
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
    LevelFixtures::LocalSnapshot untouched;
    other.fixtures.saveLocal(untouched);
    CHECK_FALSE(other.fixtures.canRestoreLocal(saved));
    CHECK_FALSE(other.fixtures.equalLocal(saved, saved));
    CHECK_FALSE(other.fixtures.restoreLocal(saved));
    LevelFixtures::LocalSnapshot refusedOther;
    other.fixtures.saveLocal(refusedOther);
    CHECK(other.fixtures.equalLocal(refusedOther, untouched));
    source.fixtures.blast({0, 0, 0}, 12, 30, source.players, source.host);
    const auto advanced = blastsOf(source.fixtures);
    REQUIRE(advanced.size() == 1);
    Chests::LocalSnapshot chests;
    source.fixtures.chests().saveLocal(chests);
    source.fixtures.gates().clear();
    CHECK_FALSE(source.fixtures.equalLocal(saved, saved));
    CHECK_FALSE(source.fixtures.canRestoreLocal(saved));
    CHECK_FALSE(source.fixtures.restoreLocal(saved));
    CHECK(blastsOf(source.fixtures) == advanced);
    Chests::LocalSnapshot refusedChests;
    source.fixtures.chests().saveLocal(refusedChests);
    CHECK(source.fixtures.chests().equalLocal(refusedChests, chests));
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
    const auto firstBlasts = blastsOf(fixture.fixtures);
    REQUIRE(firstBlasts.size() == 1);
    REQUIRE(fixture.fixtures.restoreLocal(initial));
    hit(31);
    fixture.fixtures.saveLocal(second);
    const auto secondBlasts = blastsOf(fixture.fixtures);
    REQUIRE(secondBlasts.size() == 1);
    CHECK(firstBlasts.front().position == secondBlasts.front().position);
    CHECK(firstBlasts.front().elapsed == secondBlasts.front().elapsed);
    CHECK(firstBlasts.front().opponents.size() == secondBlasts.front().opponents.size());
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
        CHECK(replay.digest() == uninterrupted.digest());
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
        sawRubble |= uninterrupted.fixtures.rubble().size() > 0;
        sawReached |= std::ranges::any_of(uninterrupted.fixtures.blasts(),
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
          "[rollback][level-fixtures-local-rewind]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first;
    Fixture second;
    LevelFixtures::LocalSnapshot empty;
    second.fixtures.saveLocal(empty);
    const auto emptyDigest = second.digest();
    first.fixtures.blast({0, 0, 0}, 12, 30, first.players, first.host);
    REQUIRE(first.hits.size() == 1);
    const auto blasts = blastsOf(first.fixtures);
    REQUIRE(blasts.size() == 1);
    REQUIRE(blasts[0].players.size() == 1);
    // The second takes the same blast, then a wrong branch that reaches no player (whose own
    // state this owner does not rewind), and rewinds to the blast.
    second.fixtures.blast({0, 0, 0}, 12, 30, second.players, second.host);
    LevelFixtures::LocalSnapshot blasted;
    second.fixtures.saveLocal(blasted);
    second.fixtures.blast({6, 0, 0}, 24, 80, {}, second.host);
    second.fixtures.advanceBlasts(seconds * 2, {}, second.host);
    LevelFixtures::LocalSnapshot wrong;
    second.fixtures.saveLocal(wrong);
    REQUIRE_FALSE(second.fixtures.equalLocal(wrong, blasted));
    const auto textures = second.device.texturesCreated;
    const auto emitted = second.hits.size();
    REQUIRE(second.fixtures.restoreLocal(blasted));
    CHECK(second.hits.size() == emitted); // Restore itself emits nothing.
    second.hits.clear();
    CHECK(second.digest() == first.digest());
    first.hits.clear();
    for (s32 tick = 0; tick < rate; ++tick) {
        first.fixtures.advanceBlasts(seconds, first.players, first.host);
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.digest() == first.digest());
        CHECK(second.hits == first.hits);
    }
    REQUIRE(first.hits.size() == 1);
    CHECK(first.hits[0].first == 1);
    CHECK(second.device.texturesCreated == textures);
    second.fixtures.blast({0, 0, 0}, 12, 30, second.players, second.host);
    const auto calls = second.hits.size();
    REQUIRE(second.fixtures.restoreLocal(empty));
    CHECK(second.digest() == emptyDigest);
    CHECK(second.hits.size() == calls);
}

TEST_CASE("fixture poison cooldown replay preserves expiry order and repeated native hits",
          "[rollback][level-fixtures-local-rewind][fixture-timers]") {
    const s32 rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    // A realm whose world explosions are gas rings, the ring living as long as its effect.
    constexpr s32 kGasRealm = 11;
    const auto root = test::sampleLevel("fixture-poison-ring");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"WORLD_EXP","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"BURST","frames":90,"frameRate":30}]}]})");
    writeTextFile(root / "world.json", R"({"objects":[
      {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
    const LevelRef level{"TEST", kGasRealm, "TEST", "", "", "", "", 0};
    Fixture first;
    Fixture second;
    // The production explosion, not a test-side ring, starts the poison; its first step
    // reaches player 0 and opponent 17, whose next contacts wait half a second.
    for (Fixture* fixture : {&first, &second}) {
        fixture->fixtures.clear();
        REQUIRE(fixture->world.load(fixture->device, root, level));
        fixture->bind();
        fixture->fixtures.worldExplosion({0, 0, 0}, fixture->players, fixture->host);
        const auto rings = fixture->fixtures.blasts();
        REQUIRE(rings.size() == 1);
        REQUIRE((rings[0].flags & damage::kGas) != 0);
        REQUIRE(rings[0].seconds > LevelFixtures::kExplosionSeconds);
        for (s32 tick = 0; tick < rate / 4; ++tick) {
            fixture->fixtures.advanceBlasts(seconds, fixture->players, fixture->host);
        }
    }
    const auto textures = second.device.texturesCreated;
    usize repeatedHits = 0;
    usize refreshedOpponents = 0;
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(rate, tick);
        first.hits.clear();
        second.hits.clear();
        const auto before = blastsOf(first.fixtures);
        const auto beforeDigest = first.digest();
        LevelFixtures::LocalSnapshot undo;
        second.fixtures.saveLocal(undo);
        first.fixtures.advanceBlasts(seconds, first.players, first.host);
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.digest() == first.digest());
        CHECK(second.hits == first.hits);
        repeatedHits += static_cast<usize>(
            std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first));
        const auto after = first.fixtures.blasts();
        REQUIRE(after.size() == 1);
        REQUIRE(after[0].opponentReady.size() == 1);
        refreshedOpponents +=
            after[0].opponentReady[0].second > before[0].opponentReady[0].second ? 1U : 0U;
        if (tick + 1 < rate / 4) {
            CHECK(std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first) == 0);
        }
        const auto emitted = second.hits.size();
        REQUIRE(second.fixtures.restoreLocal(undo));
        CHECK(second.digest() == beforeDigest);
        CHECK(second.hits.size() == emitted); // Restore itself emits nothing.
        // The owning caller discards speculative output before replay instead
        // of delivering the same accepted tick twice.
        second.hits.clear();
        second.fixtures.advanceBlasts(seconds, second.players, second.host);
        CHECK(second.digest() == first.digest());
        CHECK(second.hits == first.hits);
    }
    CHECK(repeatedHits >= 2);
    CHECK(refreshedOpponents >= 2);
    CHECK(second.device.texturesCreated == textures);
}

TEST_CASE("native fixture aggregate retains all seven rosters and restores without uploads",
          "[rollback][level-fixtures-local-rewind][assets]") {
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
    LevelFixtures::LocalSnapshot entry;
    first.fixtures.saveLocal(entry);
    first.fixtures.setPlayerCount(3);
    second.fixtures.setPlayerCount(3);
    LevelFixtures::LocalSnapshot three;
    first.fixtures.saveLocal(three);
    const auto state = first.digest();
    CHECK(second.digest() == state);
    const auto textures = first.device.texturesCreated;
    const auto updates = first.device.textureUpdates;
    LevelFixtures::LocalSnapshot restored;
    REQUIRE(first.fixtures.restoreLocal(entry));
    first.fixtures.saveLocal(restored);
    CHECK(first.fixtures.equalLocal(restored, entry));
    REQUIRE(first.fixtures.restoreLocal(three));
    first.fixtures.saveLocal(restored);
    CHECK(first.fixtures.equalLocal(restored, three));
    CHECK(first.digest() == state);
    CHECK(first.device.texturesCreated == textures);
    CHECK(first.device.textureUpdates == updates);
    CHECK(first.hits.empty());
}
} // namespace
