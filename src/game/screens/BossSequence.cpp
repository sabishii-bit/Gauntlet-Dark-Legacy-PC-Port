#include "game/screens/BossSequence.h"

#include <algorithm>
#include <format>

#include "engine/core/Types.h"

#include "game/enemies/BossCoins.h"
#include "game/enemies/SkorneRelics.h"
#include "game/players/PowerupEffects.h"
#include "game/world/PlayerFigure.h"

namespace gdl::game {
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
    m_legend = std::make_unique<LegendPresentation>(
        resources.effects,
        LegendPresentation::Assets{resources.device, resources.world.items(), resources.weapons,
                                   resources.textures},
        LegendPresentation::Audio{
            [this](std::string_view name) { return m_resources->audio.playNamed(name); },
            [this](SoundHandle handle) { m_resources->audio.stop(handle); }});
}
void BossSequence::clear() {
    if (m_resources) {
        for (const SoundHandle voice : m_victoryVoices) {
            m_resources->audio.stop(voice);
        }
    }
    m_victoryVoice = kNoSound;
    m_victoryVoices.clear();
    m_legend.reset(); // stops its loop while audio and effects remain available
    m_victory.clear();
    m_shardPosition = Vec3{0};
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
        if (bosses.view().kind == 35) {
            if (const auto lion = bosses.nodeTransform("CHIMLIONHEA")) {
                target->position = Vec3{(*lion)[3]};
                target->height = 0;
            }
        } else if (bosses.view().kind == 38) {
            // PlayerMotion targets hitnode1, TYPE.lookNode1 in PBOSS.WAD,
            // rather than the boss's floor-space collision centre.
            if (const auto eye = bosses.nodeTransform("BODY1_EYEBALL")) {
                target->position = Vec3{(*eye)[3]};
                target->height = 0;
            }
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
        m_legend->clear();
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
    m_shardPosition = where + bosses.rewardOffset();
    if (r.world.items().loaded()) {
        if (level->bossType < 42) {
            EffectTrees::Setting setting;
            setting.seconds = kBossKeySeconds;
            setting.then = kBossKeyLaterTree;
            r.effects.startSet(r.device, r.world.items(), kBossKeyTree, m_shardPosition, setting);
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
    if (level->bossType < 42) {
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
    const auto result = m_victory.update(ticks, seconds, r.world.goldLeft(), strings,
                                         r.audio.isPlaying(m_victoryVoice));
    for (const VictoryVoice& voice : result.voices) {
        // Wizard speech is serialized even if a caption is absent or finishes early.
        const SoundHandle next = r.audio.playNamed(voice.sound, 1.0f, m_victoryVoice);
        if (next != kNoSound) {
            m_victoryVoice = next;
            m_victoryVoices.push_back(next);
        }
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

BossCameraSubject BossSequence::victorySubject() const {
    if (m_victory.state().wizardShown()) {
        return m_victory.wizardSubject();
    }
    BossCameraSubject subject;
    subject.position = m_shardPosition;
    subject.radius = 5.0f;
    subject.awake = true;
    subject.focus = BossCameraSubject::Focus::Shard;
    return subject;
}

/** As the death throws them out, the boss's coins for the party fly from where it stands
 * and its blast takes the rest of the level's enemies with it. */
void BossSequence::spewCoins(const CombatSpew& spew, LevelOpponents& opponents,
                             std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    auto& r = *m_resources;
    for (const s32 enemy : opponents.enemies().within(spew.origin, kBossDeathBlastRadius)) {
        const Vec3 away = opponents.enemies().positionOf(enemy) - spew.origin;
        opponents.strikeEnemy(enemy, kBossDeathBlast, EnemyHit::kKnockDown,
                              Vec3{away.x, 0.0f, away.z}, -1, players);
    }
    for (const s32 generator : opponents.generators().within(spew.origin, kBossDeathBlastRadius)) {
        opponents.strikeGenerator(generator, kBossDeathBlast, -1);
    }
    if (const auto* level = r.world.level(); level != nullptr && level->bossType == 42) {
        for (const auto& relic : SkorneRelics::spray(spew.velocity, spew.halfAngle)) {
            r.world.throwItem(r.device, relic.name, spew.origin, relic.velocity,
                              SkorneRelics::kNoGrabSeconds, SkorneRelics::kStrength);
        }
        return;
    }
    const auto count = static_cast<s32>(std::ranges::count_if(
        players, [](const PlayerRuntime& runtime) { return !runtime.departed; }));
    for (const SpewedCoin& coin : BossCoins::spray(r.world.ref().realmId, count, spew.velocity,
                                                   spew.halfAngle, m_coinRandom)) {
        r.world.throwItem(r.device, coin.name, spew.origin, coin.velocity,
                          BossCoins::kNoGrabSeconds);
    }
}

} // namespace gdl::game
