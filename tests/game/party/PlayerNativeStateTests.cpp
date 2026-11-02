#include <array>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"
#include "engine/world/WorldCollision.h"

#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class T> NativeStateDigest::Digest nativeDigest(const T& value) {
    NativeStateDigest digest;
    digest.value(value);
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

template <class T, class Mutate>
void expectChanged(const T& before, std::string_view field, Mutate mutate) {
    INFO(field);
    const auto original = nativeDigest(before);
    auto after = before;
    REQUIRE(nativeDigest(after) == original);
    mutate(after);
    REQUIRE_FALSE(after == before);
    CHECK(nativeDigest(after) != original);
    CHECK(nativeDigest(before) == original); // traversal is read-only
    after = before; // actual in-memory rewind; no diagnostic checkpoint projection
    CHECK(nativeDigest(after) == original);
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

TEST_CASE("native player digest includes latent decisions and contact cooldowns",
          "[native-state-digest][player-native-state]") {
    const PlayerSimulation base;
    const auto change = [&](std::string_view field, auto mutate) {
        expectChanged(base, field, mutate);
    };
    change("surface cooldown", [](auto& p) { p.surfaceGap = 0.25f; });
    auto waitingForSurface = base;
    waitingForSurface.surfaceGap = 0.25f;
    REQUIRE(waitingForSurface.actor == base.actor);
    CHECK(nativeDigest(waitingForSurface) != nativeDigest(base));

    change("cursor aiming", [](auto& p) { p.cursorAiming = true; });
    change("melee direction presence", [](auto& p) { p.meleeFacing = Vec3{0}; });
    change("attack contact presence",
           [](auto& p) { p.attackStep = PlayerSimulation::AttackStep{}; });
    change("linked level baseline", [](auto& p) { p.resultsCheckpoint.emplace(); });
    change("level kills", [](auto& p) { ++p.levelKills; });
    change("life", [](auto& p) { p.life = PlayerLife::Dying; });
    change("tower prompt", [](auto& p) { p.towerPrompt = true; });
    change("departed", [](auto& p) { p.departed = true; });
    change("fractional health", [](auto& p) { p.healthFraction = 0.125f; });
    change("pending pain", [](auto& p) { p.painOwed = 0.25f; });
    change("hit sound gap", [](auto& p) { ++p.hitSoundGap; });
    change("heartbeat clock", [](auto& p) { ++p.heartbeatTicks; });
    change("fixture contact presence", [](auto& p) { p.fixtureSpot = Vec3{0}; });
    change("hit flash clock", [](auto& p) { ++p.hitFlashTicks; });
    change("tag clock", [](auto& p) { ++p.itTicks; });
    change("gesture", [](auto& p) { p.gesture = PlayerDeed::Pick; });
    change("gag clock", [](auto& p) { p.gagSeconds = 0.25f; });
    change("shock contact", [](auto& p) { p.shockGaps.push_back({4, 0.25f}); });
    change("held death", [](auto& p) { p.deathHeld = 4; });
    change("death drain clock", [](auto& p) { ++p.deathDrainTicks; });
    change("death halo latch", [](auto& p) { p.deathHaloHeard = true; });
    change("reaction", [](auto& p) { p.reaction = PlayerDeed::Flinch; });
    change("Pojo turbo", [](auto& p) { p.pojoTurbo = true; });
    change("help history", [](auto& p) { p.helpHeard.push_back(5); });
    change("charge contact", [](auto& p) { p.rammed.push_back(5); });
    change("block clock", [](auto& p) { p.blockLeft = 0.25f; });
    change("block latch", [](auto& p) { p.blocked = true; });
    change("cloud cooldown", [](auto& p) { p.cloudGap = 0.25f; });
    change("breath cooldown", [](auto& p) { p.breathGap = 0.25f; });
    change("effect cooldown", [](auto& p) { p.effectGap = 0.25f; });
    change("floor object", [](auto& p) { ++p.floor.object; });
    change("floor flags", [](auto& p) { ++p.floor.flags; });
    change("floor placement presence", [](auto& p) { p.floor.placement = Mat4{1}; });
    change("name clock", [](auto& p) { ++p.nameTicks; });
    change("previous powerup flags", [](auto& p) { p.wornSpecial = powerup::kLevitation; });

    auto contacts = base;
    contacts.meleeFacing = Vec3{0};
    contacts.attackStep = PlayerSimulation::AttackStep{};
    contacts.resultsCheckpoint.emplace();
    contacts.fixtureSpot = Vec3{0};
    contacts.floor.placement = Mat4{1};
    contacts.shockGaps = {{1, 0.25f}, {2, 0.5f}};
    contacts.helpHeard = {1, 2};
    contacts.rammed = {1, 2};
    const auto contact = [&](std::string_view field, auto mutate) {
        expectChanged(contacts, field, mutate);
    };
    contact("melee direction", [](auto& p) { p.meleeFacing->x = 1; });
    contact("attack start", [](auto& p) { p.attackStep->from.y = 1; });
    contact("attack end", [](auto& p) { p.attackStep->to.z = 1; });
    contact("fixture contact", [](auto& p) { p.fixtureSpot->y = 1; });
    contact("floor transform", [](auto& p) { (*p.floor.placement)[3].y = 2; });
    contact("shock identity", [](auto& p) { ++p.shockGaps[0].target; });
    contact("shock cooldown", [](auto& p) { p.shockGaps[1].seconds += 0.125f; });
    contact("shock order", [](auto& p) { std::swap(p.shockGaps[0], p.shockGaps[1]); });
    contact("help order", [](auto& p) { std::swap(p.helpHeard[0], p.helpHeard[1]); });
    contact("charge order", [](auto& p) { std::swap(p.rammed[0], p.rammed[1]); });
    contact("linked gold", [](auto& p) { ++p.resultsCheckpoint->gold; });
    contact("linked experience", [](auto& p) { ++p.resultsCheckpoint->experience; });
    contact("linked kills", [](auto& p) { ++p.resultsCheckpoint->kills; });

    change("combo role", [](auto& p) { p.combo.role = ComboRole::Held; });
    change("combo partner", [](auto& p) { p.combo.partner = 1; });
    change("combo class", [](auto& p) { p.combo.grabberClass = 1; });
    change("combo riding", [](auto& p) { p.combo.riding = true; });
    change("combo request", [](auto& p) { p.combo.rideAsked = true; });
    change("combo duration", [](auto& p) { ++p.combo.ticksLeft; });
    change("combo turn clock", [](auto& p) { ++p.combo.turnTicks; });
    change("combo grace", [](auto& p) { p.combo.graceSeconds = 0.25f; });
    change("combo position", [](auto& p) { p.combo.saved.z = 1; });
}

TEST_CASE("native player digest includes all inactive classes and the untouched entry save",
          "[native-state-digest][player-native-state]") {
    const PlayerSimulation base;
    for (const bool entry : {false, true}) {
        CAPTURE(entry);
        const auto saveChange = [&](std::string_view field, auto mutate) {
            expectChanged(base, field,
                          [&](auto& p) { mutate(entry ? p.entrySave : p.actor.save()); });
        };
        saveChange("name", [](auto& s) { s.name = "TEST"; });
        saveChange("selected class", [](auto& s) { s.character = 1; });
        saveChange("color", [](auto& s) { s.color = 1; });
        saveChange("class unlocks", [](auto& s) { s.classUnlock = 1; });
        saveChange("live gold", [](auto& s) { ++s.gold; });
        saveChange("total levels", [](auto& s) { ++s.levelTotal; });
        saveChange("auto attack", [](auto& s) { s.autoAttack = false; });
        saveChange("help history", [](auto& s) { s.helpSeen.push_back(7); });
        saveChange("movie history", [](auto& s) { s.moviesSeen.push_back("INTRO"); });
        for (usize index = 0; index < kClassCount; ++index) {
            CAPTURE(index);
            const auto change = [&](std::string_view field, auto mutate) {
                saveChange(field, [&](auto& s) { mutate(s.classes[index]); });
            };
            for (const auto member : {&ClassProgress::experience, &ClassProgress::gold,
                                      &ClassProgress::promotedLevel, &ClassProgress::health}) {
                change("class integer", [&](auto& p) { ++(p.*member); });
            }
            for (const auto member : {&ClassProgress::fightAdd, &ClassProgress::armorAdd,
                                      &ClassProgress::magicAdd, &ClassProgress::speedAdd}) {
                change("class bonus", [&](auto& p) { p.*member = 0.25f; });
            }
            change("realm unlocks", [](auto& p) { p.unlocked = 1; });
            change("keys", [](auto& p) { ++p.inventory.keys; });
            change("potions", [](auto& p) { p.inventory.potions.push_back(2); });
            change("permanent Pojo", [](auto& p) { p.inventory.permanentPojo = true; });
            for (usize slot = 0; slot < Inventory::kPowerupSlots; ++slot) {
                CAPTURE(slot);
                change("powerup strength",
                       [slot](auto& p) { p.inventory.powerups[slot].strength = 0.25f; });
                change("powerup kind", [slot](auto& p) { ++p.inventory.powerups[slot].kind; });
                change("powerup charge",
                       [slot](auto& p) { p.inventory.powerups[slot].charge = 0.25f; });
                change("powerup flags", [slot](auto& p) { p.inventory.powerups[slot].flags = 4; });
                change("powerup enabled",
                       [slot](auto& p) { p.inventory.powerups[slot].on = false; });
            }
            for (const auto member :
                 {&Relics::runes, &Relics::legends, &Relics::shards, &Relics::pendingRunes,
                  &Relics::pendingShards, &Relics::pendingCeremonies}) {
                change("relic masks", [&](auto& p) { p.relics.*member = 1; });
            }
            for (usize kind = 0; kind < Relics::kGargoyleKinds; ++kind) {
                change("gargoyle pieces", [kind](auto& p) { ++p.relics.gargoylePieces[kind]; });
            }
            for (usize realm = 0; realm < kRealmCount; ++realm) {
                change("crystals", [realm](auto& p) { ++p.crystals[realm]; });
                change("beaten levels", [realm](auto& p) { ++p.levels.beaten[realm]; });
            }
            for (usize pass = 0; pass < LevelRecord::kPasses; ++pass) {
                change("rune levels", [pass](auto& p) { ++p.levels.runeLevels[pass]; });
                change("legend levels", [pass](auto& p) { ++p.levels.legendLevels[pass]; });
                change("boss deaths", [pass](auto& p) { ++p.levels.bossDeaths[pass]; });
            }
            change("enemy kills", [](auto& p) { ++p.lifetime.enemiesKilled; });
            change("generators", [](auto& p) { ++p.lifetime.generatorsDestroyed; });
            change("gold found", [](auto& p) { ++p.lifetime.goldFound; });
            change("play time", [](auto& p) { p.lifetime.playSeconds = 0.125; });
            change("best streak", [](auto& p) { ++p.lifetime.bestStreak; });
        }
    }
}

TEST_CASE("native player digest includes private body and action state without projection",
          "[native-state-digest][player-native-state]") {
    const PlayerSimulation base;
    const auto change = [&](std::string_view field, auto mutate) {
        expectChanged(base, field, mutate);
    };
    change("input seat", [](auto& p) { p.actor.spawn(2, {}, nullptr, Vec3{0}, 0); });
    change("position", [](auto& p) { p.actor.place({0, 1, 0}); });
    change("yaw", [](auto& p) { p.actor.turnTo(0.25f); });
    change("pace bonus", [](auto& p) { p.actor.setPaceBonus(0.25f); });
    for (const auto member : {&ClassStats::speedMin, &ClassStats::width, &ClassStats::height,
                              &ClassStats::collisionY, &ClassStats::attentionY}) {
        change("native body envelope", [&](auto& p) {
            ClassStats stats;
            stats.*member = 10;
            p.actor.spawn(0, {}, &stats, Vec3{0}, 0);
        });
    }
    change("moving latch", [](auto& p) {
        p.actor.update({Vec2{0, 1}, 1}, 0, 0.01f, nullptr);
        p.actor.place(Vec3{0});
    });
    auto contact = base;
    const auto collision = floorAndWall();
    contact.actor.slide({8, 0, 0}, &collision);
    REQUIRE_FALSE(contact.actor.wallContacts().empty());
    expectChanged(contact, "wall contacts", [](auto& p) { p.actor.clearWallContacts(); });
    const WallContact wall{2, {1, 2, 3}};
    expectChanged(wall, "wall identity", [](auto& w) { ++w.object; });
    expectChanged(wall, "wall point", [](auto& w) { ++w.point.y; });

    auto held = base;
    held.capture.attach(4, false, Mat4{1}, held.actor);
    auto higher = base;
    higher.actor.place({0, 2, 0});
    higher.capture.attach(4, false, Mat4{1}, higher.actor);
    REQUIRE(held.actor == higher.actor);
    REQUIRE(held.capture.body() == higher.capture.body());
    CHECK(nativeDigest(held) != nativeDigest(higher)); // retained pre-grab support height
    expectChanged(held, "captor identity",
                  [](auto& p) { p.capture.attach(5, false, Mat4{1}, p.actor); });
    expectChanged(held, "boss captor",
                  [](auto& p) { p.capture.attach(4, true, Mat4{1}, p.actor); });
    expectChanged(held, "capture matrix", [](auto& p) {
        p.capture.attach(4, false, glm::translate(Mat4{1}, Vec3{1, 0, 0}), p.actor);
    });
    auto thrown = held;
    thrown.capture.release({1, 0, 0}, 2);
    expectChanged(thrown, "deferred landing damage", [&](auto& p) {
        p.capture = held.capture;
        p.capture.release({1, 0, 0}, 3);
    });
    expectChanged(thrown, "throw velocity", [&](auto& p) {
        p.capture = held.capture;
        p.capture.release({2, 0, 0}, 2);
    });

    change("transport destination", [](auto& p) {
        REQUIRE(p.transport.begin({3, 0, 0}));
        p.transport.cancel();
    });
    auto transporting = base;
    REQUIRE(transporting.transport.begin({3, 0, 0}));
    expectChanged(transporting, "transport clock", [](auto& p) { p.transport.update(1); });
    transporting.transport.cancel();
    expectChanged(transporting, "transport rearm",
                  [](auto& p) { p.transport.observeContact(false); });
    change("invisible decoy fraction", [](auto& p) {
        Inventory inventory;
        p.mikey.update(1.0f / 120.0f, inventory, Vec3{0});
        REQUIRE_FALSE(p.mikey.shown());
    });
    auto decoy = base;
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 60);
    inventory.powerups[0].on = true; // Mikey requires explicit selector activation.
    decoy.mikey.update(1.0f / 30.0f, inventory, Vec3{0});
    REQUIRE(decoy.mikey.generation() > 0);
    decoy.mikey.clear();
    REQUIRE_FALSE(decoy.mikey.shown());
    CHECK(nativeDigest(decoy) != nativeDigest(base));

    change("queued force", [](auto& p) { p.knockback.queue({1, 0, 0}, 0, 0); });
    change("queued flags", [](auto& p) { p.knockback.queue(Vec3{0}, Knockback::kKnockBack, 0); });
    change("queued damage", [](auto& p) { p.knockback.queue(Vec3{0}, 0, 1); });
    change("slide velocity", [](auto& p) { p.knockback.addVelocity({1, 0, 0}); });
    change("fractional motion tick", [](auto& p) { p.knockback.step(1.0f / 120.0f, 1); });
    change("shove latch", [](auto& p) { p.knockback.shove(Vec3{0}, 1.0f / 30.0f); });
    change("previous shove", [](auto& p) {
        p.knockback.shove(Vec3{0}, 1.0f / 30.0f);
        p.knockback.endFrame();
    });
    change("fast interval without visible velocity", [](auto& p) {
        p.knockback.queue(Vec3{0}, Knockback::kKnockDown, 2);
        p.knockback.kick(0, false);
        REQUIRE(p.knockback.velocity() == Vec3{0});
    });
    change("held turbo", [](auto& p) { p.turbo.add(40); });
    change("turbo display clock", [](auto& p) { p.turbo.step(1); });
    change("melee streak", [](auto& p) { p.streak.record(false); });
    auto streak = base;
    streak.streak.record(false);
    expectChanged(streak, "quiet streak clock", [](auto& p) { p.streak.step(0.25f); });
    change("exit wait", [](auto& p) { p.exitWait.step(true, 1); });
    auto gem = base;
    gem.gem.update(powerup::kHandOfDeath);
    gem.gem.update(powerup::kHealthVamp);
    auto onlyVamp = base;
    onlyVamp.gem.update(powerup::kHealthVamp);
    REQUIRE(gem.gem.shown() == onlyVamp.gem.shown());
    CHECK(nativeDigest(gem) != nativeDigest(onlyVamp));

    const auto stats = turboStats();
    auto attacking = base;
    attacking.turbo.add(100);
    const auto before = attacking;
    attacking.move.begin(PlayerAnimator::Action::TurboStrong, &stats, attacking.turbo);
    REQUIRE(attacking.turbo == before.turbo);
    CHECK(nativeDigest(attacking) != nativeDigest(before));
    const auto pending = attacking;
    const TurboMove::Events ignore{.announce = [](s32) {},
                                   .dim = [](f32) {},
                                   .volley = [](const Vec3&) {},
                                   .strike = [](s32) {}};
    attacking.move.advance(PlayerAnimator::Action::TurboStrong, 0, {0, 0, 1}, &stats,
                           attacking.turbo, ignore);
    REQUIRE(attacking.turbo == pending.turbo);
    REQUIRE(attacking.move.weaponHidden());
    CHECK(nativeDigest(attacking) != nativeDigest(pending));
    const auto firstShot = attacking;
    attacking.move.advance(PlayerAnimator::Action::TurboStrong, 1, {0, 0, 1}, &stats,
                           attacking.turbo, ignore);
    REQUIRE(attacking.turbo == firstShot.turbo);
    CHECK(nativeDigest(attacking) != nativeDigest(firstShot));
}

TEST_CASE("native player digest follows one through eight corrected ticks and direct rewinds",
          "[native-state-digest][player-native-state][local-rewind]") {
    constexpr f32 kDt = 1.0f / 60.0f;
    const auto stats = turboStats();
    PlayerSimulation player;
    player.actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    player.entrySave.classes.back().lifetime.goldFound = 9;
    player.resultsCheckpoint = LevelResults::Checkpoint{12, 34, 56};
    player.turbo.add(100);
    player.move.begin(PlayerAnimator::Action::TurboStrong, &stats, player.turbo);
    REQUIRE(player.transport.begin({3, 0, 0}));
    player.transport.update(12);
    const auto saved = player;
    const auto savedDigest = nativeDigest(saved);
    struct Output {
        std::optional<Vec3> transported;
        std::vector<TurboMove::Strike> strikes;
        std::vector<TurboMove::Volley> volleys;
        bool operator==(const Output&) const = default;
    };
    const auto step = [&](s32 tick, bool wrong) {
        Output result;
        player.cursorAiming = !wrong;
        player.meleeFacing = wrong ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
        result.transported = player.transport.update(1);
        if (result.transported) {
            player.actor.place(*result.transported);
        }
        player.actor.update({Vec2{wrong ? -1.0f : 1.0f, 0}, 1}, 0, kDt, nullptr);
        player.mikey.update(kDt, player.actor.save().progress().inventory, player.actor.position());
        player.turbo.step(1);
        player.move.advance(
            PlayerAnimator::Action::TurboStrong, static_cast<f32>(tick) * 0.5f, *player.meleeFacing,
            &stats, player.turbo,
            {.announce = [](s32) {},
             .dim = [](f32) {},
             .identifiedVolley = [&](const auto& shot) { result.volleys.push_back(shot); },
             .identifiedStrike = [&](const auto& hit) { result.strikes.push_back(hit); }});
        player.surfaceGap = wrong ? 0.75f : 0.25f;
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
    std::array<NativeStateDigest::Digest, 8> expectedDigests{};
    std::array<Output, 8> outputs;
    for (s32 tick = 0; tick < 8; ++tick) {
        outputs[tick] = step(tick, false);
        expected[tick] = player;
        expectedDigests[tick] = nativeDigest(player);
    }
    REQUIRE_FALSE(outputs[4].strikes.empty());
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        player = saved;
        for (s32 tick = 0; tick < distance; ++tick) {
            step(tick, true);
        }
        REQUIRE(nativeDigest(player) != expectedDigests[distance - 1]);
        player = saved;
        REQUIRE(player == saved);
        REQUIRE(nativeDigest(player) == savedDigest);
        for (s32 tick = 0; tick < distance; ++tick) {
            CAPTURE(tick);
            CHECK(step(tick, false) == outputs[tick]);
            REQUIRE(player == expected[tick]);
            CHECK(nativeDigest(player) == expectedDigests[tick]);
        }
    }
}

TEST_CASE("native player digest ignores allocation addresses and external runtime bindings",
          "[native-state-digest][player-native-state]") {
    PlayerRuntime first;
    PlayerRuntime second;
    first.actor.save().name = "A LONG NAME OUTSIDE SMALL STRING STORAGE";
    first.entrySave.helpSeen = {1, 2, 3};
    second.simulation() = first.simulation();
    REQUIRE(first.actor.save().name.data() != second.actor.save().name.data());
    REQUIRE(first.entrySave.helpSeen.data() != second.entrySave.helpSeen.data());
    first.previous.position = {4, 5, 6};
    first.previous.yaw = 1;
    first.previous.continuous = true;
    first.previous.animationRevision = 9;
    first.slot = 4;
    first.deathHeldEffect = 5;
    first.deathHeldCry = 7;
    first.deathHeldSuck = 8;
    CHECK(first.simulation() == second.simulation());
    CHECK(nativeDigest(first.simulation()) == nativeDigest(second.simulation()));
}
} // namespace
