#include <array>
#include <concepts>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCollision.h"

#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::equality_comparable<PlayerSimulation>);

template <typename Value, typename Mutate>
void expectDifferent(const Value& before, std::string_view field, Mutate mutate) {
    INFO(field);
    auto after = before;
    CHECK(after == before);
    mutate(after);
    CHECK_FALSE(after == before);
    CHECK_FALSE(before == after);
    const auto copied = after;
    CHECK(copied == after);
}

WorldCollision floorAndWall() {
    const auto triangle = [](Vec3 a, Vec3 b, Vec3 c, Vec3 normal, s32 object) {
        CollisionTriangle result;
        result.vertices = {a, b, c};
        result.normal = normal;
        result.object = object;
        return result;
    };
    WorldCollision result;
    result.build({triangle({-30, 0, -30}, {30, 0, -30}, {30, 0, 30}, {0, 1, 0}, 0),
                  triangle({-30, 0, -30}, {30, 0, 30}, {-30, 0, 30}, {0, 1, 0}, 0),
                  triangle({5, 0, -30}, {5, 10, -30}, {5, 10, 30}, {-1, 0, 0}, 1),
                  triangle({5, 0, -30}, {5, 10, 30}, {5, 0, 30}, {-1, 0, 0}, 1)});
    return result;
}

ClassStats turboStats() {
    ClassStats stats;
    stats.moves.turboB = 0;
    MoveStrike window;
    window.type = MoveStrike::kWindow;
    window.startFrame = 0;
    window.endFrame = 3;
    window.flags = MoveStrike::kHidesWeapon;
    window.help = 77;
    window.next = 1;
    MoveStrike volley;
    volley.type = MoveStrike::kVolley;
    volley.startFrame = 0;
    volley.endFrame = 6;
    volley.delay = 2;
    volley.angle = 1;
    volley.flags = MoveStrike::kSweepsIn;
    volley.next = 2;
    MoveStrike hit;
    hit.startFrame = 2;
    hit.amount = 5;
    stats.moveStrikes = {window, volley, hit};
    return stats;
}

TEST_CASE("native player equality includes every top-level decision timer and contact value",
          "[local-rewind][player-simulation-equality]") {
    const PlayerSimulation base;
    const auto change = [&](std::string_view field, auto mutate) {
        expectDifferent(base, field, mutate);
    };
    change("cursor aiming", [](auto& p) { p.cursorAiming = true; });
    change("melee facing presence", [](auto& p) { p.meleeFacing = Vec3{0}; });
    change("attack step presence", [](auto& p) { p.attackStep = PlayerSimulation::AttackStep{}; });
    change("fixture contact presence", [](auto& p) { p.fixtureSpot = Vec3{0}; });
    change("level kills", [](auto& p) { ++p.levelKills; });
    change("life", [](auto& p) { p.life = PlayerLife::Dying; });
    change("tower prompt", [](auto& p) { p.towerPrompt = true; });
    change("departure", [](auto& p) { p.departed = true; });
    change("fractional health", [](auto& p) { p.healthFraction = 0.125f; });
    change("pending pain", [](auto& p) { p.painOwed = 0.25f; });
    change("hit sound cooldown", [](auto& p) { ++p.hitSoundGap; });
    change("heartbeat cooldown", [](auto& p) { ++p.heartbeatTicks; });
    change("surface cooldown", [](auto& p) { p.surfaceGap = 0.25f; });
    change("hit flash", [](auto& p) { ++p.hitFlashTicks; });
    change("tag timer", [](auto& p) { ++p.itTicks; });
    change("gesture", [](auto& p) { p.gesture = PlayerDeed::Pick; });
    change("gag clock", [](auto& p) { p.gagSeconds = 0.25f; });
    change("shock history", [](auto& p) { p.shockGaps.push_back({4, 0.25f}); });
    change("held death", [](auto& p) { p.deathHeld = 4; });
    change("death drain clock", [](auto& p) { ++p.deathDrainTicks; });
    change("death halo cue", [](auto& p) { p.deathHaloHeard = true; });
    change("reaction", [](auto& p) { p.reaction = PlayerDeed::Flinch; });
    change("pojo turbo", [](auto& p) { p.pojoTurbo = true; });
    change("help history", [](auto& p) { p.helpHeard.push_back(5); });
    change("charge contacts", [](auto& p) { p.rammed.push_back(5); });
    change("block clock", [](auto& p) { p.blockLeft = 0.25f; });
    change("block latch", [](auto& p) { p.blocked = true; });
    change("gas cooldown", [](auto& p) { p.cloudGap = 0.25f; });
    change("breath cooldown", [](auto& p) { p.breathGap = 0.25f; });
    change("attached effect cooldown", [](auto& p) { p.effectGap = 0.25f; });
    change("floor identity", [](auto& p) { p.floor.object = 4; });
    change("floor flags", [](auto& p) { p.floor.flags = 8; });
    change("floor placement presence", [](auto& p) { p.floor.placement = Mat4{1}; });
    change("name clock", [](auto& p) { ++p.nameTicks; });
    change("previous powerup flags", [](auto& p) { p.wornSpecial = powerup::kLevitation; });
    change("result checkpoint presence", [](auto& p) { p.resultsCheckpoint.emplace(); });

    auto populated = base;
    populated.meleeFacing = Vec3{0};
    populated.attackStep = PlayerSimulation::AttackStep{};
    populated.fixtureSpot = Vec3{0};
    populated.floor.placement = Mat4{1};
    populated.shockGaps = {{4, 0.25f}, {6, 0.5f}};
    populated.helpHeard = {1, 2};
    populated.rammed = {1, 2};
    populated.resultsCheckpoint.emplace();
    const auto nested = [&](std::string_view field, auto mutate) {
        expectDifferent(populated, field, mutate);
    };
    nested("melee direction", [](auto& p) { p.meleeFacing->x = 1; });
    nested("attack segment start", [](auto& p) { p.attackStep->from.y = 1; });
    nested("attack segment end", [](auto& p) { p.attackStep->to.z = 1; });
    nested("fixture position", [](auto& p) { p.fixtureSpot->y = 1; });
    nested("floor transform", [](auto& p) { (*p.floor.placement)[3].y = 2; });
    nested("shock identity", [](auto& p) { ++p.shockGaps[0].target; });
    nested("shock cooldown", [](auto& p) { p.shockGaps[1].seconds += 0.125f; });
    nested("shock order", [](auto& p) { std::swap(p.shockGaps[0], p.shockGaps[1]); });
    nested("help order", [](auto& p) { std::swap(p.helpHeard[0], p.helpHeard[1]); });
    nested("charge contact order", [](auto& p) { std::swap(p.rammed[0], p.rammed[1]); });
    nested("linked level gold", [](auto& p) { ++p.resultsCheckpoint->gold; });
    nested("linked level experience", [](auto& p) { ++p.resultsCheckpoint->experience; });
    nested("linked level kills", [](auto& p) { ++p.resultsCheckpoint->kills; });

    const ComboState combo;
    expectDifferent(combo, "combo role", [](auto& c) { c.role = ComboRole::Held; });
    expectDifferent(combo, "combo partner", [](auto& c) { c.partner = 1; });
    expectDifferent(combo, "grabber class", [](auto& c) { c.grabberClass = 1; });
    expectDifferent(combo, "riding latch", [](auto& c) { c.riding = true; });
    expectDifferent(combo, "ride request", [](auto& c) { c.rideAsked = true; });
    expectDifferent(combo, "flight clock", [](auto& c) { ++c.ticksLeft; });
    expectDifferent(combo, "turn clock", [](auto& c) { ++c.turnTicks; });
    expectDifferent(combo, "thrower grace", [](auto& c) { c.graceSeconds = 0.25f; });
    expectDifferent(combo, "saved combo position", [](auto& c) { c.saved.z = 1; });
    change("combo owner", [](auto& p) { p.combo.role = ComboRole::Thrown; });
}

TEST_CASE("native player equality checks complete live and entry saves including inactive classes",
          "[local-rewind][player-simulation-equality]") {
    const PlayerSimulation base;
    for (const bool entry : {false, true}) {
        CAPTURE(entry);
        const auto saveChange = [&](std::string_view field, auto mutate) {
            expectDifferent(base, field,
                            [&](auto& p) { mutate(entry ? p.entrySave : p.actor.save()); });
        };
        saveChange("name", [](auto& s) { s.name = "TEST"; });
        saveChange("selected class", [](auto& s) { s.character = 1; });
        saveChange("color", [](auto& s) { s.color = 1; });
        saveChange("class unlocks", [](auto& s) { s.classUnlock = 1; });
        saveChange("live gold", [](auto& s) { ++s.gold; });
        saveChange("total levels", [](auto& s) { ++s.levelTotal; });
        saveChange("auto attack", [](auto& s) { s.autoAttack = false; });
        saveChange("saved help", [](auto& s) { s.helpSeen.push_back(7); });
        saveChange("saved movies", [](auto& s) { s.moviesSeen.push_back("INTRO"); });
        for (usize index = 0; index < kClassCount; ++index) {
            CAPTURE(index);
            const auto progressChange = [&](std::string_view field, auto mutate) {
                saveChange(field, [&](auto& s) { mutate(s.classes[index]); });
            };
            progressChange("experience", [](auto& p) { ++p.experience; });
            progressChange("banked gold", [](auto& p) { ++p.gold; });
            progressChange("promotion", [](auto& p) { ++p.promotedLevel; });
            progressChange("health", [](auto& p) { ++p.health; });
            progressChange("fight bonus", [](auto& p) { p.fightAdd = 0.25f; });
            progressChange("armor bonus", [](auto& p) { p.armorAdd = 0.25f; });
            progressChange("magic bonus", [](auto& p) { p.magicAdd = 0.25f; });
            progressChange("speed bonus", [](auto& p) { p.speedAdd = 0.25f; });
            progressChange("realm unlocks", [](auto& p) { p.unlocked = 1; });
            progressChange("keys", [](auto& p) { ++p.inventory.keys; });
            progressChange("potions", [](auto& p) { p.inventory.potions.push_back(2); });
            progressChange("permanent Pojo", [](auto& p) { p.inventory.permanentPojo = true; });
            for (usize slot = 0; slot < Inventory::kPowerupSlots; ++slot) {
                CAPTURE(slot);
                progressChange("powerup strength",
                               [slot](auto& p) { p.inventory.powerups[slot].strength = 0.25f; });
                progressChange("powerup kind",
                               [slot](auto& p) { ++p.inventory.powerups[slot].kind; });
                progressChange("powerup charge",
                               [slot](auto& p) { p.inventory.powerups[slot].charge = 0.25f; });
                progressChange("powerup flags",
                               [slot](auto& p) { p.inventory.powerups[slot].flags = 4; });
                progressChange("powerup enabled",
                               [slot](auto& p) { p.inventory.powerups[slot].on = false; });
            }
            progressChange("runes", [](auto& p) { p.relics.runes = 1; });
            progressChange("legends", [](auto& p) { p.relics.legends = 1; });
            progressChange("shards", [](auto& p) { p.relics.shards = 1; });
            progressChange("pending runes", [](auto& p) { p.relics.pendingRunes = 1; });
            progressChange("pending shards", [](auto& p) { p.relics.pendingShards = 1; });
            progressChange("pending ceremonies", [](auto& p) { p.relics.pendingCeremonies = 1; });
            for (usize kind = 0; kind < Relics::kGargoyleKinds; ++kind) {
                progressChange("gargoyle pieces",
                               [kind](auto& p) { ++p.relics.gargoylePieces[kind]; });
            }
            for (usize realm = 0; realm < kRealmCount; ++realm) {
                progressChange("realm crystal counts", [realm](auto& p) { ++p.crystals[realm]; });
                progressChange("beaten levels", [realm](auto& p) { ++p.levels.beaten[realm]; });
            }
            for (usize pass = 0; pass < LevelRecord::kPasses; ++pass) {
                progressChange("rune level record",
                               [pass](auto& p) { ++p.levels.runeLevels[pass]; });
                progressChange("legend level record",
                               [pass](auto& p) { ++p.levels.legendLevels[pass]; });
                progressChange("boss death record",
                               [pass](auto& p) { ++p.levels.bossDeaths[pass]; });
            }
            progressChange("lifetime enemy kills", [](auto& p) { ++p.lifetime.enemiesKilled; });
            progressChange("lifetime generators",
                           [](auto& p) { ++p.lifetime.generatorsDestroyed; });
            progressChange("lifetime gold", [](auto& p) { ++p.lifetime.goldFound; });
            progressChange("lifetime time", [](auto& p) { p.lifetime.playSeconds = 0.125; });
        }
    }
}

TEST_CASE("native player equality detects actor envelope contacts and private action clocks",
          "[local-rewind][player-simulation-equality]") {
    const PlayerSimulation base;
    const auto change = [&](std::string_view field, auto mutate) {
        expectDifferent(base, field, mutate);
    };
    change("input seat", [](auto& p) { p.actor.spawn(2, {}, nullptr, Vec3{0}, 0); });
    change("body position", [](auto& p) { p.actor.place({0, 1, 0}); });
    change("body yaw", [](auto& p) { p.actor.turnTo(0.25f); });
    change("pace bonus", [](auto& p) { p.actor.setPaceBonus(0.25f); });
    for (const auto member : {&ClassStats::speedMin, &ClassStats::width, &ClassStats::height,
                              &ClassStats::collisionY, &ClassStats::attentionY}) {
        change("native class envelope", [&](auto& p) {
            ClassStats stats;
            stats.*member = 10;
            p.actor.spawn(0, {}, &stats, Vec3{0}, 0);
        });
    }
    change("moving latch", [](auto& p) {
        p.actor.update({Vec2{0, 1}, 1}, 0, 0.01f, nullptr);
        p.actor.place(Vec3{0});
    });
    const auto collision = floorAndWall();
    auto touching = base;
    touching.actor.slide({8, 0, 0}, &collision);
    REQUIRE_FALSE(touching.actor.wallContacts().empty());
    expectDifferent(touching, "actual wall contact history",
                    [](auto& p) { p.actor.clearWallContacts(); });
    const WallContact wall{2, {1, 2, 3}};
    expectDifferent(wall, "wall object", [](auto& w) { ++w.object; });
    expectDifferent(wall, "wall point", [](auto& w) { ++w.point.y; });

    change("capture owner", [](auto& p) {
        auto actor = p.actor;
        p.capture.attach(4, false, Mat4{1}, actor);
    });
    PlayerCapture held;
    PlayerActor actor;
    held.attach(4, false, Mat4{1}, actor);
    expectDifferent(held, "capture boss flag", [](auto& c) {
        PlayerActor other;
        c.attach(4, true, Mat4{1}, other);
    });
    expectDifferent(held, "capture contact matrix", [](auto& c) {
        PlayerActor other;
        c.attach(4, false, glm::translate(Mat4{1}, Vec3{1, 0, 0}), other);
    });
    PlayerCapture higher;
    PlayerActor higherActor;
    higherActor.place({0, 2, 0});
    higher.attach(4, false, Mat4{1}, higherActor);
    REQUIRE(held.body() == higher.body());
    CHECK_FALSE(held == higher); // same pose, different pre-grab floor envelope
    auto thrown = held;
    thrown.release({1, 0, 0}, 2);
    auto harder = held;
    harder.release({1, 0, 0}, 3);
    CHECK_FALSE(thrown == harder);
    auto faster = held;
    faster.release({2, 0, 0}, 2);
    CHECK_FALSE(thrown == faster);

    change("transport destination and latch", [](auto& p) {
        REQUIRE(p.transport.begin({3, 0, 0}));
        p.transport.cancel();
    });
    PlayerTransport transport;
    REQUIRE(transport.begin({3, 0, 0}));
    expectDifferent(transport, "transport clock", [](auto& t) { t.update(1); });
    transport.cancel();
    expectDifferent(transport, "transport rearm latch", [](auto& t) { t.observeContact(false); });
    auto destination = transport;
    destination.observeContact(false);
    REQUIRE(destination.begin({4, 0, 0}));
    destination.cancel();
    CHECK_FALSE(destination == transport);

    change("decoy fractional clock", [](auto& p) {
        Inventory inventory;
        p.mikey.update(1.0f / 120.0f, inventory, Vec3{0});
        CHECK_FALSE(p.mikey.shown());
    });
    auto decoy = base;
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 60);
    inventory.powerups[0].on = true;
    decoy.mikey.update(1.0f / 30.0f, inventory, Vec3{0});
    REQUIRE(decoy.mikey.shown());
    REQUIRE(decoy.mikey.generation() > 0);
    decoy.mikey.clear();
    CHECK_FALSE(decoy == base); // generation survives clearing the visible decoy

    change("pending knockback force", [](auto& p) { p.knockback.queue({1, 0, 0}, 0, 0); });
    change("pending knockback flags",
           [](auto& p) { p.knockback.queue(Vec3{0}, Knockback::kKnockBack, 0); });
    change("pending knockback damage", [](auto& p) { p.knockback.queue(Vec3{0}, 0, 1); });
    change("sliding velocity", [](auto& p) { p.knockback.addVelocity({1, 0, 0}); });
    change("knockback fractional frame", [](auto& p) { p.knockback.step(1.0f / 120.0f, 1); });
    change("shove latch", [](auto& p) { p.knockback.shove(Vec3{0}, 1.0f / 30.0f); });
    change("previous shove latch", [](auto& p) {
        p.knockback.shove(Vec3{0}, 1.0f / 30.0f);
        p.knockback.endFrame();
    });
    Knockback fast;
    fast.queue(Vec3{0}, Knockback::kKnockDown, 2);
    fast.kick(0, false);
    CHECK(fast.velocity() == Vec3{0});
    CHECK_FALSE(fast == Knockback{}); // no force, but the fast-slide interval still runs
    change("held turbo", [](auto& p) { p.turbo.add(40); });
    change("turbo flash clock", [](auto& p) { p.turbo.step(1); });
    TurboMeter full;
    full.add(100);
    expectDifferent(full, "shown turbo", [](auto& t) { t.step(1); });
    full.step(100);
    expectDifferent(full, "full turbo glow phase", [](auto& t) { t.step(1); });
    change("head gem", [](auto& p) { p.gem.update(powerup::kHandOfDeath); });
    HeadGem gem;
    gem.update(powerup::kHandOfDeath);
    gem.update(powerup::kHealthVamp);
    HeadGem onlyVamp;
    onlyVamp.update(powerup::kHealthVamp);
    CHECK_FALSE(gem == onlyVamp); // same shown gem, different transition latches
    change("melee streak", [](auto& p) { p.streak.record(false); });
    MeleeStreak streak;
    streak.record(false);
    expectDifferent(streak, "melee quiet clock", [](auto& s) { s.step(0.25f); });
    change("exit wait clock", [](auto& p) { p.exitWait.step(true, 1); });

    const auto stats = turboStats();
    auto attacking = base;
    attacking.turbo.add(100);
    const auto beforeMove = attacking;
    REQUIRE(
        attacking.move.begin(PlayerAnimator::Action::TurboStrong, &stats, attacking.turbo).empty());
    CHECK_FALSE(attacking == beforeMove);
    REQUIRE(attacking.turbo == beforeMove.turbo);
    const auto pending = attacking;
    const TurboMove::Events ignore{.announce = [](s32) {},
                                   .dim = [](f32) {},
                                   .volley = [](const Vec3&) {},
                                   .strike = [](s32) {}};
    attacking.move.advance(PlayerAnimator::Action::TurboStrong, 0, {0, 0, 1}, &stats,
                           attacking.turbo, ignore);
    CHECK_FALSE(attacking == pending);
    REQUIRE(attacking.turbo == pending.turbo);
    REQUIRE(attacking.move.weaponHidden());
    const auto firstShot = attacking;
    attacking.move.advance(PlayerAnimator::Action::TurboStrong, 1, {0, 0, 1}, &stats,
                           attacking.turbo, ignore);
    CHECK_FALSE(attacking == firstShot);
    REQUIRE(attacking.turbo == firstShot.turbo);
}

TEST_CASE("native player value equality verifies one through eight corrected ticks directly",
          "[local-rewind][player-simulation-equality]") {
    constexpr f32 kDt = 1.0f / 60.0f;
    const auto collision = floorAndWall();
    const auto stats = turboStats();
    PlayerSimulation player;
    player.actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    player.entrySave.name = "ENTRY";
    player.entrySave.classes.back().lifetime.goldFound = 9;
    player.actor.save().classes.back().lifetime.playSeconds = 17.25;
    player.resultsCheckpoint = LevelResults::Checkpoint{12, 34, 56};
    player.capture.attach(4, true, glm::translate(Mat4{1}, Vec3{0, 4, 0}), player.actor);
    player.capture.release({1, -1, 0}, 12.5f);
    REQUIRE(player.transport.begin({3, 0, 0}));
    player.transport.update(12);
    player.combo = {ComboRole::Thrown, 1, ComboMove::kWarrior, false, false, 120, 4, 0.5f, Vec3{0}};
    player.turbo.add(100);
    player.move.begin(PlayerAnimator::Action::TurboStrong, &stats, player.turbo);
    player.knockback.shove({0.25f, 0, 0}, kDt);
    player.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 60);
    player.actor.save().progress().inventory.powerups[0].on = true;
    const auto saved = player;
    struct Output {
        std::optional<f32> landingDamage;
        std::optional<Vec3> transported;
        std::vector<TurboMove::Strike> strikes;
        std::vector<TurboMove::Volley> volleys;
        std::vector<s32> announcements;
        bool operator==(const Output&) const = default;
    };
    const auto step = [&](s32 tick, bool wrong) {
        Output result;
        player.cursorAiming = !wrong;
        player.meleeFacing = wrong ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
        const auto from = player.actor.position();
        result.landingDamage = player.capture.update(kDt, player.actor, collision);
        result.transported = player.transport.update(1);
        if (result.transported) {
            player.actor.place(*result.transported);
        }
        player.actor.slide(player.knockback.step(kDt, 1), &collision);
        player.attackStep = PlayerSimulation::AttackStep{from, player.actor.position()};
        ComboMove::tick(player.combo, 1, kDt);
        player.mikey.update(kDt, player.actor.save().progress().inventory, player.actor.position());
        player.turbo.step(1);
        player.move.advance(
            PlayerAnimator::Action::TurboStrong, static_cast<f32>(tick) * 0.5f, *player.meleeFacing,
            &stats, player.turbo,
            {.announce = [&](s32 id) { result.announcements.push_back(id); },
             .identifiedVolley = [&](const auto& shot) { result.volleys.push_back(shot); },
             .identifiedStrike = [&](const auto& hit) { result.strikes.push_back(hit); }});
        player.shockGaps.push_back({tick, wrong ? 0.75f : 0.25f});
        player.streak.record(wrong);
        player.streak.step(kDt);
        player.exitWait.step(true, 1);
        player.actor.save().classes.back().lifetime.playSeconds += kDt;
        player.entrySave.classes.back().lifetime.goldFound += wrong ? 2 : 1;
        player.resultsCheckpoint->kills += wrong ? 2 : 1;
        return result;
    };
    std::array<PlayerSimulation, 8> expected;
    std::array<Output, 8> outputs;
    for (s32 tick = 0; tick < 8; ++tick) {
        outputs[tick] = step(tick, false);
        expected[tick] = player;
    }
    REQUIRE(player.mikey.shown());
    REQUIRE(player.mikey.generation() > 0);
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        player = saved;
        for (s32 tick = 0; tick < distance; ++tick) {
            step(tick, true);
        }
        REQUIRE_FALSE(player == expected[distance - 1]);
        player = saved;
        REQUIRE(player == saved);
        for (s32 tick = 0; tick < distance; ++tick) {
            CAPTURE(tick);
            CHECK(step(tick, false) == outputs[tick]);
            CHECK(player == expected[tick]);
        }
    }
}

TEST_CASE("player simulation comparison excludes runtime bindings and render-only history",
          "[local-rewind][player-simulation-equality]") {
    PlayerRuntime first;
    PlayerRuntime second;
    REQUIRE(first.simulation() == second.simulation());
    first.previous.position = {4, 5, 6};
    first.previous.yaw = 1;
    first.previous.continuous = true;
    first.previous.animationRevision = 9;
    first.slot = 4;
    first.deathHeldEffect = 5;
    first.deathHeldCry = 7;
    first.deathHeldSuck = 8;
    CHECK(first.simulation() == second.simulation());
    first.cursorAiming = true;
    CHECK_FALSE(first.simulation() == second.simulation());
}
} // namespace
