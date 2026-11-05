#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/characters/ClassData.h"
#include "game/characters/NameCheats.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/EffectTrees.h"
#include "game/party/ActionObservation.h"
#include "game/party/PartyMotion.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerHealth.h"
#include "game/party/PlayerRuntime.h"
#include "game/party/ScriptedMotion.h"
#include "game/players/Knockback.h"
#include "game/players/PlayerAnimator.h"
#include "game/rollback/LocalStateHistory.h"
#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::ActionObservation;
using test::observeAction;
constexpr f32 kStep = 1.0f / 60.0f;
constexpr usize kSeats = 4;

struct CompanionObservation {
    PowerupCompanion::Kind kind{};
    AnimationPlayer::Checkpoint animation;
    const TreeModel* model = nullptr;
    f32 frame = 0;
    f32 textureClock = 0;
    std::optional<Mat4> head;
    bool shown = false;
    bool operator==(const CompanionObservation&) const = default;
};
CompanionObservation observeCompanion(const PowerupCompanion& companion) {
    const auto head = companion.attachment(Mat4{1}, "POJOBODY1_HE#1");
    if (companion.kind() == PowerupCompanion::Kind::Pojo && companion.shown()) {
        REQUIRE(head);
    }
    const auto visual = companion.visual(Mat4{1}, 1);
    return {companion.kind(),
            companion.animationCheckpoint(),
            visual ? visual->model : nullptr,
            visual ? visual->frame : 0.0f,
            visual ? visual->textureClock : 0.0f,
            head,
            companion.shown()};
}

TEST_CASE("local companion rewind crosses every native kind without replacing its artwork",
          "[local-rewind][companion-local-rewind][assets]") {
    using Kind = PowerupCompanion::Kind;
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    PowerupCompanion companion;
    LocalStateHistory<PowerupCompanion::LocalSnapshot> history;
    const std::array kinds{Kind::None,       Kind::Pojo,       Kind::Phoenix,    Kind::Wings,
                           Kind::FireShield, Kind::FireBreath, Kind::AcidBreath, Kind::ElecBreath};
    std::array<const TreeModel*, kinds.size()> models{};
    // Warm the bounded artwork cache, then exercise revisits through the normal choose path.
    for (usize index = 0; index < kinds.size(); ++index) {
        companion.choose(device, kinds[index], powerups, &weapons);
        if (kinds[index] != Kind::None) {
            REQUIRE(companion.shown());
            models[index] = companion.visual(Mat4{1}, 1)->model;
        }
    }
    const auto uploads = device.texturesCreated;
    const auto step = [&](u64 tick, bool alternative) {
        const usize selected = (tick / 3 + (alternative ? 3 : 0)) % kinds.size();
        companion.choose(device, kinds[selected], powerups, &weapons);
        companion.capturePresentation();
        companion.update(
            kStep, tick % 7 < 4 ? PlayerAnimator::Action::Run1 : PlayerAnimator::Action::Breathe,
            tick % 2 == 0, tick % 3 == 0);
        if (companion.shown()) {
            CHECK(companion.visual(Mat4{1}, 1)->model == models[selected]);
        }
    };
    for (u64 tick = 0; tick < 192; ++tick) {
        REQUIRE(history.capture(tick, [&](auto& saved) {
            companion.saveLocal(saved);
            return true;
        }));
        const auto before = observeCompanion(companion);
        const auto distance = static_cast<usize>(tick % 8 + 1);
        std::array<CompanionObservation, 8> expected;
        for (usize frame = 0; frame < distance; ++frame) {
            step(tick + frame, false);
            expected[frame] = observeCompanion(companion);
        }
        REQUIRE(companion.restoreLocal(*history.find(tick)));
        CHECK(observeCompanion(companion) == before);
        // Deliberately visit different trees before returning to this same reusable save.
        for (usize frame = 0; frame < distance; ++frame) {
            step(tick + frame, true);
        }
        REQUIRE(companion.restoreLocal(*history.find(tick)));
        for (usize frame = 0; frame < distance; ++frame) {
            step(tick + frame, false);
            CHECK(observeCompanion(companion) == expected[frame]);
        }
        REQUIRE(companion.restoreLocal(*history.find(tick)));
        step(tick, false);
    }
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("local companion history restores an unbound state and rejects foreign lifetimes",
          "[local-rewind][companion-local-rewind][assets]") {
    PowerupCompanion original;
    PowerupCompanion other;
    PowerupCompanion::LocalSnapshot empty;
    CHECK_FALSE(original.restoreLocal(empty));
    original.saveLocal(empty);
    CHECK_FALSE(other.restoreLocal(empty));
    const auto before = observeCompanion(original);
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    ItemArchive archive;
    test::FakeRenderDevice device;
    REQUIRE(archive.load(root / "POWERUPS"));
    original.choose(device, PowerupCompanion::Kind::Pojo, archive, nullptr);
    REQUIRE(original.shown());
    original.update(kStep, PlayerAnimator::Action::Breathe, true, false);
    REQUIRE(original.restoreLocal(empty));
    CHECK(observeCompanion(original) == before);
    const auto uploads = device.texturesCreated;
    original.choose(device, PowerupCompanion::Kind::Pojo, archive, nullptr);
    CHECK(original.shown());
    CHECK(device.texturesCreated == uploads);
    PowerupCompanion::LocalSnapshot destroyed;
    {
        const PowerupCompanion temporary;
        temporary.saveLocal(destroyed);
    }
    CHECK_FALSE(original.restoreLocal(destroyed));
}

TEST_CASE("local familiar history refuses replaced artwork without changing its clocks",
          "[local-rewind][companion-local-rewind]") {
    PlayerFamiliar first;
    PlayerFamiliar second;
    PlayerFamiliar::LocalSnapshot saved;
    CHECK_FALSE(first.restoreLocal(saved));
    first.saveLocal(saved);
    REQUIRE(first.restoreLocal(saved));
    CHECK_FALSE(second.restoreLocal(saved));
    test::FakeRenderDevice device;
    ItemArchive empty;
    CHECK_FALSE(first.bind(device, empty, 80, Vec3{1, 2, 3}));
    const auto afterRebind = test::liveDigest(first);
    CHECK_FALSE(first.restoreLocal(saved));
    CHECK(test::liveDigest(first) == afterRebind);
    CHECK(device.texturesCreated == 0);
}

static_assert(std::is_copy_constructible_v<PlayerRuntime::Simulation>);
static_assert(std::is_copy_assignable_v<PlayerRuntime::Simulation>);
static_assert(!std::is_copy_constructible_v<PlayerRuntime>);

// The values compared between runs: a copy of the simulation. It is never the rewind mechanism.
PlayerSimulation observeValues(const PlayerRuntime& player) {
    return player.simulation();
}

// A party-component test, deliberately not described as whole-scene rewind. The world
// is fixed; attack callbacks are recorded, not spawned as uncaptured projectiles.
struct PartyRewind {
    test::FakeRenderDevice device;
    WorldCollision collision;
    ClassStats stats;
    std::array<PlayerRuntime, kSeats> players;
    PlayerHealth health;
    TowerCamera camera;
    std::vector<std::pair<usize, PartyMotion::Action>> actions;
    std::vector<std::string> sounds;

    struct Saved {
        std::array<PlayerRuntime::Simulation, kSeats> players;
        std::array<PlayerFigure::LocalSnapshot, kSeats> figures;
        PlayerHealth health;
        TowerCamera::State camera;
    };
    explicit PartyRewind(bool native = false) {
        collision.build({
            {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}},
            {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}},
        });
        for (usize seat = 0; seat < kSeats; ++seat) {
            CharacterSave save;
            save.name = "TEST";
            save.character = std::array<s32, kSeats>{0, 2, 5, 6}[seat];
            save.color = static_cast<s32>(seat);
            save.progress().health = 2000;
            save.progress().experience = levelExperience(99);
            save.progress().inventory.potions = {0, 1, 2, 3};
            auto& player = players[seat];
            player.actor.spawn(static_cast<s32>(seat), save, &stats,
                               Vec3{static_cast<f32>(seat) * 15, 0, 0}, 0);
            player.entrySave = save;
            player.turbo.add(100);
            player.helpHeard = {2, 4};
            player.shockGaps = {{2, 0.1f}, {7, 0.3f}};
            if (native) {
                const auto root =
                    test::assetOrSkip("WEAPONS/objects.ngc").parent_path().parent_path();
                player.figure = PlayerFigure::load(device, root, save, Entrance::Skip);
                REQUIRE(player.figure);
                REQUIRE(player.figure->animator().bound());
            }
        }
    }
    void save(Saved& state) const {
        for (usize seat = 0; seat < kSeats; ++seat) {
            state.players[seat] = players[seat].simulation();
            if (players[seat].figure) {
                players[seat].figure->saveLocal(state.figures[seat]);
            }
        }
        state.health = health;
        state.camera = camera.localState();
    }
    bool restore(const Saved& state) {
        for (usize seat = 0; seat < kSeats; ++seat) {
            if (players[seat].figure &&
                !players[seat].figure->canRestoreLocal(state.figures[seat])) {
                return false;
            }
        }
        for (usize seat = 0; seat < kSeats; ++seat) {
            if (players[seat].figure && !players[seat].figure->restoreLocal(state.figures[seat])) {
                return false;
            }
            players[seat].simulation() = state.players[seat];
        }
        health = state.health;
        camera.restoreLocal(state.camera);
        return true;
    }
    void step(s32 tick, bool alternate = false) {
        actions.clear();
        sounds.clear();
        std::array<PlayInput, kSeats> inputs;
        for (usize seat = 0; seat < kSeats; ++seat) {
            auto& player = players[seat];
            auto& input = inputs[seat];
            const s32 phase = (tick + static_cast<s32>(seat) * 7) % 120;
            input.move = {Vec2{alternate ? -0.6f : 0.6f, 0.8f}, 1};
            input.attack = phase < 60;
            input.strongAttack = phase >= 80 && phase < 100;
            input.strafe = phase >= 30 && phase < 60;
            input.usePotion = phase >= 60 && phase < 80;
            input.defendPressed = phase == 100;
            input.aimDirection = Vec3{alternate ? -1.0f : 1.0f, 0, 1};
            const PlayerHealth::Events events{
                .sound = [this](std::string_view cue) { sounds.emplace_back(cue); },
                .cry = [this](std::string_view cue) { sounds.emplace_back(cue); },
                .named = [this](std::string_view cue, f32) { sounds.emplace_back(cue); }};
            if (phase == 19 || phase == 94) {
                health.hurt(player, 7.25f, HurtKind::Blow, HurtSource::Directed, {}, events,
                            {damage::kKnockBack, Vec3{0, 0, 1}}, &stats);
            }
            if (phase % 11 == 0) {
                ++player.actor.save().gold;
                player.actor.save().classes[7].experience += 9;
                player.streak.record(true);
            }
            player.streak.step(kStep);
            player.hitSoundGap = std::max(player.hitSoundGap - 1, 0);
            PlayerHealth::advanceSurfaceGap(player, kStep);
        }
        const test::MotionScript events{
            .perform = [this](usize seat,
                              PartyMotion::Action action) { actions.emplace_back(seat, action); },
            .select = [](usize, const SelectorInput&, s32) {},
            .advanceTurbo = [this](usize seat, s32 ticks,
                                   f32) { players[seat].turbo.step(ticks); }};
        test::ScriptedMotion motionLevel{events};
        const auto subjects = PartyMotion::step(players, inputs, false, camera.yaw(), 1, kStep,
                                                collision, motionLevel);
        camera.update(subjects, {}, CameraRange{}, CameraView{}, kStep);
        PlayerHealth::heartbeat(players, 1, false);
    }
};

TEST_CASE("local player values rewind without touching figure ownership or mixer handles",
          "[local-rewind][player-local-rewind]") {
    PartyRewind party;
    auto& player = party.players[0];
    player.figure = std::make_unique<PlayerFigure>();
    player.mikeyFigure = std::make_unique<MikeyFigure>();
    player.slot = 7;
    player.deathHeldCry = 21;
    player.deathHeldSuck = 22;
    player.deathHeldEffect = 23;
    EffectTrees effects;
    player.weaponGlow.update(party.device, effects, nullptr, 1, Mat4{1}, Vec3{0}, Vec3{0});
    REQUIRE(player.weaponGlow.element() == 1);
    player.previous = {Vec3{8, 7, 6}, 1, true, 42};
    player.glow.raise(1.5f);
    const auto* figure = player.figure.get();
    const auto* mikey = player.mikeyFigure.get();
    const auto expected = observeValues(player);
    const auto saved = player.simulation();
    player.actor.save().classes[7].experience += 100;
    player.actor.save().helpSeen.push_back(4);
    player.actor.save().progress().inventory.potions.clear();
    player.shockGaps.clear();
    player.helpHeard.clear();
    player.life = PlayerLife::Dying;
    player.actor.place(Vec3{100, 20, 3});
    player.simulation() = saved;
    CHECK(observeValues(player) == expected);
    CHECK(player.figure.get() == figure);
    CHECK(player.mikeyFigure.get() == mikey);
    CHECK(player.slot == 7);
    CHECK(player.deathHeldCry == 21);
    CHECK(player.deathHeldSuck == 22);
    CHECK(player.deathHeldEffect == 23);
    CHECK(player.weaponGlow.element() == 1);
    CHECK(player.previous.position == Vec3{8, 7, 6});
    CHECK(player.previous.animationRevision == 42);
    CHECK(player.glow.level() == 1.5f);
}

TEST_CASE("local four-player movement and health rewind every tick with corrected inputs",
          "[local-rewind][player-local-rewind]") {
    PartyRewind actual;
    PartyRewind reference;
    for (s32 tick = 0; tick < 20; ++tick) {
        actual.step(tick);
        reference.step(tick);
    }
    PartyRewind::Saved saved;
    actual.save(saved);
    for (s32 tick = 20; tick < 28; ++tick) {
        actual.step(tick, false); // discarded prediction
    }
    REQUIRE(actual.restore(saved));
    for (s32 tick = 20; tick < 28; ++tick) {
        actual.step(tick, true);
        reference.step(tick, true);
        for (usize seat = 0; seat < kSeats; ++seat) {
            CHECK(observeValues(actual.players[seat]) == observeValues(reference.players[seat]));
        }
        CHECK(actual.health == reference.health);
        CHECK(actual.actions == reference.actions);
        CHECK(actual.sounds == reference.sounds);
    }
}

TEST_CASE("native party actions and attachment poses survive one to eight frame local rewind",
          "[local-rewind][player-local-rewind][assets]") {
    PartyRewind party{true};
    usize releases = 0;
    const auto textures = party.device.texturesCreated;
    // Include release boundaries, damage and fractional stance blends, not just an idle pose.
    for (s32 start = 0; start < 240; start += 8) {
        PartyRewind::Saved saved;
        party.save(saved);
        std::vector<std::array<PlayerSimulation, kSeats>> values(8);
        std::array<std::array<ActionObservation, kSeats>, 8> actions;
        std::array<std::vector<std::pair<usize, PartyMotion::Action>>, 8> events;
        std::array<std::vector<std::string>, 8> sounds;
        for (usize frame = 0; frame < 8; ++frame) {
            party.step(start + static_cast<s32>(frame));
            events[frame] = party.actions;
            sounds[frame] = party.sounds;
            releases += std::ranges::count_if(party.actions, [](const auto& event) {
                return event.second == PartyMotion::Action::ThrowWeapon ||
                       event.second == PartyMotion::Action::FamiliarShot;
            });
            for (usize seat = 0; seat < kSeats; ++seat) {
                values[frame][seat] = observeValues(party.players[seat]);
                actions[frame][seat] = observeAction(*party.players[seat].figure);
            }
        }
        for (s32 distance = 1; distance <= 8; ++distance) {
            CAPTURE(start, distance);
            REQUIRE(party.restore(saved));
            for (s32 frame = 0; frame < distance; ++frame) {
                party.step(start + frame);
                for (usize seat = 0; seat < kSeats; ++seat) {
                    CAPTURE(seat, frame);
                    CHECK(observeValues(party.players[seat]) == values[frame][seat]);
                    CHECK(observeAction(*party.players[seat].figure) == actions[frame][seat]);
                }
                CHECK(party.actions == events[frame]);
                CHECK(party.sounds == sounds[frame]);
            }
        }
    }
    CHECK(releases > 0);
    CHECK(party.device.texturesCreated == textures);
}

TEST_CASE("player value rewind retains pending turbo payment strikes and volley cursors",
          "[local-rewind][player-local-rewind]") {
    PartyRewind party;
    auto& player = party.players[0];
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
    REQUIRE(player.move.begin(PlayerAnimator::Action::TurboStrong, &stats, player.turbo).empty());
    std::vector<TurboMove::Strike> strikes;
    std::vector<TurboMove::Volley> shots;
    std::vector<s32> announcements;
    const auto step = [&](s32 tick) {
        strikes.clear();
        shots.clear();
        announcements.clear();
        player.move.advance(
            PlayerAnimator::Action::TurboStrong, static_cast<f32>(tick) * 0.5f, Vec3{0, 0, 1},
            &stats, player.turbo,
            {.announce = [&](s32 id) { announcements.push_back(id); },
             .identifiedVolley = [&](const auto& value) { shots.push_back(value); },
             .identifiedStrike = [&](const auto& value) { strikes.push_back(value); }});
    };
    step(0);
    CHECK(player.move.owed() == TurboMeter::kStrongCost);
    CHECK(player.move.weaponHidden());
    const auto saved = player.simulation();
    std::vector<PlayerSimulation> expected;
    std::vector<std::vector<TurboMove::Strike>> expectedStrikes;
    std::vector<std::vector<TurboMove::Volley>> expectedShots;
    std::vector<std::vector<s32>> expectedAnnouncements;
    for (s32 tick = 1; tick <= 8; ++tick) {
        step(tick);
        expected.push_back(observeValues(player));
        expectedStrikes.push_back(strikes);
        expectedShots.push_back(shots);
        expectedAnnouncements.push_back(announcements);
    }
    CHECK(player.move.owed() == 0);
    CHECK(player.turbo.held() == 60);
    CHECK(std::ranges::any_of(expectedShots, [](const auto& value) { return !value.empty(); }));
    CHECK(std::ranges::any_of(expectedStrikes, [](const auto& value) { return !value.empty(); }));
    for (s32 replay = 0; replay < 2; ++replay) {
        player.simulation() = saved;
        for (s32 tick = 1; tick <= 8; ++tick) {
            step(tick);
            const auto index = static_cast<usize>(tick - 1);
            CHECK(observeValues(player) == expected[index]);
            CHECK(strikes == expectedStrikes[index]);
            CHECK(shots == expectedShots[index]);
            CHECK(announcements == expectedAnnouncements[index]);
        }
    }
}

TEST_CASE("native corrected party inputs match a run that never made the discarded prediction",
          "[local-rewind][player-local-rewind][assets]") {
    PartyRewind actual{true};
    PartyRewind reference{true};
    for (s32 tick = 0; tick < 25; ++tick) {
        actual.step(tick);
        reference.step(tick);
    }
    PartyRewind::Saved saved;
    actual.save(saved);
    for (s32 tick = 25; tick < 33; ++tick) {
        actual.step(tick, false);
    }
    REQUIRE(actual.restore(saved));
    for (s32 tick = 25; tick < 100; ++tick) {
        actual.step(tick, true);
        reference.step(tick, true);
        for (usize seat = 0; seat < kSeats; ++seat) {
            CAPTURE(tick, seat);
            CHECK(observeValues(actual.players[seat]) == observeValues(reference.players[seat]));
            CHECK(observeAction(*actual.players[seat].figure) ==
                  observeAction(*reference.players[seat].figure));
        }
        CHECK(actual.actions == reference.actions);
        CHECK(actual.sounds == reference.sounds);
        CHECK(actual.health == reference.health);
    }
}

TEST_CASE("native figure rewind restores Pojo attachments and both companion release clocks",
          "[local-rewind][companion-local-rewind][assets]") {
    PartyRewind party{true};
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    const auto step = [&](s32 tick, bool corrected) {
        for (usize seat = 0; seat < kSeats; ++seat) {
            auto& inventory = party.players[seat].actor.save().progress().inventory;
            // Switch a form every three frames, including a return to the bare costume.
            constexpr std::array kForms{0U, powerup::kPojo, powerup::kPhoenix, powerup::kFireBreath,
                                        powerup::kLevitation};
            const auto form =
                kForms[(static_cast<usize>(tick / 3) + seat + (corrected ? 1 : 0)) % kForms.size()];
            inventory.powerups = {};
            if (form != 0) {
                inventory.addPowerup(powerup::kSpecial, form, 0, 0.6f);
            }
            party.players[seat].figure->setCompanionPowerups(party.device, powerups, inventory,
                                                             &weapons);
        }
        party.step(tick, corrected);
    };
    const CameraFrame camera = CameraFrame::at({10, 20, 30});
    const auto draw = [&](s32 tick) {
        party.device.draws.clear();
        for (const auto& player : party.players) {
            player.figure->drawCompanions(party.device, Mat4{1}, Mat4{1}, {}, 1, &camera,
                                          tick % 2 == 0 ? 0.37f : 1.0f, TreeModel::Pass::All);
        }
        return party.device.draws;
    };
    bool pojo = false;
    bool dual = false;
    bool released = false;
    for (s32 tick = 0; tick < 45; ++tick) {
        step(tick, false);
    }
    const auto uploads = party.device.texturesCreated;
    LocalStateHistory<PartyRewind::Saved> history;
    for (s32 tick = 45; tick < 117; ++tick) {
        REQUIRE(history.capture(static_cast<u64>(tick), [&](auto& saved) {
            party.save(saved);
            return true;
        }));
        const s32 distance = tick % 8 + 1;
        std::array<std::array<CompanionObservation, kSeats>, 8> companions;
        std::array<std::array<ActionObservation, kSeats>, 8> actions;
        std::array<std::vector<test::RecordedDraw>, 8> draws;
        for (s32 frame = 0; frame < distance; ++frame) {
            step(tick + frame, true);
            for (usize seat = 0; seat < kSeats; ++seat) {
                const auto& figure = *party.players[seat].figure;
                companions[frame][seat] = observeCompanion(figure.companion());
                actions[frame][seat] = observeAction(figure);
                pojo = pojo || figure.pojoActive();
                const auto shown = figure.companionVisuals(Mat4{1}, 1);
                dual = dual || (figure.phoenixActive() && shown[0] && shown[1]);
                released = released || figure.familiarReleased();
            }
            draws[frame] = draw(tick + frame);
        }
        REQUIRE(party.restore(*history.find(tick)));
        for (s32 frame = 0; frame < distance; ++frame) {
            step(tick + frame, false);
        }
        REQUIRE(party.restore(*history.find(tick)));
        for (s32 frame = 0; frame < distance; ++frame) {
            step(tick + frame, true);
            for (usize seat = 0; seat < kSeats; ++seat) {
                CHECK(observeCompanion(party.players[seat].figure->companion()) ==
                      companions[frame][seat]);
                CHECK(observeAction(*party.players[seat].figure) == actions[frame][seat]);
            }
            const auto replayed = draw(tick + frame);
            REQUIRE(replayed.size() == draws[frame].size());
            for (usize index = 0; index < replayed.size(); ++index) {
                CHECK(replayed[index].texture == draws[frame][index].texture);
                CHECK(replayed[index].transform == draws[frame][index].transform);
                CHECK(replayed[index].vertices == draws[frame][index].vertices);
                CHECK(replayed[index].state == draws[frame][index].state);
            }
        }
        REQUIRE(party.restore(*history.find(tick)));
        step(tick, true);
    }
    CHECK(pojo);
    CHECK(dual);
    CHECK(released);
    CHECK(party.device.texturesCreated == uploads);
    CHECK(party.device.textureUpdates == 0);
}

TEST_CASE("local player copies retain capture landing damage transport and two-player combo state",
          "[local-rewind][player-local-rewind]") {
    PartyRewind party;
    auto& captured = party.players[0];
    captured.capture.attach(5, true, glm::translate(Mat4{1}, Vec3{0, 3, 0}), captured.actor);
    captured.capture.release(Vec3{2, -12, 1}, 12.5f);
    REQUIRE(party.players[1].transport.begin(Vec3{30, 0, 10}));
    ComboMove::link(party.players[2].combo, 2, party.players[3].combo, 3, ComboMove::kWarrior);
    PartyRewind::Saved saved;
    party.save(saved);
    const auto step = [&] {
        const auto damage = captured.capture.update(kStep, captured.actor, party.collision);
        const auto destination = party.players[1].transport.update(1);
        ComboMove::tick(party.players[2].combo, 1, kStep);
        ComboMove::tick(party.players[3].combo, 1, kStep);
        return std::pair{damage, destination};
    };
    std::vector<std::pair<std::optional<f32>, std::optional<Vec3>>> outputs;
    std::vector<std::array<PlayerSimulation, kSeats>> expected;
    for (s32 tick = 0; tick < 90; ++tick) {
        outputs.push_back(step());
        std::array<PlayerSimulation, kSeats> frame;
        for (usize seat = 0; seat < kSeats; ++seat) {
            frame[seat] = observeValues(party.players[seat]);
        }
        expected.push_back(std::move(frame));
    }
    CHECK(std::ranges::count_if(outputs,
                                [](const auto& value) { return value.first.has_value(); }) == 1);
    CHECK(std::ranges::count_if(outputs,
                                [](const auto& value) { return value.second.has_value(); }) == 1);
    REQUIRE(party.restore(saved));
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CHECK(step() == outputs[tick]);
        for (usize seat = 0; seat < kSeats; ++seat) {
            CHECK(observeValues(party.players[seat]) == expected[tick][seat]);
        }
    }
}

TEST_CASE(
    "native party rewinds from a sixteen-frame ring after wrap and invalidates the old branch",
    "[local-rewind][player-local-rewind][assets]") {
    PartyRewind party{true};
    LocalStateHistory<PartyRewind::Saved> history;
    std::vector<std::array<PlayerSimulation, kSeats>> expected(8);
    std::array<std::array<ActionObservation, kSeats>, 8> animations;
    const auto capture = [&](PartyRewind::Saved& state) {
        party.save(state);
        return true;
    };
    for (u64 tick = 0; tick < 40; ++tick) {
        REQUIRE(history.capture(tick, capture));
        party.step(static_cast<s32>(tick));
        if (tick >= 32) {
            for (usize seat = 0; seat < kSeats; ++seat) {
                expected[tick - 32][seat] = observeValues(party.players[seat]);
                animations[tick - 32][seat] = observeAction(*party.players[seat].figure);
            }
        }
    }
    CHECK(history.find(23) == nullptr);
    REQUIRE(history.find(24));
    for (u64 distance = 1; distance <= 8; ++distance) {
        const u64 start = 40 - distance;
        const auto* saved = history.find(start);
        REQUIRE(saved);
        REQUIRE(party.restore(*saved));
        history.discardAfter(start);
        for (u64 tick = start + 1; tick < 40; ++tick) {
            CHECK(history.find(tick) == nullptr);
        }
        for (u64 tick = start; tick < 40; ++tick) {
            REQUIRE(history.capture(tick, capture));
            party.step(static_cast<s32>(tick));
            for (usize seat = 0; seat < kSeats; ++seat) {
                CAPTURE(distance, tick, seat);
                CHECK(observeValues(party.players[seat]) == expected[tick - 32][seat]);
                CHECK(observeAction(*party.players[seat].figure) == animations[tick - 32][seat]);
            }
        }
    }
}

TEST_CASE("local action snapshots are reusable but cannot borrow another figure's resources",
          "[local-rewind][player-local-rewind][assets]") {
    PartyRewind party{true};
    auto& first = *party.players[0].figure;
    auto& other = *party.players[1].figure;
    const PlayerFigure::LocalSnapshot empty;
    const auto original = observeAction(first);
    CHECK_FALSE(first.restoreLocal(empty));
    CHECK(observeAction(first) == original);
    PlayerFigure::LocalSnapshot saved;
    first.saveLocal(saved);
    const auto foreign = observeAction(other);
    CHECK_FALSE(other.restoreLocal(saved));
    CHECK(observeAction(other) == foreign);
    for (s32 run = 0; run < 2; ++run) {
        first.animate(1, 1, kStep, PlayerDeed::Attack);
        REQUIRE(first.restoreLocal(saved));
        CHECK(observeAction(first) == original);
    }
    auto moved = std::move(saved);
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(first.restoreLocal(saved));
    REQUIRE(first.restoreLocal(moved));
}

TEST_CASE("local action state cannot outlive its figure even when its address is reused",
          "[local-rewind][player-local-rewind]") {
    alignas(PlayerFigure) std::array<std::byte, sizeof(PlayerFigure)> storage{};
    const auto destroy = [](PlayerFigure* figure) { std::destroy_at(figure); };
    using Owner = std::unique_ptr<PlayerFigure, decltype(destroy)>;
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    Owner figure{std::construct_at(reinterpret_cast<PlayerFigure*>(storage.data())), destroy};
    PlayerFigure::LocalSnapshot saved;
    figure->saveLocal(saved);
    REQUIRE(figure->restoreLocal(saved));
    figure.reset();
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    figure.reset(std::construct_at(reinterpret_cast<PlayerFigure*>(storage.data())));
    CHECK_FALSE(figure->restoreLocal(saved));
    PlayerFigure::LocalSnapshot replacement;
    figure->saveLocal(replacement);
    CHECK(figure->restoreLocal(replacement));
}

TEST_CASE("native action rewind replays releases and guards exactly at subframe boundaries",
          "[rollback][player-local-rewind]") {
    TreeInfo tree;
    tree.name = "WAR";
    tree.nodes.emplace_back();
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 9;
        sequence.frameRate = 24;
        tree.sequences.push_back(sequence);
    }
    constexpr std::array kDeeds{PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::Defend,
                                PlayerDeed::None};
    constexpr s32 kDeedTicks = 65;
    constexpr usize kFuture = 24;
    const auto request = [&](s32 tick) {
        return kDeeds[static_cast<usize>(tick / kDeedTicks) % kDeeds.size()];
    };
    PlayerAnimator original;
    REQUIRE(original.bind(tree, Entrance::Skip));
    original.setCharacter(0);
    original.setGuardArmor(2.5f);
    original.setAttackSpeed(true, false);
    s32 ordinaryReleases = 0;
    s32 strongReleases = 0;
    s32 guardFrames = 0;
    s32 diverged = 0;
    for (s32 tick = 0; tick < 400; ++tick) {
        CAPTURE(tick);
        // A figure's local snapshot of its actions holds a copy of its animator.
        const PlayerAnimator saved = original;
        auto expected = original;
        std::array<PlayerAnimator::Checkpoint, kFuture> future;
        for (usize frame = 0; frame < kFuture; ++frame) {
            expected.update(PlayerMotion::Stand, 1, kStep, request(tick + static_cast<s32>(frame)));
            future[frame] = expected.checkpoint();
        }
        // A prediction asks for the next deed of the cycle before it is rewound.
        auto replayed = original;
        for (usize frame = 0; frame < kFuture; ++frame) {
            replayed.update(PlayerMotion::Stand, 1, kStep,
                            request(tick + static_cast<s32>(frame) + kDeedTicks));
        }
        diverged += replayed.checkpoint() == future.back() ? 0 : 1;
        replayed = saved;
        REQUIRE(replayed.checkpoint() == original.checkpoint());
        for (usize frame = 0; frame < kFuture; ++frame) {
            replayed.update(PlayerMotion::Stand, 1, kStep, request(tick + static_cast<s32>(frame)));
            REQUIRE(replayed.checkpoint() == future[frame]);
        }
        original.update(PlayerMotion::Stand, 1, kStep, request(tick));
        ordinaryReleases += original.released() ? 1 : 0;
        strongReleases += original.strongReleased() ? 1 : 0;
        guardFrames += original.defending() ? 1 : 0;
    }
    CHECK(ordinaryReleases > 0);
    CHECK(strongReleases > 0);
    CHECK(guardFrames > 0);
    CHECK(diverged > 0);
}

TEST_CASE("player knockback rewind replays pending kick and shove through partial native frames",
          "[rollback][player-local-rewind][cadence]") {
    constexpr f32 kFrame = 1.0f / Knockback::kFrameRate;
    for (const s32 frequency : {30, 60, 120, 144}) {
        const f32 seconds = 1.0f / static_cast<f32>(frequency);
        for (const f32 phase : {0.0f, kFrame / 8, kFrame / 2, std::nextafter(kFrame, 0.0f)}) {
            CAPTURE(frequency, phase);
            // A fall's kick and a shove shown, part of a native frame run, then shoved again
            // and struck before motion consumes the blow.
            Knockback original;
            original.shove({-0.5f, 0, 1.25f}, kFrame);
            original.endFrame();
            original.queue({0, 0, 1}, Knockback::kKnockDown, 20);
            REQUIRE(original.kick(0, false));
            original.addVelocity({0, -0.5f, 9});
            original.step(phase, 10);
            original.shove({-0.5f, 0, 1.25f}, kFrame);
            original.queue({1.25f, -2.5f, 3.75f}, Knockback::kKnockDown | Knockback::kKnockBack,
                           7.5f);
            REQUIRE(original.pending());
            REQUIRE(original.pushed());
            for (s32 tick = 0; tick < 80; ++tick) {
                // The player's simulation holds its knockback by value: a copy is its rewind.
                const Knockback saved = original;
                Knockback replayed = original;
                // A prediction takes another blow and moves on before it is rewound.
                replayed.queue({1, 0, 0}, Knockback::kBlownAway, 40);
                CHECK_FALSE(replayed == original);
                replayed.step(seconds, 10);
                replayed.endFrame();
                replayed = saved;
                // The boundary is before native motion consumes the queued impulse.
                CHECK(replayed.step(seconds, 10) == original.step(seconds, 10));
                CHECK(replayed.kick(-0.5f, tick % 2 == 0) == original.kick(-0.5f, tick % 2 == 0));
                for (Knockback* knock : {&original, &replayed}) {
                    if (tick % 3 == 0) {
                        knock->shove({0.2f, 0, -0.4f}, seconds);
                    }
                    knock->endFrame();
                    if (tick % 9 == 0) {
                        knock->queue({-1, 4, 2}, Knockback::kBlownAway, 12);
                    }
                }
                CHECK(replayed == original);
            }
        }
    }
}

/** Three nodes, every action sequence keying all of them, each sequence a little apart. */
TreeInfo poseTree() {
    TreeInfo tree;
    tree.name = "POSE";
    for (s32 node = 0; node < 3; ++node) {
        TreeNodeInfo info;
        info.name = "NODE" + std::to_string(node);
        info.parent = node - 1;
        info.position = {0.125f * static_cast<f32>(node), 0.25f, -0.5f};
        tree.nodes.push_back(info);
    }
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 9;
        sequence.frameRate = 24;
        const auto offset = static_cast<f32>(tree.sequences.size()) * 0.03125f;
        for (u32 node = 0; node < tree.nodes.size(); ++node) {
            TrackInfo track;
            track.node = node;
            track.flags = static_cast<u16>(TrackInfo::kChannels |
                                           ((node % 2) != 0 ? TrackInfo::kPitchYawRoll : 0));
            track.frames = {0, 8};
            track.values = {
                offset,        0.2f, -0.3f, offset,        0.5f, 0.75f, 1,    1.125f, 0.875f,
                offset + 0.4f, 0.6f, -0.1f, offset + 0.5f, 0.6f, 0.8f,  0.9f, 1,      1.1f};
            sequence.trackOfNode.push_back(static_cast<s32>(sequence.tracks.size()));
            sequence.tracks.push_back(track);
        }
        tree.sequences.push_back(sequence);
    }
    return tree;
}

void samePose(const TreePose& actual, const TreePose& expected) {
    CHECK(std::ranges::equal(actual.poses(), expected.poses()));
    CHECK(std::ranges::equal(actual.matrices(), expected.matrices()));
}

/** The animator's clock, blend, pose and presented pose agree. */
void sameAnimator(const PlayerAnimator& actual, const PlayerAnimator& expected) {
    CHECK(actual.checkpoint() == expected.checkpoint());
    CHECK(actual.transitionPose() == expected.transitionPose());
    samePose(actual.pose(), expected.pose());
    TreePose actualPresentation;
    TreePose expectedPresentation;
    actual.evaluatePresentation(actualPresentation);
    expected.evaluatePresentation(expectedPresentation);
    samePose(actualPresentation, expectedPresentation);
}

/** Runs, then stands until the stance blends in from the run. */
void reachBlend(PlayerAnimator& player) {
    for (s32 tick = 0; tick < 8; ++tick) {
        player.update(PlayerMotion::Run, 1, kStep);
    }
    for (s32 tick = 0; tick < 120 && !player.player().transitioning(); ++tick) {
        player.update(PlayerMotion::Stand, 1, kStep);
    }
    REQUIRE(player.player().transitioning());
}

TEST_CASE("player pose rewind survives interrupted blends and native early update branches",
          "[rollback][player-local-rewind]") {
    const auto tree = poseTree();
    PlayerAnimator player;
    REQUIRE(player.bind(tree, Entrance::Skip));
    player.setCharacter(0);
    constexpr std::array kInterrupts{PlayerDeed::None,         PlayerDeed::Attack,
                                     PlayerDeed::StrongAttack, PlayerDeed::Defend,
                                     PlayerDeed::Flinch,       PlayerDeed::TurboStrong,
                                     PlayerDeed::TurboFull,    PlayerDeed::ComboHeld};
    s32 diverged = 0;
    for (s32 round = 0; round < 3; ++round) {
        reachBlend(player);
        const PlayerAnimator baseline = player;
        for (const auto deed : kInterrupts) {
            CAPTURE(round, static_cast<u32>(deed));
            PlayerAnimator native = baseline;
            PlayerAnimator replay = baseline;
            sameAnimator(replay, native);
            for (s32 tick = 0; tick < 12; ++tick) {
                const auto request = tick == 0 ? deed : PlayerDeed::None;
                native.update(PlayerMotion::Stand, 1, kStep, request);
                replay.update(PlayerMotion::Stand, 1, kStep, request);
                sameAnimator(replay, native);
                // Rewind again while the old blend is still live, including the ComboHeld
                // early branch which may show an unblended pose, after a prediction that ran
                // on into another action.
                const PlayerAnimator saved = replay;
                replay.update(PlayerMotion::Run, 1, kStep, PlayerDeed::Attack);
                diverged += replay.checkpoint() == native.checkpoint() ? 0 : 1;
                replay = saved;
                sameAnimator(replay, native);
            }
        }
        player.update(PlayerMotion::Run, 1, kStep);
    }
    CHECK(diverged > 0);
}

/** A run, a stance, a swing, a guard and a flinch, every 180 ticks. */
std::pair<PlayerMotion, PlayerDeed> poseInput(s32 tick) {
    const s32 phase = tick % 180;
    if (phase < 35) {
        return {PlayerMotion::Run, PlayerDeed::None};
    }
    if (phase == 95) {
        return {PlayerMotion::Stand, PlayerDeed::Attack};
    }
    if (phase == 115) {
        return {PlayerMotion::Stand, PlayerDeed::Defend};
    }
    if (phase == 135) {
        return {PlayerMotion::Stand, PlayerDeed::Flinch};
    }
    return {PlayerMotion::Stand, PlayerDeed::None};
}

TEST_CASE("native class and Sumner costume poses survive rewind and replay",
          "[rollback][player-local-rewind][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    CAPTURE(character);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = character;
    if (character == kSumnerClass) {
        // Sumner's playable artwork is the WIZ/SUM name-code costume,
        // not a SUM/YEL class-colour archive.
        save.name = "SUM224";
        REQUIRE(applyNameCheats(save));
    }
    auto figure = PlayerFigure::load(device, root, save, Entrance::Skip);
    REQUIRE(figure);
    REQUIRE(figure->actionTree());
    constexpr s32 kLeads = 5;
    constexpr std::array kParts{"HEAD", "L_HAND", "R_HAND"};
    const auto placement =
        glm::translate(glm::rotate(Mat4{1}, 0.43f, Vec3{0, 1, 0}), Vec3{2.1f, 1.7f, -4.2f});
    const auto lead = [&](s32 tick, s32 ahead) {
        auto [motion, deed] = poseInput(tick + ahead);
        if (ahead == 2 && tick % 47 == 0) {
            deed = PlayerDeed::TurboStrong;
        }
        figure->animate(motion == PlayerMotion::Run ? 1.0f : 0.0f, 1, kStep, deed);
    };
    usize blends = 0;
    usize transitions = 0;
    u64 lastTransition = 0;
    usize diverged = 0;
    for (s32 tick = 0; tick < 360; ++tick) {
        CAPTURE(tick);
        const auto [motion, deed] = poseInput(tick);
        figure->animate(motion == PlayerMotion::Run ? 1.0f : 0.0f, 1, kStep, deed);
        const auto& live = figure->animator();
        if (live.transitionPose()) {
            ++blends;
            if (lastTransition != live.player().generation()) {
                ++transitions;
                lastTransition = live.player().generation();
            }
        }
        PlayerFigure::LocalSnapshot saved;
        figure->saveLocal(saved);
        const PlayerAnimator now = live;
        const auto rootPlace = figure->rootAttachment(placement);
        std::array<std::optional<Mat4>, kParts.size()> parts;
        for (usize part = 0; part < kParts.size(); ++part) {
            parts[part] = figure->attachment(placement, kParts[part]);
        }
        // The prediction runs five frames ahead, a turbo strike among them now and then; its
        // poses are what the replay after the rewind must show again.
        std::vector<PlayerAnimator> ahead;
        for (s32 step = 1; step <= kLeads; ++step) {
            lead(tick, step);
            ahead.push_back(figure->animator());
        }
        diverged += figure->animator().checkpoint() == now.checkpoint() ? 0U : 1U;
        REQUIRE(figure->restoreLocal(saved));
        sameAnimator(figure->animator(), now);
        CHECK(figure->rootAttachment(placement) == rootPlace);
        for (usize part = 0; part < kParts.size(); ++part) {
            CHECK(figure->attachment(placement, kParts[part]) == parts[part]);
        }
        for (s32 step = 1; step <= kLeads; ++step) {
            lead(tick, step);
            sameAnimator(figure->animator(), ahead[static_cast<usize>(step - 1)]);
        }
        REQUIRE(figure->restoreLocal(saved));
    }
    CHECK(blends > 0);
    CHECK(transitions >= 2);
    CHECK(diverged > 0);
}

TEST_CASE("measure native four-player value and action copy before scene integration",
          "[.][local-rewind-timing][player-local-rewind-timing][assets]") {
    PartyRewind party{true};
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    ItemArchive powerups;
    ItemArchive weapons;
    REQUIRE(powerups.load(root / "POWERUPS"));
    REQUIRE(weapons.load(root / "WEAPONS"));
    constexpr std::array kForms{powerup::kPojo, powerup::kPhoenix, powerup::kLevitation,
                                powerup::kFireBreath};
    for (usize seat = 0; seat < kSeats; ++seat) {
        auto& player = party.players[seat];
        auto& inventory = player.actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, kForms[seat], 0, 60);
        player.figure->setCompanionPowerups(party.device, powerups, inventory, &weapons);
        REQUIRE(player.figure->companion().shown());
        REQUIRE(player.figure->familiarTier() > 0);
    }
    LocalStateHistory<PartyRewind::Saved> history;
    for (s32 tick = 0; tick < 50; ++tick) {
        party.step(tick);
    }
    using Clock = std::chrono::steady_clock;
    const auto capture = [&](PartyRewind::Saved& state) {
        party.save(state);
        return true;
    };
    const auto coldStart = Clock::now();
    const bool coldSaved = history.capture(0, capture);
    const auto coldEnd = Clock::now();
    REQUIRE(coldSaved);
    for (u64 tick = 1; tick < LocalStateHistory<PartyRewind::Saved>::kCapacity; ++tick) {
        REQUIRE(history.capture(tick, capture));
    }
    std::vector<f64> saves;
    std::vector<f64> restores;
    std::vector<f64> replays;
    for (s32 sample = 0; sample < 256; ++sample) {
        const auto frame =
            static_cast<u64>(sample) + LocalStateHistory<PartyRewind::Saved>::kCapacity;
        const auto saveStart = Clock::now();
        const bool copied = history.capture(frame, capture);
        const auto saveEnd = Clock::now();
        REQUIRE(copied);
        const auto* saved = history.find(frame);
        REQUIRE(saved);
        for (s32 tick = 50; tick < 58; ++tick) {
            party.step(tick);
        }
        const auto restoreStart = Clock::now();
        const bool restored = party.restore(*saved);
        const auto restoreEnd = Clock::now();
        REQUIRE(restored);
        const auto replayStart = Clock::now();
        for (s32 tick = 50; tick < 58; ++tick) {
            party.step(tick);
        }
        const auto replayEnd = Clock::now();
        saves.push_back(std::chrono::duration<f64, std::milli>(saveEnd - saveStart).count());
        restores.push_back(
            std::chrono::duration<f64, std::milli>(restoreEnd - restoreStart).count());
        replays.push_back(std::chrono::duration<f64, std::milli>(replayEnd - replayStart).count());
        REQUIRE(party.restore(*saved));
    }
    const auto report = [](const char* operation, const std::vector<f64>& samples) {
        const auto summary = test::summarizeNativeTimings(samples);
        REQUIRE(summary);
        std::cout << "party-local-" << operation << " samples=" << summary->samples
                  << " mean_ms=" << summary->mean << " p99_ms=" << summary->p99
                  << " max_ms=" << summary->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
    std::cout << "party-local-cold-save ms="
              << std::chrono::duration<f64, std::milli>(coldEnd - coldStart).count() << '\n'
              << "party-local-state history_slots="
              << LocalStateHistory<PartyRewind::Saved>::kCapacity
              << " inline_bytes_per_slot=" << sizeof(PartyRewind::Saved)
              << " (excludes dynamic buffers; party, companions and camera, not a scene budget)\n";
}

// The actor and the melee streak rewind with the participant's simulation, which holds
// them by value; health cues rewind with the party's PlayerHealth, copied the same way.

CollisionTriangle face(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                       s32 object = -1) {
    return {normal, {a, b, c}, object};
}

void slopedFloor(std::vector<CollisionTriangle>& triangles, f32 left, f32 right, f32 leftY,
                 f32 rightY, s32 object = -1) {
    const Vec3 normal = glm::normalize(Vec3{leftY - rightY, right - left, 0});
    triangles.push_back(
        face({left, leftY, -10}, {right, rightY, 10}, {right, rightY, -10}, normal, object));
    triangles.push_back(
        face({left, leftY, -10}, {left, leftY, 10}, {right, rightY, 10}, normal, object));
}

WorldCollision walledRoom() {
    std::vector<CollisionTriangle> triangles;
    slopedFloor(triangles, -20, 20, 0, 0, 0);
    triangles.push_back(face({5, 0, -10}, {5, 8, -10}, {5, 8, 10}, {-1, 0, 0}, 1));
    triangles.push_back(face({5, 0, -10}, {5, 8, 10}, {5, 0, 10}, {-1, 0, 0}, 1));
    triangles.push_back(face({-10, 0, 6}, {10, 8, 6}, {-10, 8, 6}, {0, 0, -1}, 2));
    triangles.push_back(face({-10, 0, 6}, {10, 0, 6}, {10, 8, 6}, {0, 0, -1}, 2));
    WorldCollision result;
    result.build(triangles);
    return result;
}

void sameActor(const PlayerActor& replay, const PlayerActor& native) {
    CHECK(replay == native);
    CHECK(replay.speed() == native.speed());
    CHECK(replay.facing() == native.facing());
    CHECK(replay.followPoint() == native.followPoint());
    CHECK(replay.attentionPoint() == native.attentionPoint());
    CHECK(replay.transform() == native.transform());
}

TEST_CASE("actor rewind resumes both directions across native slopes seams and stair landings",
          "[game][players][player-local-rewind][terrain-seams][rollback]") {
    const bool stairs = GENERATE(false, true);
    const f32 direction = GENERATE(-1.0f, 1.0f);
    CAPTURE(stairs, direction);
    std::vector<CollisionTriangle> triangles;
    if (stairs) {
        slopedFloor(triangles, -10, -2, 0, 0);
        slopedFloor(triangles, -2, 0, 0.75f, 0.75f);
        slopedFloor(triangles, 0, 2, 1.5f, 1.5f);
        slopedFloor(triangles, 2, 10, 2.25f, 2.25f);
    } else {
        slopedFloor(triangles, -10, 0, -5, 0);
        slopedFloor(triangles, 0.02f, 10, 0.01f, 5);
    }
    WorldCollision geometry;
    geometry.build(triangles);
    const f32 startX = -5 * direction;
    const f32 startY = stairs ? (direction > 0 ? 0 : 2.25f) : startX * 0.5f;
    PlayerActor native;
    native.spawn(0, {}, nullptr, {startX, startY, 0}, 0.75f);
    native.setPaceBonus(1.25f);
    PlayerActor replay = native;
    for (s32 tick = 0; tick < 96; ++tick) {
        CAPTURE(tick);
        const PlayerActor before = native;
        if (tick % 7 == 0) {
            // A rewind replaces every local field, not only feet/yaw. No velocity or
            // fall clock is invented by restoring.
            replay.place({80, 90, 100});
            replay.setPaceBonus(-20);
            replay = before;
        }
        native.clearWallContacts();
        replay.clearWallContacts();
        const MoveInput move{{direction, 0}, 1};
        native.update(move, 0, kStep, &geometry, 1, (tick % 3) == 0);
        replay.update(move, 0, kStep, &geometry, 1, (tick % 3) == 0);
        CHECK(replay.fall(kStep, geometry) == native.fall(kStep, geometry));
        sameActor(replay, native);
        // PlayerMotion shortens a climbing step by the rise: a stair riser takes most of
        // its frame, and a slope a tenth of the pace throughout.
        REQUIRE((native.position().x - before.position().x) * direction > (stairs ? 0.0f : 0.08f));
        if (!stairs) {
            CHECK(native.position().y == Catch::Approx(native.position().x * 0.5f).margin(0.02f));
        }
    }
    CHECK(native.position().x * direction > (stairs ? 4.4f : 3.9f));
    CHECK(std::abs(native.position().y - startY) > 2);
}

TEST_CASE("actor rewind resumes midfall and changing floor availability without resetting progress",
          "[game][players][player-local-rewind][falling][rollback]") {
    const f32 rate = GENERATE(30.0f, 60.0f, 120.0f);
    const f32 seconds = 1 / rate;
    auto geometry = walledRoom();
    PlayerActor native;
    native.spawn(0, {}, nullptr, {0, 10, 0}, 1.25f);
    for (s32 tick = 0; tick < 7; ++tick) {
        REQUIRE(native.fall(seconds, geometry));
    }
    REQUIRE(native.position().y > 0);
    REQUIRE(native.position().y < 10);
    PlayerActor replay = native;
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(rate, tick);
        if (tick == 80) {
            geometry.clear(); // a disappearing support changes only the next floor query
        } else if (tick == 100) {
            std::vector<CollisionTriangle> raised;
            slopedFloor(raised, -20, 20, native.position().y + 1, native.position().y + 1);
            geometry.build(raised);
        }
        if (tick % 11 == 0) {
            replay.turnTo(17);
            replay = native;
        }
        const f32 before = native.position().y;
        const bool nativeFalling = native.fall(seconds, geometry);
        CHECK(replay.fall(seconds, geometry) == nativeFalling);
        sameActor(replay, native);
        CHECK(native.position().y >= before - PlayerActor::kFallSpeed * seconds - 0.00001f);
        if (tick == 79) {
            CHECK(native.position().y == 0);
        } else if (tick == 99) {
            CHECK(native.position().y < 0);
        } else if (tick == 100) {
            CHECK_FALSE(nativeFalling);
            CHECK(native.position().y == Catch::Approx(before + 1));
        }
    }
}

TEST_CASE("health cue rewind preserves the existing random sequence without emitting on restore",
          "[rollback][player-local-rewind]") {
    PlayerHealth health;
    std::mt19937 reference{0x5A17u};
    std::vector<std::string> cries;
    PlayerHealth::Events events{
        .block = [](f32, f32) { FAIL("unexpected guard"); },
        .sound = [](std::string_view) {},
        .cry = [&](std::string_view cue) { cries.emplace_back(cue); },
        .named = [](std::string_view, f32) { FAIL("unexpected low-health line"); }};
    const auto burn = [&] {
        PlayerRuntime player;
        CharacterSave save;
        save.progress().health = 1000;
        player.actor.spawn(0, save, nullptr, Vec3{0}, 0);
        health.hurt(player, 1, HurtKind::Burn, HurtSource::Undirected, {}, events);
        REQUIRE(player.actor.save().health() == 999);
    };
    for (s32 index = 0; index < 257; ++index) {
        burn();
        CHECK(cries.back() == "PAIN" + std::to_string(1 + reference() % 4));
    }
    const PlayerHealth saved = health;
    cries.clear();
    for (s32 index = 0; index < 700; ++index) {
        burn();
        CHECK(cries.back() == "PAIN" + std::to_string(1 + reference() % 4));
    }
    const auto expected = cries;
    cries.clear();
    health = saved;
    CHECK(cries.empty());
    CHECK(health == saved);
    for (s32 index = 0; index < 700; ++index) {
        burn();
    }
    CHECK(cries == expected);
}

TEST_CASE("melee praise rewind restores its exact count and fractional quiet timer",
          "[rollback][player-local-rewind]") {
    PlayerRuntime native;
    for (s32 hit = 0; hit < 15; ++hit) {
        native.streak.record(true);
    }
    REQUIRE(native.streak.step(1.25f).empty());
    const PlayerRuntime::Simulation saved = native.simulation();
    PlayerRuntime replay;
    replay.simulation() = saved;
    CHECK(replay.streak == native.streak);
    CHECK(native.streak.step(0.5f).empty());
    CHECK(replay.streak.step(0.5f).empty());
    // Past the original's two seconds of quiet.
    CHECK(native.streak.step(0.26f) == MeleeStreak::kBraveryVoice);
    CHECK(replay.streak.step(0.26f) == MeleeStreak::kBraveryVoice);
    CHECK(replay.streak == native.streak);
    CHECK(replay.streak.step(4).empty());
    replay.simulation() = saved;
    CHECK(replay.streak == saved.streak);
}
} // namespace
