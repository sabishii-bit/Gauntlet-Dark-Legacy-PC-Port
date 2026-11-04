#include "game/bosses/BossSequence.h"

#include <algorithm>
#include <format>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/bosses/BossCoins.h"
#include "game/bosses/BossTraits.h"
#include "game/bosses/SkorneRelics.h"
#include "game/level/ImportedLevel.h"
#include "game/party/PlayerFigure.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
bool BossSequence::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("BossSequence.native.1");
    digest.fields<6>(m_sim);
    if (!m_victory.appendNativeDigest(digest)) {
        return false;
    }
    digest.flag(m_legend != nullptr);
    return m_legend ? m_legend->appendNativeDigest(digest) : digest.good();
}

bool BossSequence::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, victory, legend, owner, binding] = saved;
    digest.string("BossSequence.native.1");
    digest.fields<6>(state);
    if (!m_victory.appendLocalDigest(digest, victory)) {
        return false;
    }
    digest.flag(legend.has_value());
    return legend ? m_legend->appendLocalDigest(digest, *legend) : digest.good();
}

void BossSequence::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    saved.m_state = m_sim;
    m_victory.saveLocal(saved.m_victory);
    if (m_legend != nullptr) {
        if (!saved.m_legend) {
            saved.m_legend.emplace();
        }
        m_legend->saveLocal(*saved.m_legend);
    } else {
        saved.m_legend.reset();
    }
}

bool BossSequence::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_localBinding &&
           m_victory.canRestoreLocal(saved.m_victory) &&
           saved.m_legend.has_value() == (m_legend != nullptr) &&
           (!m_legend || m_legend->canRestoreLocal(*saved.m_legend));
}

bool BossSequence::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_victory.restoreLocal(saved.m_victory);
    if (m_legend) {
        m_legend->restoreLocal(*saved.m_legend);
    }
    m_sim = saved.m_state;
    return true;
}

namespace {
constexpr std::string_view kBossKeyTree = "BOSSKEY";
constexpr std::string_view kBossKeyLaterTree = "BOSSKEY2";
constexpr f32 kBossKeySeconds = 30.0f;
constexpr std::string_view kBossKeySoundPrefix = "S_BOSSKEY";
constexpr std::string_view kSpawnEffect = "STARTFX";
constexpr f32 kBossDeathBlast = 1000.0f;
constexpr f32 kBossDeathBlastRadius = 1000.0f;
} // namespace

void BossSequence::bind(Resources resources) {
    clear();
    m_resources.emplace(resources);
    auto* nativeItems =
        resources.world.imported() ? resources.world.imported()->bossLegendArchive() : nullptr;
    m_legend = std::make_unique<LegendPresentation>(
        resources.effects,
        LegendPresentation::Assets{
            resources.device, nativeItems ? *nativeItems : resources.world.items(),
            resources.weapons, resources.textures,
            resources.world.imported() ? resources.world.imported()->bossLegendProfile() : nullptr},
        LegendPresentation::Audio{
            [this](std::string_view name) { return m_resources->audio.playNamed(name); },
            [this](SoundHandle handle) { m_resources->audio.stop(handle); },
            [this](std::string_view name) { return m_resources->audio.hasNamed(name); },
            [this](std::string_view name, bool looping, s32 player, s32 kind,
                   u32 cue) -> std::optional<SoundCueToken> {
                auto& audio = m_resources->audio;
                if (!audio.recording()) {
                    return std::nullopt;
                }
                const auto scope =
                    audio.soundScope({14, static_cast<u64>(kind), 1, static_cast<u8>(player)}, cue);
                if (looping) {
                    return audio.beginNamedCue(name);
                }
                audio.playNamed(name);
                return SoundCueToken{};
            },
            [this](std::string_view name, s32 player, s32 kind, SoundCueToken start) {
                auto& audio = m_resources->audio;
                if (audio.recording()) {
                    audio.desireNamedLoop(
                        {{14, static_cast<u64>(kind), start.event.tick, static_cast<u8>(player)},
                         1,
                         1},
                        name, SceneSoundPlayback::PlayOnceUntilCancelled, 1, 0, start);
                }
            },
            [this](LegendSound phase) {
                auto* imported = m_resources->world.imported();
                return imported != nullptr && imported->playBossLegendSound(phase);
            }});
}

void BossSequence::gatherAudio() const {
    if (m_legend) {
        m_legend->gatherAudio();
    }
}
void BossSequence::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    if (m_resources) {
        for (const SoundHandle voice : m_victoryVoices) {
            m_resources->audio.stop(voice);
        }
    }
    m_victoryHandle = kNoSound;
    m_sim.speech = {};
    m_sim.speechCue = {};
    m_victoryVoices.clear();
    m_legend.reset(); // stops its loop while audio and effects remain available
    m_victory.clear();
    m_sim.shardPosition = Vec3{0};
    m_sim.fallenSpot = Vec3{0};
    m_sim.fallenFacing = 0.0f;
    m_resources.reset();
}

/** Translate scene-owned poses into the presentation's small, read-only snapshot. */
std::optional<LegendPresentation::Bearer>
BossSequence::bearer(s32 player, s32 kind, std::span<const PlayerRuntime> players) {
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() != player || runtime.departed || runtime.life != PlayerLife::Standing) {
            continue;
        }
        const PlayerFigure* figure = runtime.figure.get();
        LegendPresentation::Bearer bearer;
        bearer.player = player;
        bearer.color = actor.save().color;
        bearer.position = actor.position();
        bearer.facing = actor.facing();
        bearer.holdPoint = actor.position() + Vec3{0.0f, LegendShow::kHeldLift, 0.0f};
        bearer.canGesture = figure != nullptr && runtime.life == PlayerLife::Standing;
        bearer.casting = figure != nullptr && figure->animator().castingLegend();
        bearer.released = figure != nullptr && figure->animator().legendReleased();
        if (figure != nullptr) {
            const Mat4 body = PlayerFigure::bodyPlacement(
                runtime.capture.body().value_or(actor.transform()), actor.save(),
                PowerupEffects::of(actor.save().progress().inventory));
            bearer.holdTransform =
                LegendShow::heldInHand(kind)
                    ? figure->handAttachment(body)
                    : std::optional{glm::translate(figure->rootAttachment(body),
                                                   Vec3{0, LegendShow::kHeldLift, 0})};
            if (bearer.holdTransform) {
                bearer.holdPoint = Vec3{(*bearer.holdTransform)[3]};
            }
        }
        return bearer;
    }
    return std::nullopt;
}

void BossSequence::showLegend(const LegendEvent& event, const Bosses& bosses,
                              std::span<const PlayerRuntime> players) {
    if (m_legend != nullptr) {
        const s32 kind = bosses.view().kind;
        m_legend->show(event.cue, event.player, event.realm, kind,
                       bearer(event.player, kind, players));
    }
}

void BossSequence::advanceLegend(f32 seconds, Bosses& bosses, std::span<PlayerRuntime> players) {
    if (m_legend == nullptr) {
        return;
    }
    if (bosses.legend().stage() == LegendRite::Stage::Carried) {
        const s32 player = bosses.legend().player();
        const s32 kind = bosses.view().kind;
        m_legend->carry(player, bosses.legendRealm(), kind, bearer(player, kind, players));
    }
    std::optional<LegendPresentation::Target> target;
    if (const Vec3* at = bosses.position(); at != nullptr) {
        target = LegendPresentation::Target{*at, bosses.height(), bosses.rootTransform()};
        if (const auto attachment = bosses.legendTargetTransform()) {
            target->position = Vec3{(*attachment)[3]};
            target->height = 0;
            target->attachment = *attachment;
        }
        if (bosses.view().kind == 38) {
            // PlayerMotion targets hitnode1, TYPE.lookNode1 in PBOSS.WAD,
            // rather than the boss's floor-space collision centre.
            target->touches = [&bosses](const Vec3& from, const Vec3& to, f32 radius) {
                return bosses.struckBy(from, to, radius).has_value();
            };
        }
    }
    const LegendPresentation::Update result =
        m_legend->update(seconds, bearer(m_legend->player(), m_legend->kind(), players), target);
    if (result.gesture != PlayerDeed::None) {
        for (PlayerRuntime& runtime : players) {
            if (runtime.actor.player() == m_legend->player()) {
                runtime.reaction = result.gesture;
            }
        }
    }
    if (result.landed) {
        bosses.landLegend();
    }
}

/** Standing players receive the victory bit (BossDeath followed by PlayerGiveRune).
 * BossDeath only creates the floating key and its sound for the eight ordinary bosses. */
void BossSequence::fallen(const Vec3& where, const Bosses& bosses,
                          std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    auto& r = *m_resources;
    const LevelInfo* level = r.world.level();
    if (level == nullptr || m_victory.state().running() || m_victory.state().finished()) {
        return;
    }
    if (m_legend != nullptr) {
        m_legend->reset();
    }
    const s32 order = LevelRef::orderOf(r.world.ref().realmId);
    u16 found = 0;
    for (PlayerRuntime& runtime : players) {
        if (runtime.departed) {
            continue;
        }
        PlayerActor& actor = runtime.actor;
        if (runtime.life == PlayerLife::Standing) {
            actor.save().progress().relics.addShard(order);
        }
        // The narrator counts runes held by everyone still joined, including the fallen
        // waiting in the tower (PlayerHasShard's state != 0 union).
        found |= actor.save().progress().relics.runes;
    }
    // The realm's runestones are those its levels' records number, from one.
    u16 inRealm = 0;
    if (r.levels != nullptr) {
        for (const s32 rune : r.levels->runesOf(r.world.ref().realm)) {
            if (rune > 0 && rune <= Relics::kRuneCount) {
                inRealm |= static_cast<u16>(1U << static_cast<u32>(rune - 1));
            }
        }
    }
    const char letter = r.world.ref().name.empty() ? 'G' : r.world.ref().name.front();
    m_victory.begin(level->bossType, letter, inRealm, found, false);
    m_sim.shardPosition = where + bosses.rewardOffset();
    m_sim.fallenSpot = where;
    m_sim.fallenFacing = bosses.facing();
    const bool dropsKey = orderedBossTraitsOf(level->bossType).defeat.dropsKey;
    if (r.world.items().loaded()) {
        if (dropsKey) {
            EffectTrees::Setting setting;
            setting.seconds = kBossKeySeconds;
            setting.then = kBossKeyLaterTree;
            r.effects.startSet(r.device, r.world.items(), kBossKeyTree, m_sim.shardPosition,
                               setting);
        }
        std::vector<Vec3> standing;
        for (const PlayerRuntime& runtime : players) {
            if (!runtime.departed && runtime.life == PlayerLife::Standing) {
                standing.push_back(runtime.actor.position());
            }
        }
        const Vec3* boss = bosses.position();
        m_victory.bindWizard(r.device, r.world.items(), boss != nullptr ? *boss : where, standing);
    }
    if (dropsKey) {
        r.audio.playNamed(std::format("{}{}", kBossKeySoundPrefix, letter));
    }
}

/** The wizard's visit runs on: he fades in, says his piece (typed out under the view, his
 * lines from the level's bank), then the party sparkles and is taken to the tower. */
bool BossSequence::advanceVictory(s32 ticks, f32 seconds, std::span<const PlayerRuntime> players,
                                  const MessageTable& strings) {
    if (!m_resources || !m_victory.state().running()) {
        return false;
    }
    auto& r = *m_resources;
    const auto result =
        m_victory.update(ticks, seconds, r.world.goldLeft(), strings, m_sim.speech.playing());
    for (const VictoryVoice& voice : result.voices) {
        // Wizard speech is serialized even if a caption is absent or finishes early.
        const auto next =
            r.audio.speechNamed(voice.sound, {m_victoryHandle, m_sim.speech, m_sim.speechCue});
        if (next.handle != kNoSound && next.handle != m_victoryHandle) {
            m_victoryVoices.push_back(next.handle);
        }
        m_victoryHandle = next.handle;
        m_sim.speech = next.timing;
        m_sim.speechCue = next.cue;
    }
    if (result.sparkle) {
        if (r.weapons.loaded()) {
            for (const PlayerRuntime& runtime : players) {
                if (!runtime.departed && runtime.life == PlayerLife::Standing) {
                    r.effects.start(r.device, r.weapons, kSpawnEffect, runtime.actor.position());
                }
            }
        }
    }
    return m_victory.state().finished();
}

/** BossCameraUpdate's radius for the key and the fallen boss's spot. */
constexpr f32 kFallenRadius = 5.0f;

BossCameraSubject BossSequence::restSubject() const {
    BossCameraSubject subject;
    subject.position = m_sim.fallenSpot;
    subject.baseAttention = m_sim.fallenSpot;
    subject.facing = m_sim.fallenFacing;
    subject.radius = kFallenRadius;
    subject.awake = true;
    return subject;
}

BossCameraSubject BossSequence::victorySubject() const {
    if (m_victory.state().wizardShown()) {
        return m_victory.wizardSubject();
    }
    BossCameraSubject subject;
    subject.position = m_sim.shardPosition;
    subject.radius = 5.0f;
    subject.awake = true;
    subject.focus = BossCameraSubject::Focus::Shard;
    return subject;
}

/** As the death throws them out, the boss's coins for the party fly from where it stands
 * and its blast takes the rest of the level's enemies with it. BossSpewCoins starts an
 * unseen effect of 1000 damage over 1000 units that hits items too, so the arena's safe
 * rocks break and no longer keep the coins from the party. */
void BossSequence::spewCoins(const CombatSpew& spew, LevelOpponents& opponents,
                             LevelFixtures& fixtures, std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    auto& r = *m_resources;
    const SafeRocks& rocks = fixtures.safeRocks();
    for (usize rock = 0; rock < rocks.size(); ++rock) {
        if (rocks.standing(rock) &&
            glm::distance(rocks.rock(rock).position, spew.origin) <= kBossDeathBlastRadius) {
            fixtures.strikeSafeRock(rock, kBossDeathBlast);
        }
    }
    for (const s32 enemy : opponents.enemies().within(spew.origin, kBossDeathBlastRadius)) {
        const Vec3 away = opponents.enemies().positionOf(enemy) - spew.origin;
        opponents.strikeEnemy(enemy, kBossDeathBlast, EnemyHit::kKnockDown,
                              Vec3{away.x, 0.0f, away.z}, -1, players);
    }
    for (const s32 generator : opponents.generators().within(spew.origin, kBossDeathBlastRadius)) {
        opponents.strikeGenerator(generator, kBossDeathBlast, -1);
    }
    const auto* level = r.world.level();
    const BossTraits* traits = level != nullptr ? bossTraitsOf(level->bossType) : nullptr;
    switch (traits != nullptr ? traits->defeat.spew : BossSpew::Coins) {
    case BossSpew::Relics:
        for (const auto& relic : SkorneRelics::spray(spew.velocity, spew.halfAngle)) {
            r.world.throwItem(r.device, relic.name, spew.origin, relic.velocity,
                              SkorneRelics::kNoGrabSeconds, SkorneRelics::kStrength);
        }
        return;
    case BossSpew::Coins: break;
    }
    const auto count = static_cast<s32>(std::ranges::count_if(
        players, [](const PlayerRuntime& runtime) { return !runtime.departed; }));
    for (const SpewedCoin& coin : BossCoins::spray(r.world.ref().realmId, count, spew.velocity,
                                                   spew.halfAngle, m_sim.coinRandom)) {
        // BossSpewCoins writes each coin's worth over its record's (the level tables keep
        // a tenth of it), so the Fiend's four silver and one gold are 9000, not 900.
        r.world.throwItem(r.device, coin.name, spew.origin, coin.velocity,
                          BossCoins::kNoGrabSeconds, std::nullopt, coin.value);
    }
}

} // namespace gdl::game
