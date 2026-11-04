#include "game/party/PlayerRuntimeCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kMagnitude = 1'000'000;
bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= kMagnitude;
}
bool vector(const Vec3& value) {
    return scalar(value.x) && scalar(value.y) && scalar(value.z);
}
bool matrix(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!scalar(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}
bool counter(s32 value) {
    return value >= 0 && value <= 1'000'000'000;
}
bool timer(s32 value) {
    return value >= -1'000'000'000 && value <= 1'000'000'000;
}
bool identifier(s32 value) {
    return value >= -1 && value <= 65535;
}
bool progressValid(const ClassProgress& progress) {
    const auto& inventory = progress.inventory;
    const auto& relics = progress.relics;
    return counter(progress.experience) && counter(progress.gold) && counter(progress.health) &&
           progress.promotedLevel >= -1 && progress.promotedLevel <= kMaxLevel &&
           scalar(progress.fightAdd) && scalar(progress.armorAdd) && scalar(progress.magicAdd) &&
           scalar(progress.speedAdd) && (progress.unlocked >> kRealmCount) == 0 &&
           std::ranges::all_of(
               progress.crystals,
               [](s32 value) { return value >= -1'000'000 && value <= 1'000'000; }) &&
           inventory.keys >= 0 && inventory.keys <= Inventory::kMostKeys &&
           inventory.potions.size() <= Inventory::kMostPotions &&
           std::ranges::all_of(inventory.potions,
                               [](s32 kind) { return kind >= 0 && kind <= 4; }) &&
           std::ranges::all_of(inventory.powerups,
                               [](const PowerupSlot& slot) {
                                   // Vacated slots can retain their former kind, flags and selector
                                   // state.
                                   return scalar(slot.strength) && scalar(slot.charge) &&
                                          (slot.kind == 0
                                               ? !slot.held() && slot.charge == 0 && slot.flags == 0
                                               : slot.kind >= 5 && slot.kind <= 9);
                               }) &&
           (relics.runes >> Relics::kRuneCount) == 0 &&
           (relics.pendingRunes & ~relics.runes) == 0 &&
           (relics.pendingShards & ~relics.shards) == 0 &&
           (relics.pendingCeremonies & ~Relics::kTowerCeremonyMask) == 0 &&
           std::ranges::all_of(relics.gargoylePieces,
                               [](s32 value) { return value == -1 || counter(value); }) &&
           std::ranges::all_of(progress.levels.runeLevels,
                               [](u16 mask) { return (mask >> Relics::kRuneCount) == 0; }) &&
           counter(progress.lifetime.enemiesKilled) &&
           counter(progress.lifetime.generatorsDestroyed) && counter(progress.lifetime.goldFound) &&
           std::isfinite(progress.lifetime.playSeconds) && progress.lifetime.playSeconds >= 0 &&
           progress.lifetime.playSeconds <= 1e12 &&
           std::ranges::all_of(progress.legends,
                               [](const auto& campaign) { return campaign.valid(); });
}
bool saveValid(const CharacterSave& save) {
    return save.name.size() <= kCharacterNameLength &&
           std::ranges::all_of(save.name,
                               [](unsigned char value) { return value >= 32 && value <= 126; }) &&
           save.character >= 0 && save.character < kClassCount && save.color >= 0 &&
           save.color < kColorCount && counter(save.gold) && counter(save.levelTotal) &&
           save.helpSeen.size() <= PlayerRuntimeCheckpoint::State::kMaxRecords &&
           std::ranges::all_of(save.helpSeen, identifier) && save.moviesSeen.size() <= 4096 &&
           std::ranges::all_of(save.moviesSeen,
                               [](const std::string& value) { return value.size() <= 4096; }) &&
           std::ranges::all_of(save.classes, progressValid);
}
bool sameProgress(const ClassProgress& a, const ClassProgress& b) {
    return std::tie(a.experience, a.gold, a.promotedLevel, a.health, a.fightAdd, a.armorAdd,
                    a.magicAdd, a.speedAdd, a.crystals, a.unlocked, a.inventory, a.relics, a.levels,
                    a.lifetime.enemiesKilled, a.lifetime.generatorsDestroyed, a.lifetime.goldFound,
                    a.lifetime.playSeconds, a.legends) ==
           std::tie(b.experience, b.gold, b.promotedLevel, b.health, b.fightAdd, b.armorAdd,
                    b.magicAdd, b.speedAdd, b.crystals, b.unlocked, b.inventory, b.relics, b.levels,
                    b.lifetime.enemiesKilled, b.lifetime.generatorsDestroyed, b.lifetime.goldFound,
                    b.lifetime.playSeconds, b.legends);
}
bool sameSave(const CharacterSave& a, const CharacterSave& b) {
    return std::tie(a.name, a.character, a.color, a.classUnlock, a.gold, a.levelTotal, a.autoAttack,
                    a.helpSeen, a.moviesSeen) == std::tie(b.name, b.character, b.color,
                                                          b.classUnlock, b.gold, b.levelTotal,
                                                          b.autoAttack, b.helpSeen, b.moviesSeen) &&
           std::ranges::equal(a.classes, b.classes, sameProgress);
}
auto comboValue(const ComboState& value) {
    return std::tie(value.role, value.partner, value.grabberClass, value.riding, value.rideAsked,
                    value.ticksLeft, value.turnTicks, value.graceSeconds, value.saved);
}
bool sameOptionalStep(const std::optional<PlayerRuntime::AttackStep>& a,
                      const std::optional<PlayerRuntime::AttackStep>& b) {
    return a.has_value() == b.has_value() && (!a || (a->from == b->from && a->to == b->to));
}
bool sameResults(const std::optional<LevelResults::Checkpoint>& a,
                 const std::optional<LevelResults::Checkpoint>& b) {
    return a.has_value() == b.has_value() && (!a || std::tie(a->gold, a->experience, a->kills) ==
                                                        std::tie(b->gold, b->experience, b->kills));
}
bool deedValid(PlayerDeed value) {
    return static_cast<usize>(value) < kPlayerDeedCount;
}
} // namespace

bool PlayerRuntimeCheckpoint::State::valid() const {
    // NOLINTNEXTLINE(readability-simplify-boolean-expr): one negated list of every invalid case
    return !(player < 0 || player >= 4 || !saveValid(save) || !saveValid(entrySave) ||
             !actor.valid() || (meleeFacing && !vector(*meleeFacing)) ||
             (attackStep && (!vector(attackStep->from) || !vector(attackStep->to))) ||
             !vector(previous.position) || !scalar(previous.yaw) || !capture.valid() ||
             combo.role < ComboRole::None || combo.role > ComboRole::Thrown || combo.partner < -1 ||
             combo.partner >= 4 || combo.grabberClass < -1 || combo.grabberClass >= kClassCount ||
             !counter(combo.ticksLeft) || !counter(combo.turnTicks) ||
             !scalar(combo.graceSeconds) || !vector(combo.saved) || !transport.valid() ||
             !mikey.valid() || (figure && !figure->valid()) || (slot && *slot > 65535) ||
             (resultsCheckpoint &&
              (!counter(resultsCheckpoint->gold) || !counter(resultsCheckpoint->experience) ||
               !counter(resultsCheckpoint->kills))) ||
             !counter(levelKills) || life < PlayerLife::Standing || life > PlayerLife::InTower ||
             !scalar(healthFraction) || std::abs(healthFraction) > 1 || !scalar(painOwed) ||
             painOwed < 0 || !counter(hitSoundGap) || !counter(heartbeatTicks) ||
             (fixtureSpot && !vector(*fixtureSpot)) || !scalar(surfaceGap) ||
             !counter(hitFlashTicks) || !counter(itTicks) || !deedValid(gesture) ||
             !scalar(gagSeconds) || shockGaps.size() > kMaxRecords ||
             !std::ranges::all_of(shockGaps,
                                  [](const PlayerRuntime::ShockGap& gap) {
                                      return identifier(gap.target) && scalar(gap.seconds);
                                  }) ||
             !identifier(deathHeld) || !timer(deathDrainTicks) || !deedValid(reaction) ||
             !Knockback::valid(knockback) || !TurboMeter::valid(turbo) ||
             helpHeard.size() > kMaxRecords || !std::ranges::all_of(helpHeard, identifier) ||
             !TurboMove::valid(move) || rammed.size() > kMaxRecords ||
             !std::ranges::all_of(rammed, [](usize id) { return id <= 65535; }) ||
             !scalar(blockLeft) || !scalar(cloudGap) || !scalar(breathGap) || !scalar(effectGap) ||
             !identifier(floor.object) || (floor.placement && !matrix(*floor.placement)) ||
             !gem.valid() || !scalar(glow) || !weaponGlow.valid() || weaponGlow.effect != 0 ||
             !streak.valid() || !exitWait.valid() || !counter(nameTicks));
}

bool PlayerRuntimeCheckpoint::State::operator==(const State& other) const {
    return sameSave(save, other.save) && sameSave(entrySave, other.entrySave) &&
           sameOptionalStep(attackStep, other.attackStep) &&
           sameResults(resultsCheckpoint, other.resultsCheckpoint) &&
           comboValue(combo) == comboValue(other.combo) &&
           std::ranges::equal(shockGaps, other.shockGaps,
                              [](const auto& a, const auto& b) {
                                  return a.target == b.target && a.seconds == b.seconds;
                              }) &&
           std::tie(player, actor, cursorAiming, meleeFacing, previous.position, previous.yaw,
                    previous.continuous, previous.animationRevision, capture, transport, mikey,
                    figure, slot, levelKills, life, towerPrompt, departed, healthFraction, painOwed,
                    hitSoundGap, heartbeatTicks, fixtureSpot, surfaceGap, hitFlashTicks, itTicks,
                    gesture, gagSeconds, deathHeld, deathDrainTicks, deathHaloHeard, reaction,
                    knockback, turbo, pojoTurbo, helpHeard, move, rammed, blockLeft, blocked,
                    cloudGap, breathGap, effectGap, floor.object, floor.flags, floor.placement, gem,
                    glow, weaponGlow, streak, exitWait, nameTicks, wornSpecial) ==
               std::tie(other.player, other.actor, other.cursorAiming, other.meleeFacing,
                        other.previous.position, other.previous.yaw, other.previous.continuous,
                        other.previous.animationRevision, other.capture, other.transport,
                        other.mikey, other.figure, other.slot, other.levelKills, other.life,
                        other.towerPrompt, other.departed, other.healthFraction, other.painOwed,
                        other.hitSoundGap, other.heartbeatTicks, other.fixtureSpot,
                        other.surfaceGap, other.hitFlashTicks, other.itTicks, other.gesture,
                        other.gagSeconds, other.deathHeld, other.deathDrainTicks,
                        other.deathHaloHeard, other.reaction, other.knockback, other.turbo,
                        other.pojoTurbo, other.helpHeard, other.move, other.rammed, other.blockLeft,
                        other.blocked, other.cloudGap, other.breathGap, other.effectGap,
                        other.floor.object, other.floor.flags, other.floor.placement, other.gem,
                        other.glow, other.weaponGlow, other.streak, other.exitWait, other.nameTicks,
                        other.wornSpecial);
}

struct PlayerRuntimeCheckpoint::Prepared::Data {
    const PlayerRuntimeCheckpoint* owner = nullptr;
    std::weak_ptr<const u8> lifetime;
    u64 generation = 0;
    PlayerFigure* figure = nullptr;
    std::optional<PlayerFigure::PreparedGameplay> figureState;
    PlayerRuntime runtime;
};
PlayerRuntimeCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlayerRuntimeCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
PlayerRuntimeCheckpoint::Prepared&
PlayerRuntimeCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
PlayerRuntimeCheckpoint::Prepared::~Prepared() = default;
PlayerRuntimeCheckpoint::PlayerRuntimeCheckpoint(PlayerRuntime& runtime, const ClassStats* stats)
    : m_runtime(runtime), m_stats(stats), m_player(runtime.actor.player()),
      m_character(runtime.actor.save().character), m_color(runtime.actor.save().color) {}
PlayerRuntimeCheckpoint::~PlayerRuntimeCheckpoint() = default;

bool PlayerRuntimeCheckpoint::supported() const {
    return m_runtime.actor.player() == m_player &&
           m_runtime.actor.save().character == m_character &&
           m_runtime.actor.save().color == m_color && !m_runtime.mikeyFigure &&
           m_runtime.deathHeldEffect == 0 && m_runtime.deathHeldCry == kNoSound &&
           m_runtime.deathHeldSuck == kNoSound && m_runtime.weaponGlow.effect() == 0 &&
           (!m_runtime.figure || !m_runtime.figure->pojoActive());
}

std::optional<PlayerRuntimeCheckpoint::State> PlayerRuntimeCheckpoint::checkpoint() const {
    if (!supported()) {
        return std::nullopt;
    }
    const auto& runtime = m_runtime;
    State state;
    state.player = runtime.actor.player();
    state.save = runtime.actor.save();
    state.actor = runtime.actor.checkpoint();
    state.cursorAiming = runtime.cursorAiming;
    state.meleeFacing = runtime.meleeFacing;
    state.attackStep = runtime.attackStep;
    state.previous = runtime.previous;
    state.capture = runtime.capture.checkpoint();
    state.combo = runtime.combo;
    state.transport = runtime.transport.checkpoint();
    state.mikey = runtime.mikey.checkpoint();
    if (runtime.figure) {
        state.figure = runtime.figure->gameplayCheckpoint();
        if (!state.figure) {
            return std::nullopt;
        }
    }
    state.slot = runtime.slot;
    state.entrySave = runtime.entrySave;
    state.resultsCheckpoint = runtime.resultsCheckpoint;
    state.levelKills = runtime.levelKills;
    state.life = runtime.life;
    state.towerPrompt = runtime.towerPrompt;
    state.departed = runtime.departed;
    state.healthFraction = runtime.healthFraction;
    state.painOwed = runtime.painOwed;
    state.hitSoundGap = runtime.hitSoundGap;
    state.heartbeatTicks = runtime.heartbeatTicks;
    state.fixtureSpot = runtime.fixtureSpot;
    state.surfaceGap = runtime.surfaceGap;
    state.hitFlashTicks = runtime.hitFlashTicks;
    state.itTicks = runtime.itTicks;
    state.gesture = runtime.gesture;
    state.gagSeconds = runtime.gagSeconds;
    state.shockGaps = runtime.shockGaps;
    state.deathHeld = runtime.deathHeld;
    state.deathDrainTicks = runtime.deathDrainTicks;
    state.deathHaloHeard = runtime.deathHaloHeard;
    state.reaction = runtime.reaction;
    state.knockback = runtime.knockback.checkpoint();
    state.turbo = runtime.turbo.checkpoint();
    state.pojoTurbo = runtime.pojoTurbo;
    state.helpHeard = runtime.helpHeard;
    state.move = runtime.move.checkpoint();
    state.rammed = runtime.rammed;
    state.blockLeft = runtime.blockLeft;
    state.blocked = runtime.blocked;
    state.cloudGap = runtime.cloudGap;
    state.breathGap = runtime.breathGap;
    state.effectGap = runtime.effectGap;
    state.floor = runtime.floor;
    state.gem = runtime.gem.checkpoint();
    state.glow = runtime.glow.level();
    state.weaponGlow = runtime.weaponGlow.checkpoint();
    state.streak = runtime.streak.checkpoint();
    state.exitWait = runtime.exitWait.checkpoint();
    state.nameTicks = runtime.nameTicks;
    state.wornSpecial = runtime.wornSpecial;
    return state.valid() ? std::optional<State>{std::move(state)} : std::nullopt;
}

std::optional<PlayerRuntimeCheckpoint::Prepared>
PlayerRuntimeCheckpoint::prepareRestore(const State& state) const {
    if (!supported() || !state.valid() || state.player != m_player ||
        state.save.character != m_character || state.save.color != m_color ||
        state.figure.has_value() != (m_runtime.figure != nullptr)) {
        return std::nullopt;
    }
    auto prepared = std::make_unique<Prepared::Data>();
    prepared->owner = this;
    prepared->lifetime = m_lifetime;
    prepared->generation = m_generation;
    prepared->figure = m_runtime.figure.get();
    if (state.figure) {
        prepared->figureState = m_runtime.figure->prepareGameplayRestore(*state.figure);
        if (!prepared->figureState) {
            return std::nullopt;
        }
    }
    auto& runtime = prepared->runtime;
    runtime.actor = m_runtime.actor;
    runtime.actor.save() = state.save;
    if (!runtime.actor.restore(state.actor) || !runtime.capture.restore(state.capture) ||
        !runtime.transport.restore(state.transport) || !runtime.mikey.restore(state.mikey) ||
        !runtime.knockback.restore(state.knockback) || !runtime.turbo.restore(state.turbo) ||
        !runtime.move.restore(state.move, m_stats,
                              state.figure ? state.figure->animation.action
                                           : PlayerAnimator::Action::Ready) ||
        !runtime.gem.restore(state.gem) || !runtime.weaponGlow.restore(state.weaponGlow) ||
        !runtime.streak.restore(state.streak) || !runtime.exitWait.restore(state.exitWait)) {
        return std::nullopt;
    }
    runtime.cursorAiming = state.cursorAiming;
    runtime.meleeFacing = state.meleeFacing;
    runtime.attackStep = state.attackStep;
    runtime.previous = state.previous;
    runtime.combo = state.combo;
    runtime.slot = state.slot;
    runtime.entrySave = state.entrySave;
    runtime.resultsCheckpoint = state.resultsCheckpoint;
    runtime.levelKills = state.levelKills;
    runtime.life = state.life;
    runtime.towerPrompt = state.towerPrompt;
    runtime.departed = state.departed;
    runtime.healthFraction = state.healthFraction;
    runtime.painOwed = state.painOwed;
    runtime.hitSoundGap = state.hitSoundGap;
    runtime.heartbeatTicks = state.heartbeatTicks;
    runtime.fixtureSpot = state.fixtureSpot;
    runtime.surfaceGap = state.surfaceGap;
    runtime.hitFlashTicks = state.hitFlashTicks;
    runtime.itTicks = state.itTicks;
    runtime.gesture = state.gesture;
    runtime.gagSeconds = state.gagSeconds;
    runtime.shockGaps = state.shockGaps;
    runtime.deathHeld = state.deathHeld;
    runtime.deathDrainTicks = state.deathDrainTicks;
    runtime.deathHaloHeard = state.deathHaloHeard;
    runtime.reaction = state.reaction;
    runtime.pojoTurbo = state.pojoTurbo;
    runtime.helpHeard = state.helpHeard;
    runtime.rammed = state.rammed;
    runtime.blockLeft = state.blockLeft;
    runtime.blocked = state.blocked;
    runtime.cloudGap = state.cloudGap;
    runtime.breathGap = state.breathGap;
    runtime.effectGap = state.effectGap;
    runtime.floor = state.floor;
    runtime.glow.raise(state.glow);
    runtime.nameTicks = state.nameTicks;
    runtime.wornSpecial = state.wornSpecial;
    return Prepared{std::move(prepared)};
}

bool PlayerRuntimeCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data || prepared.m_data->owner != this ||
        prepared.m_data->lifetime.lock() != m_lifetime ||
        prepared.m_data->generation != m_generation || !supported() ||
        prepared.m_data->figure != m_runtime.figure.get()) {
        return false;
    }
    return !prepared.m_data->figureState ||
           m_runtime.figure->canCommitGameplay(*prepared.m_data->figureState);
}

// the prepared state's parts are taken in place
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
bool PlayerRuntimeCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto& data = *prepared.m_data;
    if (data.figureState &&
        !m_runtime.figure->commitGameplayRestore(std::move(*data.figureState))) {
        return false;
    }
    static_assert(std::is_nothrow_move_assignable_v<PlayerRuntime>);
    data.runtime.figure = std::move(m_runtime.figure);
    m_runtime = std::move(data.runtime);
    ++m_generation;
    prepared.m_data.reset();
    return true;
}
bool PlayerRuntimeCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

} // namespace gdl::game
