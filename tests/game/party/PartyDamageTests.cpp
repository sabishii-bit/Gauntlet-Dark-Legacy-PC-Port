#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/hud/HelpMessages.h"
#include "game/party/ActionObservation.h"
#include "game/party/PartyDamage.h"
#include "game/party/PartyMotion.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerRuntime.h"
#include "game/party/ScriptedMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;
enum class OutputKind { Sound, Cry, Named, Rumble, Block };
struct Output {
    OutputKind kind;
    s32 player;
    std::string name;
    f32 seconds = 0;
    s32 frames = 0;
    bool operator==(const Output&) const = default;
};
constexpr f32 kStep = 1.0f / 60;

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    WorldCollision floor;
    ItemArchive weapons; // Bound but deliberately empty: block gameplay is not its VFX.
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 2> players;
    PlayerHealth health;
    std::vector<Output> outputs;
    std::vector<PartyMotion::Action> actions;
    usize lessons = 0;

    Fixture() {
        REQUIRE(classes.load(root / "PDATA"));
        floor.build({{{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}},
                     {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}}});
        arsenal.bind({device, classes, weapons, floor, effects, audio, nullptr, {}, false, false});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        constexpr std::array kSlots{3, 1};
        constexpr std::array kClasses{0, 6};
        for (usize i = 0; i < players.size(); ++i) {
            auto& player = players[i];
            CharacterSave save;
            save.name = "TEST";
            save.character = kClasses[i];
            save.progress().health = 10000;
            const auto* stats = classes.stats(save.character);
            REQUIRE(stats);
            player.actor.spawn(kSlots[i], save, stats, {i == 0 ? -12.0f : 12.0f, 0, 0}, 0);
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(player.figure);
            player.turbo.add(100);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        attacks.clear();
        arsenal.clear();
    }
    /** What one member shows: its simulation values and its figure's action state. */
    struct Observed {
        PlayerSimulation player;
        test::ActionObservation figure;
        bool operator==(const Observed&) const = default;
    };
    std::array<Observed, 2> state() const {
        std::array<Observed, 2> result;
        for (usize i = 0; i < players.size(); ++i) {
            result[i] = {players[i].simulation(), test::observeAction(*players[i].figure)};
        }
        return result;
    }
    /** Where the party rewinds to: each member's simulation values and figure. */
    struct Saved {
        std::array<PlayerSimulation, 2> players;
        std::array<PlayerFigure::LocalSnapshot, 2> figures;
    };
    Saved save() const {
        Saved saved;
        for (usize i = 0; i < players.size(); ++i) {
            saved.players[i] = players[i].simulation();
            players[i].figure->saveLocal(saved.figures[i]);
        }
        return saved;
    }
    void restore(const Saved& saved, const PlayerHealth& random) {
        for (usize i = 0; i < players.size(); ++i) {
            REQUIRE(players[i].figure->restoreLocal(saved.figures[i]));
            players[i].simulation() = saved.players[i];
        }
        health = random;
    }
    PlayerHealth::Events events(usize index, const PartyDamage::Context& context) {
        const s32 slot = players[index].actor.player();
        return {.block =
                    [this, index, slot](f32 taken, f32 left) {
                        const f32 before = players[index].blockLeft;
                        attacks.showBlock(index, taken, left, players);
                        if (players[index].blockLeft != before) {
                            outputs.push_back(
                                {OutputKind::Block, slot, {}, players[index].blockLeft});
                        }
                    },
                .sound =
                    [this, slot](std::string_view name) {
                        outputs.push_back({OutputKind::Sound, slot, std::string{name}});
                    },
                .cry =
                    [this, slot](std::string_view name) {
                        outputs.push_back({OutputKind::Cry, slot, std::string{name}});
                    },
                .named =
                    [this, slot](std::string_view name, f32 wait) {
                        outputs.push_back({OutputKind::Named, slot, std::string{name}, wait});
                    },
                .learnBlock =
                    [this, playSeconds = context.playSeconds] {
                        if (playSeconds > 60) {
                            ++lessons;
                        }
                    },
                .vibrate =
                    [this, slot](s32 frames) {
                        outputs.push_back({OutputKind::Rumble, slot, {}, 0, frames});
                    },
                .logDeaths = false};
    }
    // Independent oracle for the old LevelOpponents -> PlayScene -> PlayerHealth
    // sequence. In particular cue optionality, party-vector order, and scene
    // locks are not inferred from PartyDamage's result.
    void native(std::span<const EnemyBlow> blows, const PartyDamage::Context& context,
                std::span<const LevelEnemy> roster = {}) {
        for (const auto& blow : blows) {
            REQUIRE(blow.ward == EnemyMeleeWard::None);
            for (usize index = 0; index < players.size(); ++index) {
                auto& player = players[index];
                if (player.actor.player() != blow.player || !PlayerHealth::canBeDamaged(player)) {
                    continue;
                }
                const auto cue = EnemyFeedback::meleeSound(blow.kind, blow.tier, roster);
                if (cue) {
                    outputs.push_back({OutputKind::Sound, blow.player, *cue});
                }
                if (context.cutscene) {
                    continue;
                }
                const bool stood = player.life == PlayerLife::Standing;
                health.hurt(player, blow.damage, cue ? HurtKind::QuietBlow : HurtKind::Blow,
                            HurtSource::Directed, context.setting, events(index, context),
                            {blow.flags, blow.direction},
                            classes.stats(player.actor.save().character));
                if (stood && player.life == PlayerLife::Dying && context.lastRealmLevel) {
                    player.entrySave.progress().levels.recordBossDeath(context.realm);
                }
            }
        }
    }
    void dispatch(std::span<const EnemyBlow> blows, const PartyDamage::Context& context,
                  std::span<const LevelEnemy> roster = {}) {
        for (const auto& blow : blows) {
            for (usize index = 0; index < players.size(); ++index) {
                auto& player = players[index];
                const auto impact = PartyDamage::enemy(blow, player, roster);
                if (!impact) {
                    continue;
                }
                if (impact->sound) {
                    outputs.push_back({OutputKind::Sound, blow.player, *impact->sound});
                }
                PartyDamage::hurt(health, player, blow.damage, impact->kind, HurtSource::Directed,
                                  context, events(index, context), impact->impact,
                                  classes.stats(player.actor.save().character));
            }
        }
    }
    void tick(s32 tick, bool defend = false) {
        std::array<PlayInput, 4> input{};
        for (const auto& player : players) {
            auto& current = input[static_cast<usize>(player.actor.player())];
            if (defend) {
                // Native guard is edge-triggered. Repeating the edge every
                // tick would restart DefendRaise before the blocking phase.
                current.defendPressed = tick == 0;
            } else if (tick >= 40) {
                current.move = {Vec2{tick % 40 < 20 ? 0.6f : -0.6f, 0.4f}, 0.72f};
            }
        }
        const test::MotionScript callbacks{
            .perform = [this](usize, PartyMotion::Action action) { actions.push_back(action); },
            .select = [](usize, const SelectorInput&, s32) {},
            .advanceTurbo =
                [this](usize index, s32 ticks, f32 seconds) {
                    attacks.updateTurbo(index, ticks, seconds, players,
                                        [this](s32, usize) { ++lessons; });
                }};
        test::ScriptedMotion motionLevel{callbacks};
        PartyMotion::step(players, input, false, 0, 1, kStep, floor, motionLevel);
    }
};

TEST_CASE("native enemy damage dispatch preserves party damage and following motion",
          "[diagnostics][party-damage][assets]") {
    Fixture fixture;
    Enemies enemies;
    enemies.open(fixture.device, fixture.root, &fixture.floor, 8, {}, 79);
    REQUIRE(enemies.loadKind(kGruntKind));
    for (const auto& player : fixture.players) {
        REQUIRE(enemies.spawn({.kind = kGruntKind,
                               .tier = 3,
                               .position = player.actor.position() + Vec3{0, 0, 3},
                               .placed = true},
                              {}));
    }
    std::vector<EnemyBlow> blows;
    bool power = false;
    u32 victims = 0;
    for (s32 tick = 0; tick < 4800 && (!power || victims != ((1U << 3) | (1U << 1))); ++tick) {
        enemies.update(1, kStep, LevelOpponents::enemyViews(fixture.players));
        for (const auto& blow : enemies.takeBlows()) {
            power |= (blow.flags & damage::kKnockBack) != 0;
            victims |= 1U << blow.player;
            blows.push_back(blow);
        }
    }
    REQUIRE_FALSE(blows.empty());
    REQUIRE(power);
    REQUIRE(victims == ((1U << 3) | (1U << 1)));
    const auto saved = fixture.save();
    const auto before = fixture.state();
    const auto random = fixture.health;
    const auto uploads = fixture.device.texturesCreated;
    const PartyDamage::Context context{.setting = {.damageScale = 1.125f}, .playSeconds = 75};
    fixture.native(blows, context);
    const auto afterDamage = fixture.state();
    REQUIRE(std::ranges::any_of(afterDamage, [](const auto& member) {
        return member.player.reaction == PlayerDeed::Flinch;
    }));
    REQUIRE(afterDamage[0].player.actor.save().health() < before[0].player.actor.save().health());
    REQUIRE(afterDamage[1].player.actor.save().health() < before[1].player.actor.save().health());
    CHECK(afterDamage[0].figure == before[0].figure); // queued, not animated during damage
    CHECK(afterDamage[1].figure == before[1].figure);
    const auto afterRandom = fixture.health;
    const auto outputs = fixture.outputs;
    std::vector<std::array<Fixture::Observed, 2>> continuation;
    for (s32 tick = 0; tick < 90; ++tick) {
        fixture.tick(tick);
        continuation.push_back(fixture.state());
    }
    const auto actions = fixture.actions;
    CHECK(continuation.front()[0].player.reaction == PlayerDeed::None);
    REQUIRE(continuation.back()[0].player.actor.position() != before[0].player.actor.position());
    fixture.restore(saved, random);
    fixture.actions.clear();
    fixture.outputs.clear();
    fixture.dispatch(blows, context);
    CHECK(fixture.outputs == outputs);
    CHECK(fixture.state() == afterDamage);
    CHECK(fixture.health == afterRandom);
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick);
        fixture.tick(tick);
        CHECK(fixture.state() == continuation[static_cast<usize>(tick)]);
    }
    CHECK(fixture.actions == actions);
    CHECK(fixture.effects.count() == 0);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("native guard damage preserves block state fractions and global cue RNG order",
          "[diagnostics][party-damage][assets]") {
    const bool defend = GENERATE(false, true);
    Fixture fixture;
    if (defend) {
        for (s32 tick = 0;
             tick < 60 && !std::ranges::all_of(fixture.players,
                                               [](const auto& player) {
                                                   return player.figure->animator().defending();
                                               });
             ++tick) {
            fixture.tick(tick, true);
        }
        REQUIRE(fixture.players[0].figure->animator().defending());
        REQUIRE(fixture.players[1].figure->animator().defending());
    }
    fixture.players[0].actor.save().progress().health = 55;
    fixture.players[1].actor.save().progress().health = 155;
    fixture.players[0].healthFraction = -0.25f;
    fixture.players[1].healthFraction = 0.375f;
    const std::array blows{EnemyBlow{.player = 1, .kind = 0, .damage = 19.75f},
                           EnemyBlow{.player = 3, .kind = 0, .damage = 19.25f},
                           EnemyBlow{.player = 1, .kind = 0, .damage = 11.125f}};
    const PartyDamage::Context context{.setting = {.damageScale = 1.125f}};
    const auto saved = fixture.save();
    const auto random = fixture.health;
    fixture.native(blows, context);
    const auto expected = fixture.state();
    const auto expectedRandom = fixture.health;
    const auto cues = fixture.outputs;
    REQUIRE_FALSE(cues.empty());
    CHECK(expectedRandom != random); // below 50 chooses a native party-wide random line
    CHECK(expected[0].player.blocked == defend);
    if (defend) {
        REQUIRE(expected[0].player.blockLeft > 0);
        CHECK(std::ranges::count(cues, OutputKind::Block, &Output::kind) == 2);
    }
    fixture.restore(saved, random);
    fixture.outputs.clear();
    fixture.dispatch(blows, context);
    CHECK(fixture.outputs == cues);
    CHECK(fixture.state() == expected);
    CHECK(fixture.health == expectedRandom);
}

TEST_CASE("ordinary knockback retains native damage and reaction without requesting guard lessons",
          "[diagnostics][party-damage][help-admission][assets]") {
    const bool known = GENERATE(false, true);
    Fixture fixture;
    if (known) {
        fixture.players[0].actor.save().helpSeen = {HelpMessages::kLearnBlock};
    }
    const auto saved = fixture.save();
    const auto before = fixture.state();
    const auto random = fixture.health;
    const std::array blows{EnemyBlow{.player = 3,
                                     .kind = kGruntKind,
                                     .damage = 70,
                                     .flags = damage::kKnockBack,
                                     .direction = {0, 0, 1}}};
    const PartyDamage::Context context{.playSeconds = 75};
    fixture.native(blows, context);
    // Native learning requires a heavy impact; ordinary swarm knock-back is
    // deliberately not a heavy flag, regardless of the damage amount.
    REQUIRE(fixture.lessons == 0);
    const auto expected = fixture.state();
    const auto expectedRandom = fixture.health;
    const auto outputs = fixture.outputs;
    REQUIRE(expected[0].player.actor.save().health() < before[0].player.actor.save().health());
    REQUIRE(expected[0].player.reaction != PlayerDeed::None);
    for (usize index = 0; index < fixture.players.size(); ++index) {
        CHECK(expected[index].player.actor.save().helpSeen ==
              before[index].player.actor.save().helpSeen);
        CHECK(expected[index].player.helpHeard == before[index].player.helpHeard);
    }
    fixture.tick(0);
    const auto following = fixture.state();
    fixture.restore(saved, random);
    fixture.outputs.clear();
    fixture.dispatch(blows, context);
    CHECK(fixture.outputs == outputs);
    CHECK(fixture.state() == expected);
    CHECK(fixture.health == expectedRandom);
    fixture.tick(0);
    CHECK(fixture.state() == following);
    CHECK(fixture.lessons == 0);
}

TEST_CASE("native heavy damage preserves knockdowns and guard lesson routing",
          "[diagnostics][party-damage][help-admission][assets]") {
    const bool known = GENERATE(false, true);
    Fixture fixture;
    if (known) {
        fixture.players[0].actor.save().helpSeen = {HelpMessages::kLearnBlock};
    }
    const auto saved = fixture.save();
    const auto before = fixture.state();
    const auto random = fixture.health;
    const std::array blows{EnemyBlow{.player = 3,
                                     .kind = kGruntKind,
                                     .damage = 70,
                                     .flags = damage::kKnockDown,
                                     .direction = {0, 0, 1}}};
    const PartyDamage::Context context{.playSeconds = 75};
    fixture.native(blows, context);
    REQUIRE(fixture.lessons == 1);
    REQUIRE(fixture.players[0].actor.save().health() < before[0].player.actor.save().health());
    REQUIRE(fixture.players[0].reaction != PlayerDeed::None);
    const auto expected = fixture.state();
    const auto expectedRandom = fixture.health;
    const auto outputs = fixture.outputs;
    MessageTable strings;
    REQUIRE(strings.load(fixture.root / "TEXT/english.rom"));
    HelpMessages help;
    help.setTexts(&strings);
    std::array<HelpReader, 2> readers;
    for (usize index = 0; index < readers.size(); ++index) {
        auto& player = fixture.players[index];
        readers[index] = {player.actor.player(), &player.actor.save().helpSeen, &player.helpHeard};
    }
    CHECK((help.post(HelpMessages::kLearnBlock, 3, readers) == nullptr) == known);
    CHECK(help.showing() == !known);
    fixture.restore(saved, random);
    fixture.outputs.clear();
    fixture.lessons = 0;
    fixture.dispatch(blows, context);
    CHECK(fixture.state() == expected);
    CHECK(fixture.health == expectedRandom);
    CHECK(fixture.outputs == outputs);
    CHECK(fixture.lessons == 1);
}

TEST_CASE("native impact cue optionality and locks are retained before damage dispatch",
          "[diagnostics][party-damage][assets]") {
    const s32 lock = GENERATE(0, 1, 2);
    Fixture fixture;
    const std::array blows{EnemyBlow{.player = 0, .kind = kGruntKind, .damage = 9},
                           EnemyBlow{.player = 3, .kind = kRatKind, .damage = 9},
                           EnemyBlow{.player = 1, .kind = kGruntKind, .damage = 9}};
    const auto emptyName = PartyDamage::enemy(blows[1], fixture.players[0], {});
    REQUIRE(emptyName);
    REQUIRE(emptyName->sound);
    CHECK(emptyName->sound->empty());
    CHECK(emptyName->kind == HurtKind::QuietBlow);
    CHECK_FALSE(PartyDamage::enemy(blows[0], fixture.players[0], {}));
    const PartyDamage::Context context{.setting = {.tower = lock == 1}, .cutscene = lock == 2};
    const auto saved = fixture.save();
    const auto before = fixture.state();
    const auto random = fixture.health;
    fixture.native(blows, context);
    const auto expected = fixture.state();
    const auto cues = fixture.outputs;
    fixture.restore(saved, random);
    fixture.outputs.clear();
    fixture.dispatch(blows, context);
    CHECK(fixture.outputs == cues);
    CHECK(fixture.state() == expected);
    if (lock != 0) {
        CHECK(expected == before);
        REQUIRE(cues.size() == 2);
        CHECK(cues[0].kind == OutputKind::Sound);
        CHECK(cues[0].name.empty());
        CHECK(cues[1].name == "S_PLYRDMG5");
    }
}

TEST_CASE("shared scene hurt preserves the strict lesson threshold and last level death record",
          "[diagnostics][party-damage][assets]") {
    const f32 elapsed = GENERATE(60.0f, 60.01f);
    Fixture fixture;
    const PartyDamage::Context context{.lastRealmLevel = true, .realm = 1, .playSeconds = elapsed};
    auto& player = fixture.players[0];
    auto events = fixture.events(0, context);
    events.learnBlock = [&] { ++fixture.lessons; };
    PartyDamage::hurt(fixture.health, player, 70, HurtKind::Blow, HurtSource::Directed, context,
                      events, {damage::kKnockDown, {0, 0, 1}},
                      fixture.classes.stats(player.actor.save().character));
    CHECK(fixture.lessons == (elapsed > 60 ? 1 : 0));
    const auto expected = player.entrySave.progress().levels;
    auto recorded = expected;
    recorded.recordBossDeath(1);
    PartyDamage::hurt(fixture.health, player, 20000, HurtKind::Blow, HurtSource::Directed, context,
                      fixture.events(0, context), {},
                      fixture.classes.stats(player.actor.save().character));
    REQUIRE(player.life == PlayerLife::Dying);
    CHECK(player.entrySave.progress().levels == recorded);
    CHECK(recorded != expected);
}
} // namespace
