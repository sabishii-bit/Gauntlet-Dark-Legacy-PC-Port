#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/enemies/DeathRules.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/Generators.h"
#include "game/enemies/ImportedOpponentCues.h"
#include "game/enemies/OpponentCues.h"
#include "game/enemies/OpponentPool.h"
#include "game/level/ImportedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

using Log = std::vector<std::string>;

static_assert(std::is_base_of_v<OpponentCues, ImportedOpponentCues>);
static_assert(std::is_final_v<ImportedOpponentCues>);

/** An imported level that owns every cue it is asked about, or none, logging each question. */
class RecordingLevel final : public ImportedLevel {
public:
    RecordingLevel(Log& log, bool owns) : m_log(log), m_owns(owns) {}
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    bool playOpponentSound(std::string_view name, const Vec3& position) override {
        return ask(std::format("level sound {} at {}", name, position.x));
    }
    bool playEnemyFeedback(const EnemyFeedback& feedback, f32 health) override {
        return ask(std::format("level feedback {} health {}", feedback.kind, health));
    }
    bool playEnemyCue(const EnemyCue& cue) override {
        return ask(std::format("level cue {}", cue.enemyKind));
    }
    bool playEnemyMelee(s32 kind, s32 tier) override {
        return ask(std::format("level melee {} tier {}", kind, tier));
    }
    bool playDeathCue(const DeathEvent& cue) override {
        return ask(std::format("level death {}", static_cast<s32>(cue.kind)));
    }
    bool setDeathContact(bool active) override {
        return ask(std::format("level contact {}", active));
    }
    bool playGeneratorFeedback(const GeneratorEvent& event) override {
        return ask(std::format("level generator {}", event.generator));
    }

private:
    bool ask(std::string question) {
        m_log.push_back(std::move(question));
        return m_owns;
    }
    Log& m_log;
    bool m_owns;
};

/** The native cues, logging each cue with what came with it. */
class RecordingCues final : public OpponentCues {
public:
    explicit RecordingCues(Log& log) : m_log(log) {}
    void enemyCue(const EnemyCue& cue) override {
        m_log.push_back(std::format("native cue {}", cue.enemyKind));
    }
    void missileLanded(std::string_view sound, f32 level, const Vec3& at) override {
        m_log.push_back(std::format("native missile {} level {} at {}", sound, level, at.x));
    }
    void suicideBang(std::string_view sound, const Vec3& at) override {
        m_log.push_back(std::format("native bang {} at {}", sound, at.x));
    }
    void shotSound(const Shot& shot) override {
        m_log.push_back(std::format("native shot {} at {} instance {} effect {} phase {} pool {}",
                                    shot.name, shot.position.x, shot.instance, shot.effect,
                                    shot.phase, static_cast<s32>(shot.owner)));
    }
    void enemyFeedback(const EnemyFeedback& feedback, std::string_view sound,
                       const Vec3& at) override {
        m_log.push_back(std::format("native feedback {} {} at {}", feedback.kind, sound, at.x));
    }
    CueOwner enemyMelee(s32 kind, s32 tier, const std::optional<std::string>& sound) override {
        m_log.push_back(
            std::format("native melee {} tier {} {}", kind, tier, sound.value_or("unsounded")));
        return CueOwner::Native;
    }
    void generatorFeedback(const GeneratorEvent& event, std::string_view sound) override {
        m_log.push_back(std::format("native generator {} {}", event.generator, sound));
    }
    void combatantSound(std::string_view sound, const Vec3& at, bool attenuated,
                        CombatantPool pool) override {
        m_log.push_back(std::format("native combatant {} at {} attenuated {} pool {}", sound, at.x,
                                    attenuated, static_cast<s32>(pool)));
    }
    void deathCue(const DeathEvent& cue) override {
        m_log.push_back(std::format("native death {}", static_cast<s32>(cue.kind)));
    }
    CueOwner deathContact(bool contact) override {
        m_log.push_back(std::format("native contact {}", contact));
        return CueOwner::Native;
    }
    void deathContactReleased() override { m_log.emplace_back("native released"); }

private:
    Log& m_log;
};

/** Sends each cue once, in the order the interface lists them, every one of them a cue an
 * imported level is asked about; returns the melee's and the contact's owners. */
std::pair<CueOwner, CueOwner> sendEveryCue(OpponentCues& cues) {
    cues.enemyCue(EnemyCue{.kind = EnemyCue::Kind::Yell, .enemyKind = 9});
    cues.missileLanded("S_LOBBER_BOMB", 127, Vec3{1, 0, 0});
    cues.suicideBang("S_SUICIDE_BOMB", Vec3{2, 0, 0});
    cues.shotSound({.name = "S_SPIT",
                    .position = Vec3{3, 0, 0},
                    .instance = 41,
                    .effect = 5,
                    .phase = 3,
                    .owner = CombatantPool::Boss});
    cues.enemyFeedback(EnemyFeedback{.kind = 7, .remainingHealth = 12.5f}, "S_GRUNTHIT",
                       Vec3{4, 0, 0});
    const CueOwner melee = cues.enemyMelee(1, 2, std::string{"S_PLYRDMG4"});
    cues.generatorFeedback(GeneratorEvent{.generator = 6}, "S_GENHIT");
    cues.combatantSound("S_ROAR", Vec3{5, 0, 0}, true, CombatantPool::Boss);
    cues.deathCue(DeathEvent{.kind = DeathEvent::Kind::Exhausted});
    const CueOwner contact = cues.deathContact(true);
    cues.deathContactReleased();
    return {melee, contact};
}

TEST_CASE("on a native level every opponent cue goes straight to the native cues",
          "[game][level-opponents][imported-opponent-cues]") {
    Log log;
    RecordingCues native{log};
    ImportedOpponentCues cues{nullptr, native};
    const auto [melee, contact] = sendEveryCue(cues);
    CHECK(melee == CueOwner::Native);
    CHECK(contact == CueOwner::Native);
    CHECK(log == Log{"native cue 9", "native missile S_LOBBER_BOMB level 127 at 1",
                     "native bang S_SUICIDE_BOMB at 2",
                     "native shot S_SPIT at 3 instance 41 effect 5 phase 3 pool 1",
                     "native feedback 7 S_GRUNTHIT at 4", "native melee 1 tier 2 S_PLYRDMG4",
                     "native generator 6 S_GENHIT",
                     "native combatant S_ROAR at 5 attenuated true pool 1", "native death 4",
                     "native contact true", "native released"});
}

TEST_CASE("an imported level that owns a cue keeps it from the native cues",
          "[game][level-opponents][imported-opponent-cues]") {
    Log log;
    RecordingCues native{log};
    RecordingLevel level{log, true};
    ImportedOpponentCues cues{&level, native};
    const auto [melee, contact] = sendEveryCue(cues);
    CHECK(melee == CueOwner::Imported);
    CHECK(contact == CueOwner::Imported);
    CHECK(log == Log{"level cue 9", "level sound S_LOBBER_BOMB at 1",
                     "level sound S_SUICIDE_BOMB at 2", "level sound S_SPIT at 3",
                     "level feedback 7 health 12.5", "level melee 1 tier 2", "level generator 6",
                     "level sound S_ROAR at 5", "level death 4", "level contact true",
                     "level contact false"});
}

TEST_CASE("a cue the imported level refuses reaches the native cues once, after the level",
          "[game][level-opponents][imported-opponent-cues]") {
    Log log;
    RecordingCues native{log};
    RecordingLevel level{log, false};
    ImportedOpponentCues cues{&level, native};
    const auto [melee, contact] = sendEveryCue(cues);
    CHECK(melee == CueOwner::Native);
    CHECK(contact == CueOwner::Native);
    CHECK(log == Log{"level cue 9",
                     "native cue 9",
                     "level sound S_LOBBER_BOMB at 1",
                     "native missile S_LOBBER_BOMB level 127 at 1",
                     "level sound S_SUICIDE_BOMB at 2",
                     "native bang S_SUICIDE_BOMB at 2",
                     "level sound S_SPIT at 3",
                     "native shot S_SPIT at 3 instance 41 effect 5 phase 3 pool 1",
                     "level feedback 7 health 12.5",
                     "native feedback 7 S_GRUNTHIT at 4",
                     "level melee 1 tier 2",
                     "native melee 1 tier 2 S_PLYRDMG4",
                     "level generator 6",
                     "native generator 6 S_GENHIT",
                     "level sound S_ROAR at 5",
                     "native combatant S_ROAR at 5 attenuated true pool 1",
                     "level death 4",
                     "native death 4",
                     "level contact true",
                     "native contact true",
                     "level contact false",
                     "native released"});
}

TEST_CASE("an imported level is asked about Death's cue of every kind",
          "[game][level-opponents][imported-opponent-cues]") {
    const auto kind =
        GENERATE(DeathEvent::Kind::Drain, DeathEvent::Kind::Return, DeathEvent::Kind::MagicHeal,
                 DeathEvent::Kind::Killed, DeathEvent::Kind::Exhausted, DeathEvent::Kind::Awakened,
                 DeathEvent::Kind::Unmoved);
    const bool owns = GENERATE(false, true);
    Log log;
    RecordingCues native{log};
    RecordingLevel level{log, owns};
    ImportedOpponentCues cues{&level, native};
    cues.deathCue(DeathEvent{.kind = kind});
    const auto code = static_cast<s32>(kind);
    CHECK(log ==
          (owns ? Log{std::format("level death {}", code)}
                : Log{std::format("level death {}", code), std::format("native death {}", code)}));
}

TEST_CASE("an imported level sounds only the lobber's bomb, the boss's shots and its named sounds",
          "[game][level-opponents][imported-opponent-cues]") {
    Log log;
    RecordingCues native{log};
    RecordingLevel level{log, true};
    ImportedOpponentCues cues{&level, native};
    cues.missileLanded("S_ENEMYARROW", 127, Vec3{1, 0, 0});
    cues.shotSound({.name = "S_SPIT",
                    .position = Vec3{3, 0, 0},
                    .instance = 41,
                    .effect = 5,
                    .phase = 1,
                    .owner = CombatantPool::Critters});
    cues.combatantSound("S_ROAR", Vec3{5, 0, 0}, false, CombatantPool::Critters);
    cues.combatantSound("", Vec3{6, 0, 0}, true, CombatantPool::Boss);
    CHECK(log == Log{"native missile S_ENEMYARROW level 127 at 1",
                     "native shot S_SPIT at 3 instance 41 effect 5 phase 1 pool 0",
                     "native combatant S_ROAR at 5 attenuated false pool 0",
                     "native combatant  at 6 attenuated true pool 1"});
}

TEST_CASE("an imported level is asked about a melee or a generator without a sound of its own",
          "[game][level-opponents][imported-opponent-cues]") {
    const bool owns = GENERATE(false, true);
    Log log;
    RecordingCues native{log};
    RecordingLevel level{log, owns};
    ImportedOpponentCues cues{&level, native};
    CHECK(cues.enemyMelee(15, 3, std::nullopt) == (owns ? CueOwner::Imported : CueOwner::Native));
    cues.generatorFeedback(GeneratorEvent{.generator = 2}, "");
    CHECK(log == (owns ? Log{"level melee 15 tier 3", "level generator 2"}
                       : Log{"level melee 15 tier 3", "native melee 15 tier 3 unsounded",
                             "level generator 2", "native generator 2 "}));
}
} // namespace
