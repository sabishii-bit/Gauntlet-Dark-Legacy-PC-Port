#include "game/screens/BossSequence.h"

#include <format>

#include "engine/core/Types.h"

#include "game/enemies/BossCoins.h"
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
    m_legend.reset(); // stops its loop while audio and effects remain available
    m_victory.clear();
    m_resources.reset();
}

/** Translate scene-owned poses into the presentation's small, read-only snapshot. */
std::optional<LegendPresentation::Bearer>
BossSequence::bearer(s32 player, s32 kind, std::span<const PlayerRuntime> players) {
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() != player) {
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
        if (LegendShow::heldInHand(kind) && figure != nullptr) {
            const f32 size = PlayerFigure::bodyScale(
                actor.save(), PowerupEffects::of(actor.save().progress().inventory));
            const Mat4 body = glm::scale(actor.transform(), Vec3{size, size, size});
            if (const auto hand = figure->handPosition(body); hand.has_value()) {
                bearer.holdPoint = *hand;
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
    std::optional<LegendPresentation::Target> target;
    if (const Vec3* at = bosses.position(); at != nullptr) {
        target = LegendPresentation::Target{*at, bosses.height(), bosses.rootTransform()};
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

/** The boss has fallen: everyone in play gets its shard, its key rises where it fell, the
 * meter goes, and the wizard's visit is set going. */
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
    const s32 order = LevelRef::orderOf(r.world.ref().realmId);
    u16 found = 0;
    for (PlayerRuntime& runtime : players) {
        PlayerActor& actor = runtime.actor;
        actor.save().progress().relics.addShard(order);
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
    if (r.world.items().loaded()) {
        EffectTrees::Setting setting;
        setting.seconds = kBossKeySeconds;
        setting.then = kBossKeyLaterTree;
        r.effects.startSet(r.device, r.world.items(), kBossKeyTree, where, setting);
        std::vector<Vec3> standing;
        for (const PlayerRuntime& runtime : players) {
            if (runtime.life == PlayerLife::Standing) {
                standing.push_back(runtime.actor.position());
            }
        }
        const Vec3* boss = bosses.position();
        m_victory.bindWizard(r.device, r.world.items(), boss != nullptr ? *boss : Vec3{0.0f},
                             standing);
    }
    r.audio.playNamed(std::format("{}{}", kBossKeySoundPrefix, letter));
}

/** The wizard's visit runs on: he fades in, says his piece (typed out under the view, his
 * lines from the level's bank), then the party sparkles and is taken to the tower. */
bool BossSequence::advanceVictory(s32 ticks, f32 seconds, std::span<const PlayerRuntime> players,
                                  const MessageTable& strings) {
    if (!m_resources || !m_victory.state().running()) {
        return false;
    }
    auto& r = *m_resources;
    const auto result = m_victory.update(ticks, seconds, r.world.goldLeft(), strings);
    for (const VictoryVoice& voice : result.voices) {
        r.audio.playNamed(voice.sound);
    }
    if (result.sparkle) {
        if (r.weapons.loaded()) {
            for (const PlayerRuntime& runtime : players) {
                if (runtime.life == PlayerLife::Standing) {
                    r.effects.start(r.device, r.weapons, kSpawnEffect, runtime.actor.position());
                }
            }
        }
    }
    return m_victory.state().finished();
}

/** As the death throws them out, the boss's coins for the party fly from where it stands
 * and its blast takes the rest of the level's enemies with it. */
void BossSequence::spewCoins(const CritterSpew& spew, LevelOpponents& opponents,
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
    const auto count = static_cast<s32>(players.size());
    for (const SpewedCoin& coin : BossCoins::spray(r.world.ref().realmId, count, spew.velocity,
                                                   spew.halfAngle, m_coinRandom)) {
        r.world.throwItem(r.device, coin.name, spew.origin, coin.velocity,
                          BossCoins::kNoGrabSeconds);
    }
}

} // namespace gdl::game
